// 实现单个、单次有效的本地闹钟；设置入口由小智 MCP 提供。
#include "alarm_services_internal.h"

#include "alarm_replacement_policy.h"
#include "alarm_runtime_state_internal.h"
#include "alarm_storage.h"
#include "alarm_task_wait_policy.h"
#include "app_constexpr.h"
#include "app_metadata.h"
#include "app_task_readiness.h"
#include "app_tick_time.h"
#include "audio_services.h"
#include "pomodoro_services.h"
#include "reminder_schedule.h"
#include "runtime_health.h"
#include "scoped_semaphore_lock.h"
#include "sensor_time.h"
#include "task_notification_target.h"
#include "ui_task_notify.h"
#include "xiaozhi_ai.h"
#include "xiaozhi_mcp.h"

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstring>

#include <esp_log.h>

namespace {
constexpr int kAlarmSoundIndex = 1; // 设置页“声音选择 2”。
constexpr uint32_t kAlarmMaximumRingMs = 60U * 1000U;
constexpr uint32_t kAlarmRepeatPauseMs = 5U * 1000U;
constexpr uint32_t kAlarmAudioReleaseWaitMs = 3000U;
constexpr uint32_t kAlarmAudioReleasePollMs = 20U;
constexpr uint32_t kAlarmReplaceConfirmationTimeoutMs = 2U * 60U * 1000U;
constexpr uint8_t kAlarmPendingSaveCatchUpLimit = 3;
constexpr const char *kAlarmSetResultFormat =
    "{\"enabled\":true,\"hour\":%d,\"minute\":%d,\"single_use\":true}";
constexpr const char *kAlarmDisabledResult =
    "{\"enabled\":false,\"single_use\":true}";
constexpr const char *kAlarmPomodoroConflictResult =
    "alarm rejected: active pomodoro ends in the same minute";
constexpr const char *kAlarmReplaceConfirmationFormat =
    "{\"confirmation_required\":true,\"existing\":\"%02d:%02d\",\"requested\":\"%02d:%02d\",\"message\":\"已有闹钟，是否覆盖？\"}";
constexpr const char *kAlarmReplaceConfirmationInvalidResult =
    "alarm replacement confirmation invalid or expired; ask the user again";

TaskNotificationTarget s_alarm_task_target;
StaticTaskMutex s_alarm_persistence_mutex;
std::atomic<bool> s_stop_requested{false};
std::atomic<bool> s_save_retry_pending{false};
std::atomic<bool> s_auto_disable_save_pending{false};

bool conflicts_with_running_pomodoro(int hour, int minute)
{
    if (!is_system_time_plausible()) {
        return false;
    }
    PomodoroSnapshot pomodoro = {};
    pomodoro_get_snapshot(&pomodoro);
    return pomodoro.state == kPomodoroRunning &&
           reminder_targets_same_local_minute(reminder_wall_clock_ms(),
                                              hour,
                                              minute,
                                              pomodoro.remaining_ms);
}

void clear_pending_alarm_replacement()
{
    (void)alarm_runtime_clear_replacement();
}

AlarmReplacementDecision replacement_decision(const XiaozhiMcpAlarmRequest &request,
                                                AlarmSnapshot *existing)
{
    return alarm_runtime_replacement_decision(
        request.hour,
        request.minute,
        request.confirm_replace,
        pdTICKS_TO_MS(xTaskGetTickCount()),
        kAlarmReplaceConfirmationTimeoutMs,
        existing);
}

void publish_alarm_state(bool enabled, bool ringing, int hour, int minute)
{
    if (alarm_runtime_publish(enabled, ringing, hour, minute)) {
        (void)s_alarm_task_target.notify();
        notify_ui_task();
    }
}

void publish_alarm_state_for_deferred_save(bool enabled,
                                           bool ringing,
                                           int hour,
                                           int minute)
{
    if (alarm_runtime_publish_deferred_save(
            enabled, ringing, hour, minute)) {
        (void)s_alarm_task_target.notify();
        notify_ui_task();
    }
}

bool persist_alarm_unlocked(bool enabled, int hour, int minute)
{
    alarm_storage::WriteResult result = alarm_storage::write(
        enabled, static_cast<uint8_t>(hour), static_cast<uint8_t>(minute));
    if (result.status == alarm_storage::WriteStatus::kOpenFailed) {
        ESP_LOGW(TAG, "alarm NVS open failed: %s", esp_err_to_name(result.error));
        return false;
    }
    if (result.status != alarm_storage::WriteStatus::kSaved) {
        ESP_LOGW(TAG, "alarm NVS save failed: %s", esp_err_to_name(result.error));
        return false;
    }
    return true;
}

bool persist_alarm(bool enabled, int hour, int minute)
{
    ScopedSemaphoreLock lock(s_alarm_persistence_mutex);
    if (!lock) {
        ESP_LOGW(TAG, "alarm persistence lock unavailable");
        return false;
    }
    return persist_alarm_unlocked(enabled, hour, minute);
}

alarm_storage::ClearResult clear_alarm_storage()
{
    ScopedSemaphoreLock lock(s_alarm_persistence_mutex);
    if (!lock) {
        return {alarm_storage::ClearStatus::kOpenFailed,
                ESP_ERR_INVALID_STATE};
    }
    return alarm_storage::clear();
}

bool load_alarm()
{
    alarm_storage::ReadResult loaded = alarm_storage::read();
    if (loaded.status == alarm_storage::ReadStatus::kEmpty) {
        return true;
    }
    if (loaded.status == alarm_storage::ReadStatus::kOpenFailed) {
        ESP_LOGW(TAG, "alarm NVS load open failed: %s", esp_err_to_name(loaded.error));
        return false;
    }
    if (loaded.status != alarm_storage::ReadStatus::kLoaded ||
        loaded.enabled > 1 || !alarm_time_valid(loaded.hour, loaded.minute)) {
        ESP_LOGW(TAG, "alarm NVS state invalid, disabling alarm");
        if (!persist_alarm(false, 0, 0)) {
            s_save_retry_pending.store(true, std::memory_order_release);
            publish_alarm_state_for_deferred_save(false, false, 0, 0);
        } else {
            publish_alarm_state(false, false, 0, 0);
        }
        return false;
    }
    publish_alarm_state(loaded.enabled != 0, false, loaded.hour, loaded.minute);
    return true;
}

bool alarm_stop_callback()
{
    return s_stop_requested.load();
}

void mark_alarm_stop_requested()
{
    s_stop_requested.store(true);
}

void wake_alarm_task()
{
    (void)s_alarm_task_target.notify();
}

void request_alarm_save_retry(bool schedule_retry)
{
    if (!schedule_retry) {
        return;
    }
    s_save_retry_pending.store(true, std::memory_order_release);
    wake_alarm_task();
}

bool flush_alarm_pending_save(bool schedule_retry)
{
    ScopedSemaphoreLock persistence_lock(s_alarm_persistence_mutex);
    if (!persistence_lock) {
        request_alarm_save_retry(schedule_retry);
        return false;
    }
    // A newer MCP request may arrive during NVS I/O. Catch it up while the
    // persistence transaction is still serialized, then fall back to retry.
    for (uint8_t attempt = 0;
         attempt < kAlarmPendingSaveCatchUpLimit;
         ++attempt) {
        AlarmPendingSaveSnapshot pending = {};
        if (!alarm_runtime_pending_save_snapshot(&pending)) {
            request_alarm_save_retry(schedule_retry);
            return false;
        }
        if (!pending.pending) {
            s_save_retry_pending.store(false, std::memory_order_release);
            return true;
        }
        if (!persist_alarm_unlocked(pending.enabled,
                                    pending.hour,
                                    pending.minute)) {
            request_alarm_save_retry(schedule_retry);
            return false;
        }
        if (alarm_runtime_pending_save_clear(pending.generation)) {
            s_save_retry_pending.store(false, std::memory_order_release);
            return true;
        }
    }
    request_alarm_save_retry(schedule_retry);
    return false;
}

void request_alarm_stop()
{
    mark_alarm_stop_requested();
    wake_alarm_task();
}

bool wait_interruptible(uint32_t delay_ms)
{
    TickType_t started = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(delay_ms);
    while (!s_stop_requested.load()) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= duration) {
            break;
        }
        const TickType_t remaining = duration - elapsed;
        ulTaskNotifyTake(pdTRUE, remaining);
    }
    return !s_stop_requested.load();
}

void wait_for_xiaozhi_audio_release()
{
    xiaozhi_ai_set_alarm_suspended(true);
    (void)wait_for_audio_playback_idle(kAlarmAudioReleaseWaitMs,
                                       kAlarmAudioReleasePollMs,
                                       alarm_stop_callback);
}

bool flush_alarm_auto_disable_save()
{
    if (!s_auto_disable_save_pending.load(std::memory_order_acquire)) {
        return true;
    }
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    if (snapshot.enabled || snapshot.ringing) {
        // 新闹钟状态已经覆盖旧的单次自动关闭待办。
        s_auto_disable_save_pending.store(false, std::memory_order_release);
        return true;
    }
    if (!persist_alarm(false, snapshot.hour, snapshot.minute)) {
        return false;
    }
    s_auto_disable_save_pending.store(false, std::memory_order_release);
    return true;
}

void schedule_alarm_save_retry(TickType_t now,
                               uint8_t failure_count,
                               TickType_t *deadline)
{
    if (!deadline) {
        return;
    }
    const TickType_t delay_ticks = app_tick_nonzero_delay(
        pdMS_TO_TICKS(alarm_save_retry_delay_ms(failure_count)));
    *deadline = now + delay_ticks;
}

void run_alarm_ring()
{
    s_stop_requested.store(false);
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    clear_pending_alarm_replacement();
    // 先关闭运行态，再释放小智音频后写 NVS；避免实时语音期间 Flash 写入。
    (void)alarm_runtime_pending_save_discard();
    s_save_retry_pending.store(false, std::memory_order_release);
    s_auto_disable_save_pending.store(false, std::memory_order_release);
    publish_alarm_state(false, true, snapshot.hour, snapshot.minute);
    wait_for_xiaozhi_audio_release();
    if (!persist_alarm(false, snapshot.hour, snapshot.minute)) {
        ESP_LOGW(TAG, "alarm auto-disable persistence failed");
        s_auto_disable_save_pending.store(true, std::memory_order_release);
    }

    TickType_t started = xTaskGetTickCount();
    TickType_t maximum_duration = pdMS_TO_TICKS(kAlarmMaximumRingMs);
    while (!s_stop_requested.load() && xTaskGetTickCount() - started < maximum_duration) {
        if (play_chime_sound_blocking(kAlarmSoundIndex, alarm_stop_callback)) {
            if (!wait_interruptible(kAlarmRepeatPauseMs)) {
                break;
            }
        } else {
            // 其他短提示音正在占用 Codec 时稍后重试，但总时长仍受 1 分钟限制。
            if (!wait_interruptible(250)) {
                break;
            }
        }
    }
    xiaozhi_ai_set_alarm_suspended(false);
    publish_alarm_state(false, false, snapshot.hour, snapshot.minute);
    if (!flush_alarm_auto_disable_save()) {
        ESP_LOGW(TAG, "alarm deferred auto-disable save failed");
    }
    ESP_LOGI(TAG, "alarm finished stopped=%d", s_stop_requested.load() ? 1 : 0);
}

bool mcp_set_alarm(const XiaozhiMcpAlarmRequest &request, char *result, size_t result_len)
{
    AlarmSnapshot snapshot = {};
    if (!alarm_time_valid(request.hour, request.minute)) {
        if (result && result_len > 0) {
            strlcpy(result, "alarm rejected", result_len);
        }
        return false;
    }
    alarm_get_snapshot(&snapshot);
    if (snapshot.ringing) {
        clear_pending_alarm_replacement();
        if (result && result_len > 0) {
            strlcpy(result, "alarm rejected", result_len);
        }
        return false;
    }
    if (conflicts_with_running_pomodoro(request.hour, request.minute)) {
        clear_pending_alarm_replacement();
        ESP_LOGW(TAG, "alarm rejected by active pomodoro minute conflict");
        if (result && result_len > 0) {
            strlcpy(result, kAlarmPomodoroConflictResult, result_len);
        }
        return false;
    }
    AlarmReplacementDecision replace = replacement_decision(request, &snapshot);
    if (replace == kAlarmReplacementConfirmationRequired) {
        ESP_LOGI(TAG,
                 "alarm replacement confirmation requested existing=%02u:%02u requested=%02d:%02d",
                 static_cast<unsigned>(snapshot.hour),
                 static_cast<unsigned>(snapshot.minute),
                 request.hour,
                 request.minute);
        if (result && result_len > 0) {
            snprintf(result,
                     result_len,
                     kAlarmReplaceConfirmationFormat,
                     snapshot.hour,
                     snapshot.minute,
                     request.hour,
                     request.minute);
        }
        return false;
    }
    if (replace == kAlarmReplacementConfirmationInvalid) {
        ESP_LOGW(TAG, "alarm replacement confirmation invalid or expired");
        if (result && result_len > 0) {
            strlcpy(result, kAlarmReplaceConfirmationInvalidResult, result_len);
        }
        return false;
    }
    if (snapshot.enabled &&
        snapshot.hour == request.hour && snapshot.minute == request.minute) {
        if (result && result_len > 0) {
            snprintf(result, result_len, kAlarmSetResultFormat, request.hour, request.minute);
        }
        return true;
    }
    if (snapshot.enabled) {
        ESP_LOGI(TAG,
                 "alarm replacement confirmed existing=%02u:%02u requested=%02d:%02d",
                 static_cast<unsigned>(snapshot.hour),
                 static_cast<unsigned>(snapshot.minute),
                 request.hour,
                 request.minute);
    }
    mark_alarm_stop_requested();
    s_auto_disable_save_pending.store(false, std::memory_order_release);
    s_save_retry_pending.store(false, std::memory_order_release);
    publish_alarm_state_for_deferred_save(
        true, false, request.hour, request.minute);
    if (result && result_len > 0) {
        snprintf(result, result_len, kAlarmSetResultFormat, request.hour, request.minute);
    }
    return true;
}

bool mcp_disable_alarm(char *result, size_t result_len)
{
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    clear_pending_alarm_replacement();
    mark_alarm_stop_requested();
    s_auto_disable_save_pending.store(false, std::memory_order_release);
    s_save_retry_pending.store(false, std::memory_order_release);
    publish_alarm_state_for_deferred_save(
        false, false, snapshot.hour, snapshot.minute);
    if (result && result_len > 0) {
        strlcpy(result, kAlarmDisabledResult, result_len);
    }
    return true;
}
} // namespace

bool alarm_services_init()
{
    if (!alarm_runtime_state_init() ||
        !s_alarm_persistence_mutex.init()) {
        return false;
    }
    (void)load_alarm();
    xiaozhi_mcp_register_alarm_handler(mcp_set_alarm);
    xiaozhi_mcp_register_alarm_disable_handler(mcp_disable_alarm);
    return true;
}

void alarm_task(void *)
{
    s_alarm_task_target.publish(xTaskGetCurrentTaskHandle());
    regular_app_task_mark_ready(RegularAppTaskId::kAlarm);
    uint8_t deferred_save_failures = 0;
    TickType_t deferred_save_retry_at = 0;
    bool deferred_save_retry_scheduled = false;
    for (;;) {
        runtime_health_record_current_task_stack(RegularAppTaskId::kAlarm);
        const bool save_retry_pending =
            s_save_retry_pending.load(std::memory_order_acquire);
        const bool auto_disable_save_pending =
            s_auto_disable_save_pending.load(std::memory_order_acquire);
        const bool deferred_save_pending =
            save_retry_pending || auto_disable_save_pending;
        AlarmSnapshot snapshot = {};
        alarm_get_snapshot(&snapshot);
        if (!snapshot.enabled) {
            if (!deferred_save_pending) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
        }

        const int64_t wall_clock_ms =
            snapshot.enabled ? reminder_wall_clock_ms() : -1;
        const time_t wall_clock_seconds =
            static_cast<time_t>(wall_clock_ms / 1000);
        struct tm local = {};
        const bool time_valid = snapshot.enabled && wall_clock_ms >= 0 &&
                                localtime_r(&wall_clock_seconds, &local) != nullptr &&
                                is_tm_plausible(local);
        if (snapshot.enabled && !snapshot.ringing && time_valid &&
            local.tm_hour == snapshot.hour && local.tm_min == snapshot.minute) {
            run_alarm_ring();
            continue;
        }
        const int current_millisecond = wall_clock_ms >= 0
                                            ? static_cast<int>(wall_clock_ms % 1000)
                                            : -1;
        const uint32_t alarm_wait_ms = alarm_task_wait_ms(snapshot.enabled,
                                                          time_valid,
                                                          local.tm_hour,
                                                          local.tm_min,
                                                          local.tm_sec,
                                                          current_millisecond,
                                                          snapshot.hour,
                                                          snapshot.minute);
        const TickType_t alarm_wait_ticks =
            alarm_wait_ms > 0
                ? app_tick_nonzero_delay(pdMS_TO_TICKS(alarm_wait_ms))
                : portMAX_DELAY;

        if (deferred_save_pending) {
            TickType_t now = xTaskGetTickCount();
            if (!deferred_save_retry_scheduled) {
                deferred_save_failures = 1;
                schedule_alarm_save_retry(
                    now, deferred_save_failures, &deferred_save_retry_at);
                deferred_save_retry_scheduled = true;
            }
            TickType_t retry_wait =
                app_tick_deadline_remaining(now, deferred_save_retry_at);
            if (alarm_wait_ticks < retry_wait) {
                retry_wait = alarm_wait_ticks;
            }
            if (retry_wait > 0) {
                ulTaskNotifyTake(pdTRUE, retry_wait);
                continue;
            }
            const bool recovered = save_retry_pending
                                       ? flush_alarm_pending_save(false)
                                       : flush_alarm_auto_disable_save();
            if (recovered) {
                ESP_LOGI(TAG,
                         "alarm deferred %s save recovered",
                         save_retry_pending ? "state" : "auto-disable");
                deferred_save_failures = 0;
                deferred_save_retry_at = 0;
                deferred_save_retry_scheduled = false;
            } else {
                deferred_save_failures =
                    saturating_increment_u8(deferred_save_failures);
                schedule_alarm_save_retry(
                    xTaskGetTickCount(),
                    deferred_save_failures,
                    &deferred_save_retry_at);
                ESP_LOGW(TAG,
                         "alarm deferred %s save retry=%u delay_ms=%" PRIu32,
                         save_retry_pending ? "state" : "auto-disable",
                         static_cast<unsigned>(deferred_save_failures),
                         alarm_save_retry_delay_ms(deferred_save_failures));
            }
            continue;
        }
        deferred_save_failures = 0;
        deferred_save_retry_at = 0;
        deferred_save_retry_scheduled = false;
        ulTaskNotifyTake(pdTRUE, alarm_wait_ticks);
    }
}

void alarm_get_snapshot(AlarmSnapshot *out)
{
    if (!out) {
        return;
    }
    if (!alarm_runtime_snapshot(out)) {
        *out = {};
    }
}

bool alarm_is_enabled()
{
    return alarm_runtime_is_enabled();
}

bool alarm_disable()
{
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    clear_pending_alarm_replacement();
    mark_alarm_stop_requested();
    if (!persist_alarm(false, snapshot.hour, snapshot.minute)) {
        wake_alarm_task();
        return false;
    }
    (void)alarm_runtime_pending_save_discard();
    s_save_retry_pending.store(false, std::memory_order_release);
    s_auto_disable_save_pending.store(false, std::memory_order_release);
    publish_alarm_state(false, false, snapshot.hour, snapshot.minute);
    ESP_LOGI(TAG, "alarm disabled");
    return true;
}

bool alarm_stop_ringing_from_button()
{
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    if (!snapshot.ringing) {
        return false;
    }
    request_alarm_stop();
    return true;
}

void alarm_notify_time_changed()
{
    (void)s_alarm_task_target.notify();
}

bool alarm_clear_saved_state()
{
    clear_pending_alarm_replacement();
    mark_alarm_stop_requested();
    (void)alarm_runtime_pending_save_discard();
    s_save_retry_pending.store(false, std::memory_order_release);
    alarm_storage::ClearResult result = clear_alarm_storage();
    if (result.status == alarm_storage::ClearStatus::kAlreadyEmpty) {
        s_auto_disable_save_pending.store(false, std::memory_order_release);
        publish_alarm_state(false, false, 0, 0);
        return true;
    }
    if (result.status == alarm_storage::ClearStatus::kOpenFailed) {
        ESP_LOGW(TAG, "alarm NVS clear open failed: %s", esp_err_to_name(result.error));
        wake_alarm_task();
        return false;
    }
    if (result.status != alarm_storage::ClearStatus::kCleared) {
        ESP_LOGW(TAG, "alarm NVS clear failed: %s", esp_err_to_name(result.error));
        wake_alarm_task();
        return false;
    }
    s_auto_disable_save_pending.store(false, std::memory_order_release);
    publish_alarm_state(false, false, 0, 0);
    return true;
}

bool alarm_save_pending()
{
    return alarm_runtime_pending_save_exists();
}

bool alarm_flush_pending_save()
{
    return flush_alarm_pending_save(true);
}
