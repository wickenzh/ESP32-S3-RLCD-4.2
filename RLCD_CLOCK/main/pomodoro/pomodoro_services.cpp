// 实现不依赖墙钟时间的番茄钟，并在完成后安全播放两次声音选择 4。
#include "pomodoro_services_internal.h"

#include "alarm_services.h"
#include "app_metadata.h"
#include "app_task_readiness.h"
#include "app_tick_time.h"
#include "audio_services.h"
#include "battery_runtime_state.h"
#include "ota_runtime_state.h"
#include "pomodoro_runtime_state_internal.h"
#include "reminder_schedule.h"
#include "runtime_health.h"
#include "sensor_time.h"
#include "task_notification_target.h"
#include "ui_task_notify.h"
#include "wifi_portal_state.h"
#include "xiaozhi_ai.h"
#include "xiaozhi_mcp.h"

#include <atomic>
#include <cstdio>

#include <esp_log.h>
#include "esp_timer.h"

namespace {
constexpr uint32_t kDefaultDurationSeconds = 25U * 60U;
constexpr uint32_t kMaximumDurationSeconds = 99U * 60U + 59U;
constexpr uint32_t kCompletedHoldMs = 60U * 1000U;
constexpr int kCompletionSoundIndex = 3; // 设置页“声音选择 4”。
constexpr int kCompletionSoundRepeats = 2;
constexpr uint32_t kAudioReleaseWaitMs = 3000U;
constexpr uint32_t kAudioReleasePollMs = 20U;
constexpr const char *kPomodoroAlarmConflictResult =
    "pomodoro rejected: alarm is set for the same minute";

TaskNotificationTarget s_task_target;
std::atomic<bool> s_stop_alert_requested{false};

void notify_state_changed()
{
    (void)s_task_target.notify();
    notify_ui_task();
}

void publish_state(PomodoroState state,
                   uint32_t total_ms,
                   uint32_t remaining_ms,
                   bool alerting,
                   int64_t deadline_us,
                   int64_t completed_at_us)
{
    if (pomodoro_runtime_publish(state,
                                 total_ms,
                                 remaining_ms,
                                 alerting,
                                 deadline_us,
                                 completed_at_us)) {
        notify_state_changed();
    }
}

bool completion_stop_requested()
{
    return s_stop_alert_requested.load();
}

bool completion_audio_blocked()
{
    AlarmSnapshot alarm = {};
    alarm_get_snapshot(&alarm);
    int ota_state = ota_runtime_state_load();
    return battery_low_mode_load() ||
           setup_portal_active_load() ||
           ota_state == kOtaChecking ||
           ota_state == kOtaUpdating ||
           ota_state == kOtaSucceeded ||
           alarm.ringing;
}

bool conflicts_with_enabled_alarm(uint32_t duration_ms)
{
    if (!is_system_time_plausible()) {
        return false;
    }
    AlarmSnapshot alarm = {};
    alarm_get_snapshot(&alarm);
    return alarm.enabled &&
           reminder_targets_same_local_minute(reminder_wall_clock_ms(),
                                              alarm.hour,
                                              alarm.minute,
                                              duration_ms);
}

void set_alerting(bool alerting)
{
    if (pomodoro_runtime_set_alerting(alerting)) {
        notify_ui_task();
    }
}

void play_completion_audio()
{
    if (completion_audio_blocked()) {
        ESP_LOGI(TAG, "pomodoro completion audio skipped by system state");
        return;
    }
    s_stop_alert_requested.store(false);
    set_alerting(true);
    xiaozhi_ai_set_pomodoro_audio_suspended(true);
    const bool audio_idle =
        wait_for_audio_playback_idle(kAudioReleaseWaitMs,
                                     kAudioReleasePollMs,
                                     completion_stop_requested);
    if (!s_stop_alert_requested.load() && audio_idle) {
        (void)play_chime_sound_repeated_blocking(kCompletionSoundIndex,
                                                 kCompletionSoundRepeats,
                                                 completion_stop_requested);
    }
    xiaozhi_ai_set_pomodoro_audio_suspended(false);
    set_alerting(false);
}

bool mcp_control_pomodoro(const XiaozhiMcpPomodoroRequest &request,
                          char *result,
                          size_t result_len)
{
    bool ok = false;
    if (request.action == kXiaozhiMcpPomodoroStart) {
        uint32_t duration = request.has_duration_seconds
                                ? request.duration_seconds
                                : kDefaultDurationSeconds;
        if (duration == 0 || duration > kMaximumDurationSeconds) {
            return false;
        }
        uint32_t duration_ms = duration * 1000U;
        if (conflicts_with_enabled_alarm(duration_ms)) {
            ESP_LOGW(TAG, "pomodoro rejected by alarm minute conflict");
            if (result && result_len > 0) {
                std::snprintf(result, result_len, "%s", kPomodoroAlarmConflictResult);
            }
            return false;
        }
        ok = pomodoro_start(duration);
    } else if (request.action == kXiaozhiMcpPomodoroCancel) {
        ok = pomodoro_cancel();
    } else if (request.action == kXiaozhiMcpPomodoroStatus) {
        ok = true;
    }
    PomodoroSnapshot snapshot = {};
    pomodoro_get_snapshot(&snapshot);
    if (result && result_len > 0) {
        const char *state = snapshot.state == kPomodoroRunning
                                ? "running"
                                : (snapshot.state == kPomodoroCompleted ? "completed" : "idle");
        std::snprintf(result,
                      result_len,
                      "{\"state\":\"%s\",\"remaining_ms\":%u,\"total_ms\":%u}",
                      state,
                      static_cast<unsigned>(snapshot.remaining_ms),
                      static_cast<unsigned>(snapshot.total_ms));
    }
    return ok;
}
} // namespace

bool pomodoro_services_init()
{
    if (!pomodoro_runtime_state_init()) {
        return false;
    }
    xiaozhi_mcp_register_pomodoro_handler(mcp_control_pomodoro);
    return true;
}

void pomodoro_task(void *)
{
    s_task_target.publish(xTaskGetCurrentTaskHandle());
    regular_app_task_mark_ready(RegularAppTaskId::kPomodoro);
    for (;;) {
        runtime_health_record_current_task_stack(
            RegularAppTaskId::kPomodoro);
        const int64_t now_us = esp_timer_get_time();
        PomodoroRuntimeSnapshot runtime = {};
        if (!pomodoro_runtime_snapshot(now_us, &runtime)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const PomodoroSnapshot &snapshot = runtime.visible;
        if (snapshot.state == kPomodoroRunning && snapshot.remaining_ms == 0) {
            publish_state(kPomodoroCompleted,
                          snapshot.total_ms,
                          0,
                          false,
                          0,
                          now_us);
            ESP_LOGI(TAG, "pomodoro completed duration_ms=%u", static_cast<unsigned>(snapshot.total_ms));
            play_completion_audio();
            continue;
        }
        if (snapshot.state == kPomodoroCompleted) {
            int64_t hold_remaining_us = runtime.completed_at_us +
                                        static_cast<int64_t>(kCompletedHoldMs) * 1000 - now_us;
            if (hold_remaining_us <= 0) {
                publish_state(kPomodoroIdle, 0, 0, false, 0, 0);
                continue;
            }
            const TickType_t wait_ticks = app_tick_nonzero_delay(
                pdMS_TO_TICKS(static_cast<uint32_t>(
                    (hold_remaining_us + 999) / 1000)));
            ulTaskNotifyTake(pdTRUE, wait_ticks);
            continue;
        }
        if (snapshot.state == kPomodoroRunning) {
            const TickType_t wait_ticks =
                app_tick_nonzero_delay(pdMS_TO_TICKS(snapshot.remaining_ms));
            ulTaskNotifyTake(pdTRUE, wait_ticks);
        } else {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

void pomodoro_get_snapshot(PomodoroSnapshot *out)
{
    if (!out) {
        return;
    }
    PomodoroRuntimeSnapshot runtime = {};
    if (!pomodoro_runtime_snapshot(esp_timer_get_time(), &runtime)) {
        *out = {};
        return;
    }
    *out = runtime.visible;
}

bool pomodoro_is_running()
{
    return pomodoro_runtime_state_load() == kPomodoroRunning;
}

bool pomodoro_start(uint32_t duration_seconds)
{
    if (duration_seconds == 0 || duration_seconds > kMaximumDurationSeconds) {
        return false;
    }
    uint32_t duration_ms = duration_seconds * 1000U;
    if (conflicts_with_enabled_alarm(duration_ms)) {
        return false;
    }
    int64_t now_us = esp_timer_get_time();
    s_stop_alert_requested.store(true);
    publish_state(kPomodoroRunning,
                  duration_ms,
                  duration_ms,
                  false,
                  now_us + static_cast<int64_t>(duration_ms) * 1000,
                  0);
    ESP_LOGI(TAG, "pomodoro started duration=%u seconds", static_cast<unsigned>(duration_seconds));
    return true;
}

bool pomodoro_cancel()
{
    s_stop_alert_requested.store(true);
    publish_state(kPomodoroIdle, 0, 0, false, 0, 0);
    ESP_LOGI(TAG, "pomodoro cancelled");
    return true;
}

bool pomodoro_stop_alert_from_button()
{
    PomodoroSnapshot snapshot = {};
    pomodoro_get_snapshot(&snapshot);
    if (!snapshot.alerting) {
        return false;
    }
    s_stop_alert_requested.store(true);
    (void)s_task_target.notify();
    return true;
}
