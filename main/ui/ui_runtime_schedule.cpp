// 编排 UI 运行期超时、辅助页门控、唤醒间隔和小智自动返回。
#include "ui_runtime_schedule.h"

#include "app_constexpr.h"
#include "app_state.h"
#include "app_tick_time.h"
#include "battery_runtime_state.h"
#include "network_diagnostics_state.h"
#include "pomodoro_services.h"
#include "sensor_time.h"
#include "ui_info_page_state.h"
#include "ui_loop_schedule.h"
#include "ui_settings_activity_state.h"
#include "ui_visible_data_sync.h"
#include "ui_views.h"
#include "ui_xiaozhi_auto_return.h"
#include "xiaozhi_ai.h"

#include <esp_log.h>
#include <esp_timer.h>

namespace {

#define UI_XIAOZHI_AUTO_RETURN_LOG "Xiaozhi idle timeout, returning to home page=%d"

constexpr const char *kUiRuntimeLogTexts[] = {
    UI_XIAOZHI_AUTO_RETURN_LOG,
};

TickType_t next_second_delay_ticks()
{
    return pdMS_TO_TICKS(ui_next_second_delay_ms(esp_timer_get_time()));
}

TickType_t next_minute_delay_ticks(const struct tm &local)
{
    return pdMS_TO_TICKS(ui_next_minute_delay_ms(local.tm_sec));
}

bool low_refresh_work_page_idle(const struct tm &local,
                                const BatteryRuntimeSnapshot &battery)
{
    return work_page_uses_low_refresh_idle(active_work_page_load()) &&
           !battery.low_battery_mode &&
           !battery.charging &&
           !setup_portal_active_load() &&
           !ui_runtime_auxiliary_page_requested() &&
           is_tm_plausible(local);
}

bool radio_fast_poll_active(const struct tm &local)
{
    return active_work_page_load() == kWorkPageRadio &&
           !battery_low_mode_load() &&
           !setup_portal_active_load() &&
           !ui_runtime_auxiliary_page_requested() &&
           is_tm_plausible(local);
}

static_assert(sizeof(TickType_t) == sizeof(uint32_t),
              "UI delay candidates require 32-bit FreeRTOS ticks");
static_assert(array_count(kUiRuntimeLogTexts) > 0,
              "UI runtime log text registry must not be empty");
static_assert(cstr_array_nonempty(kUiRuntimeLogTexts),
              "UI runtime log texts must be non-empty");

} // namespace

bool ui_runtime_settings_timeout_elapsed(TickType_t last_activity)
{
    if (last_activity == 0) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(kSettingsTimeoutMs);
    return app_tick_interval_elapsed(now, last_activity, timeout_ticks);
}

bool ui_runtime_auxiliary_page_requested()
{
    return settings_page_requested() ||
           info_page_requested() ||
           network_diag_page_requested();
}

TickType_t ui_runtime_next_loop_delay_ticks(const struct tm &local,
                                            bool battery_blink_visible)
{
    BatteryRuntimeSnapshot battery;
    battery_runtime_snapshot_load(&battery);
    bool low_idle = battery.low_battery_mode &&
                    !battery.charging &&
                    !ui_runtime_auxiliary_page_requested() &&
                    is_tm_plausible(local);
    bool low_refresh_page_idle = low_refresh_work_page_idle(local, battery);
    uint32_t delay_candidates[5] = {};
    delay_candidates[0] = (low_idle || low_refresh_page_idle)
                              ? next_minute_delay_ticks(local)
                              : next_second_delay_ticks();
    if (radio_fast_poll_active(local)) {
        delay_candidates[1] = pdMS_TO_TICKS(kUiLoopRadioPollMs);
    }
    if (normal_work_page_active(kWorkPageXiaozhiAI)) {
        PomodoroSnapshot pomodoro = {};
        pomodoro_get_snapshot(&pomodoro);
        if (pomodoro.state == kPomodoroRunning) {
            uint32_t boundary_ms = pomodoro_next_display_boundary_ms(
                pomodoro.remaining_ms);
            if (boundary_ms > 0) {
                delay_candidates[2] = ui_nonzero_delay_ticks(
                    pdMS_TO_TICKS(ui_pomodoro_boundary_delay_ms(boundary_ms)));
            }
        }
        uint32_t subtitle_delay_ms = xiaozhi_subtitle_animation_delay_ms();
        if (subtitle_delay_ms > 0) {
            delay_candidates[3] = ui_nonzero_delay_ticks(
                pdMS_TO_TICKS(subtitle_delay_ms));
        }
    }
    if (battery_blink_visible) {
        delay_candidates[4] = next_second_delay_ticks();
    }
    return static_cast<TickType_t>(ui_shortest_delay_ticks(
        delay_candidates,
        sizeof(delay_candidates) / sizeof(delay_candidates[0])));
}

void ui_runtime_update_xiaozhi_auto_return(TickType_t tick_now,
                                           TickType_t &last_activity_tick,
                                           uint32_t &last_activity_sequence)
{
    int active_page = active_work_page_load();
    if (active_page == kWorkPageXiaozhiAI &&
        !battery_low_mode_load() &&
        !setup_portal_active_load() &&
        !ui_runtime_auxiliary_page_requested()) {
        XiaozhiAiSnapshot snapshot = {};
        xiaozhi_ai_get_snapshot(&snapshot);
        bool conversation_active = snapshot.state == kXiaozhiAiListening ||
                                   snapshot.state == kXiaozhiAiSpeaking;
        XiaozhiAutoReturnDecision auto_return = xiaozhi_auto_return_decision(
            tick_now,
            last_activity_tick,
            pdMS_TO_TICKS(kXiaozhiAutoReturnTimeoutMs),
            g_xiaozhi_auto_return_enabled,
            pomodoro_is_running(),
            conversation_active,
            snapshot.activity_sequence != last_activity_sequence);
        if (auto_return.record_activity) {
            last_activity_tick = tick_now;
            last_activity_sequence = snapshot.activity_sequence;
        } else if (auto_return.return_home) {
            int home_page = first_enabled_work_page();
            if (home_page != kWorkPageXiaozhiAI) {
                ESP_LOGI(TAG, UI_XIAOZHI_AUTO_RETURN_LOG, home_page);
                active_work_page_store(home_page);
            }
            last_activity_tick = tick_now;
        }
    } else if (active_page != kWorkPageXiaozhiAI) {
        last_activity_tick = 0;
        last_activity_sequence = 0;
    }
}
