// 实现单个、单次有效的本地闹钟；设置入口由小智 MCP 提供。
#include "alarm_services.h"

#include "alarm_replacement_policy.h"
#include "alarm_storage.h"
#include "alarm_task_wait_policy.h"
#include "audio_services.h"
#include "pomodoro_services.h"
#include "reminder_schedule.h"
#include "sensor_services.h"
#include "task_notification_target.h"
#include "ui_views.h"
#include "xiaozhi_ai.h"
#include "xiaozhi_mcp.h"

#include <atomic>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace {
constexpr int kAlarmSoundIndex = 1; // 设置页“声音选择 2”。
constexpr uint32_t kAlarmMaximumRingMs = 60U * 1000U;
constexpr uint32_t kAlarmRepeatPauseMs = 5U * 1000U;
constexpr uint32_t kAlarmAudioReleaseWaitMs = 3000U;
constexpr uint32_t kAlarmAudioReleasePollMs = 20U;
constexpr uint32_t kAlarmReplaceConfirmationTimeoutMs = 2U * 60U * 1000U;
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

portMUX_TYPE s_alarm_mux = portMUX_INITIALIZER_UNLOCKED;
AlarmSnapshot s_alarm = {false, false, 0, 0, 1};
TaskNotificationTarget s_alarm_task_target;
std::atomic<bool> s_stop_requested{false};
std::atomic<bool> s_save_pending{false};
AlarmReplacementConfirmation s_replacement_confirmation = {};

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
    portENTER_CRITICAL(&s_alarm_mux);
    clear_alarm_replacement_confirmation(&s_replacement_confirmation);
    portEXIT_CRITICAL(&s_alarm_mux);
}

AlarmReplacementDecision replacement_decision(const XiaozhiMcpAlarmRequest &request,
                                                AlarmSnapshot *existing)
{
    portENTER_CRITICAL(&s_alarm_mux);
    if (existing) {
        *existing = s_alarm;
    }
    AlarmReplacementDecision decision = evaluate_alarm_replacement(
        s_alarm.enabled,
        s_alarm.hour,
        s_alarm.minute,
        s_alarm.version,
        request.hour,
        request.minute,
        request.confirm_replace,
        pdTICKS_TO_MS(xTaskGetTickCount()),
        kAlarmReplaceConfirmationTimeoutMs,
        &s_replacement_confirmation);
    portEXIT_CRITICAL(&s_alarm_mux);
    return decision;
}

void publish_alarm_state(bool enabled, bool ringing, int hour, int minute)
{
    portENTER_CRITICAL(&s_alarm_mux);
    s_alarm.enabled = enabled;
    s_alarm.ringing = ringing;
    s_alarm.hour = static_cast<uint8_t>(hour);
    s_alarm.minute = static_cast<uint8_t>(minute);
    ++s_alarm.version;
    portEXIT_CRITICAL(&s_alarm_mux);
    (void)s_alarm_task_target.notify();
    notify_ui_task();
}

bool persist_alarm(bool enabled, int hour, int minute)
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
        (void)persist_alarm(false, 0, 0);
        publish_alarm_state(false, false, 0, 0);
        return false;
    }
    publish_alarm_state(loaded.enabled != 0, false, loaded.hour, loaded.minute);
    return true;
}

bool alarm_stop_callback()
{
    return s_stop_requested.load();
}

bool wait_interruptible(uint32_t delay_ms)
{
    TickType_t started = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(delay_ms);
    while (xTaskGetTickCount() - started < duration && !s_stop_requested.load()) {
        TickType_t remaining = duration - (xTaskGetTickCount() - started);
        TickType_t slice = remaining > pdMS_TO_TICKS(100) ? pdMS_TO_TICKS(100) : remaining;
        ulTaskNotifyTake(pdTRUE, slice);
    }
    return !s_stop_requested.load();
}

void wait_for_xiaozhi_audio_release()
{
    xiaozhi_ai_set_alarm_suspended(true);
    for (uint32_t waited = 0;
         is_audio_playing() && waited < kAlarmAudioReleaseWaitMs && !s_stop_requested.load();
         waited += kAlarmAudioReleasePollMs) {
        vTaskDelay(pdMS_TO_TICKS(kAlarmAudioReleasePollMs));
    }
}

void run_alarm_ring()
{
    s_stop_requested.store(false);
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    clear_pending_alarm_replacement();
    // 先关闭运行态，再释放小智音频后写 NVS；避免实时语音期间 Flash 写入。
    s_save_pending.store(false);
    publish_alarm_state(false, true, snapshot.hour, snapshot.minute);
    wait_for_xiaozhi_audio_release();
    if (!persist_alarm(false, snapshot.hour, snapshot.minute)) {
        ESP_LOGW(TAG, "alarm auto-disable persistence failed");
        s_save_pending.store(true);
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
    if (!alarm_flush_pending_save()) {
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
    s_stop_requested.store(true);
    publish_alarm_state(true, false, request.hour, request.minute);
    s_save_pending.store(true);
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
    s_stop_requested.store(true);
    publish_alarm_state(false, false, snapshot.hour, snapshot.minute);
    s_save_pending.store(true);
    if (result && result_len > 0) {
        strlcpy(result, kAlarmDisabledResult, result_len);
    }
    return true;
}
} // namespace

void alarm_services_init()
{
    (void)load_alarm();
    xiaozhi_mcp_register_alarm_handler(mcp_set_alarm);
    xiaozhi_mcp_register_alarm_disable_handler(mcp_disable_alarm);
}

void alarm_task(void *)
{
    s_alarm_task_target.publish(xTaskGetCurrentTaskHandle());
    for (;;) {
        AlarmSnapshot snapshot = {};
        alarm_get_snapshot(&snapshot);
        const int64_t wall_clock_ms = reminder_wall_clock_ms();
        const time_t wall_clock_seconds =
            static_cast<time_t>(wall_clock_ms / 1000);
        struct tm local = {};
        const bool time_valid = snapshot.enabled && wall_clock_ms >= 0 &&
                                localtime_r(&wall_clock_seconds, &local) != nullptr &&
                                is_tm_plausible(local);
        if (snapshot.enabled && !snapshot.ringing && time_valid &&
            local.tm_hour == snapshot.hour && local.tm_min == snapshot.minute) {
            run_alarm_ring();
        }
        const uint32_t wait_ms = alarm_task_wait_ms(snapshot.enabled,
                                                    time_valid,
                                                    wall_clock_ms);
        TickType_t wait_ticks = wait_ms > 0 ? pdMS_TO_TICKS(wait_ms) : portMAX_DELAY;
        if (wait_ms > 0 && wait_ticks == 0) {
            wait_ticks = 1;
        }
        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

void alarm_get_snapshot(AlarmSnapshot *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_alarm_mux);
    *out = s_alarm;
    portEXIT_CRITICAL(&s_alarm_mux);
}

bool alarm_is_enabled()
{
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    return snapshot.enabled;
}

uint32_t alarm_state_version()
{
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    return snapshot.version;
}

bool alarm_set_once(int hour, int minute)
{
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    if (snapshot.ringing || !alarm_time_valid(hour, minute) ||
        conflicts_with_running_pomodoro(hour, minute) ||
        !persist_alarm(true, hour, minute)) {
        return false;
    }
    clear_pending_alarm_replacement();
    s_save_pending.store(false);
    s_stop_requested.store(true);
    publish_alarm_state(true, false, hour, minute);
    ESP_LOGI(TAG, "alarm set %02d:%02d single-use", hour, minute);
    return true;
}

bool alarm_disable()
{
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    clear_pending_alarm_replacement();
    s_stop_requested.store(true);
    if (!persist_alarm(false, snapshot.hour, snapshot.minute)) {
        return false;
    }
    s_save_pending.store(false);
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
    s_stop_requested.store(true);
    (void)s_alarm_task_target.notify();
    return true;
}

void alarm_notify_time_changed()
{
    (void)s_alarm_task_target.notify();
}

bool alarm_clear_saved_state()
{
    clear_pending_alarm_replacement();
    s_stop_requested.store(true);
    s_save_pending.store(false);
    alarm_storage::ClearResult result = alarm_storage::clear();
    if (result.status == alarm_storage::ClearStatus::kAlreadyEmpty) {
        publish_alarm_state(false, false, 0, 0);
        return true;
    }
    if (result.status == alarm_storage::ClearStatus::kOpenFailed) {
        ESP_LOGW(TAG, "alarm NVS clear open failed: %s", esp_err_to_name(result.error));
        return false;
    }
    if (result.status != alarm_storage::ClearStatus::kCleared) {
        ESP_LOGW(TAG, "alarm NVS clear failed: %s", esp_err_to_name(result.error));
        return false;
    }
    publish_alarm_state(false, false, 0, 0);
    return true;
}

bool alarm_save_pending()
{
    return s_save_pending.load();
}

bool alarm_flush_pending_save()
{
    if (!s_save_pending.load()) {
        return true;
    }
    AlarmSnapshot snapshot = {};
    alarm_get_snapshot(&snapshot);
    if (!persist_alarm(snapshot.enabled, snapshot.hour, snapshot.minute)) {
        return false;
    }
    s_save_pending.store(false);
    return true;
}
