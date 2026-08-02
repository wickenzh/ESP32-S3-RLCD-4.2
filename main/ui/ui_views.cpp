// 运行 LVGL UI 主任务并统一调度各页面刷新。
#include "ui_views.h"

#include "ui_clock_alert_state.h"

#include "alarm_services.h"
#include "app_constexpr.h"
#include "app_tick_time.h"
#include "audio_services.h"
#include "network_diagnostics_state.h"
#include "network_services.h"
#include "ota_runtime_state.h"
#include "ota_services.h"
#include "sensor_services.h"
#include "ui_battery.h"
#include "ui_battery_blink.h"
#include "ui_draw_cache.h"
#include "ui_info_page_state.h"
#include "ui_runtime_schedule.h"
#include "ui_setup_status.h"
#include "ui_status_refresh_policy.h"
#include "ui_settings_activity_state.h"
#include "ui_visible_data_sync.h"
#include "xiaozhi_ai.h"
#include "radio_services.h"
#include "music_player.h"

#include <stdint.h>

namespace {
constexpr int kUiInfoPagePollMs = 250;
constexpr int kUiNetworkDiagRunningPollMs = 250;
constexpr int kUiNetworkDiagIdlePollMs = 500;
constexpr int kUiSettingsPollMs = 100;
constexpr int kUiPostPageSwitchPollMs = 250;
constexpr int kUiLvglLockTimeoutMs = 80;
constexpr const char *kUiDatePlaceholder = "----/--/-- / 星期-";
constexpr const char *kUiTimePlaceholder = "--:--";
#define UI_SETTINGS_TIMEOUT_RETURN_LOG "settings timeout, returning to clock"

constexpr const char *kUiFormatTexts[] = {
    kUiDatePlaceholder,
    kUiTimePlaceholder,
};
constexpr const char *kUiLogTexts[] = {
    UI_SETTINGS_TIMEOUT_RETURN_LOG,
};

static_assert(kUiInfoPagePollMs > 0, "UI info page poll interval must be positive");
static_assert(kUiNetworkDiagRunningPollMs > 0, "network diagnostics running poll interval must be positive");
static_assert(kUiNetworkDiagIdlePollMs >= kUiNetworkDiagRunningPollMs,
              "idle network diagnostics polling must not be faster than running diagnostics polling");
static_assert(kUiSettingsPollMs > 0, "settings poll interval must be positive");
static_assert(kUiPostPageSwitchPollMs > 0, "post page switch poll interval must be positive");
static_assert(kUiLvglLockTimeoutMs > 0, "UI LVGL lock timeout must be positive");
static_assert(array_count(kUiFormatTexts) > 0, "UI format text registry must not be empty");
static_assert(array_count(kUiLogTexts) > 0, "UI log text registry must not be empty");
static_assert(cstr_array_nonempty(kUiFormatTexts), "UI status format and placeholder texts must be non-empty");
static_assert(cstr_array_nonempty(kUiLogTexts), "UI main-loop log texts must be non-empty");

} // namespace

namespace {
bool update_invalid_time_labels_for_active_page(int active_work_page)
{
    int status_page = (battery_low_mode_load() || setup_portal_active_load())
                          ? kWorkPageWeatherClock
                          : active_work_page;
    WorkPageStatusLabels labels = get_work_page_status_labels(status_page);
    bool changed = set_label_text_if_changed(labels.date, kUiDatePlaceholder);
    changed |= set_label_text_if_changed(labels.time, kUiTimePlaceholder);
    return changed;
}

bool update_visible_work_page_body(const struct tm &local,
                                   const ActiveWorkPageState &state)
{
    bool changed = false;
    if (state.history) {
        changed |= update_history_page(local);
    }
    if (state.gallery) {
        changed |= update_gallery_page(local);
    }
    if (state.calendar) {
        changed |= update_calendar_page(local);
    }
    if (state.weather_board) {
        changed |= update_weather_board_page(local);
    }
    if (state.radio) {
        changed |= update_radio_page(local);
    }
    if (state.xiaozhi) {
        changed |= update_xiaozhi_page(local);
    }
    return changed;
}

bool update_weather_alert_state(const struct tm &local,
                                const ActiveWorkPageState &state,
                                bool status_due,
                                bool &alert_visible,
                                int &alert_index)
{
    if (!state.weather_clock) {
        if (!alert_visible) {
            return false;
        }
        update_alert_pill(false);
        alert_visible = false;
        alert_index = -1;
        return true;
    }

    WeatherAlertData alert = {};
    get_weather_snapshot(nullptr, &alert);
    ClockAlertDisplayState next_alert = clock_alert_display_state(local.tm_sec,
                                                                 battery_low_mode_load(),
                                                                 alert.active,
                                                                 alert.count);
    if (!clock_alert_display_needs_update(next_alert,
                                          alert_visible,
                                          alert_index,
                                          status_due)) {
        return false;
    }
    update_alert_pill(next_alert.visible, next_alert.index);
    alert_visible = next_alert.visible;
    alert_index = next_alert.visible ? next_alert.index : -1;
    return true;
}

void show_boot_info_aux_page(bool &info_page_visible,
                             bool &settings_page_visible)
{
    build_boot_info_page();
    show_page(g_info_root);
    info_page_visible = true;
    settings_page_visible = false;
}

void show_network_diag_aux_page(bool &network_diag_page_visible,
                                bool &info_page_visible,
                                bool &settings_page_visible)
{
    build_network_diag_page();
    show_page(g_network_diag_root);
    network_diag_page_visible = true;
    info_page_visible = false;
    settings_page_visible = false;
}

bool update_active_work_page_content(struct tm &local,
                                     const ActiveWorkPageState &state,
                                     int active_page,
                                     bool status_due,
                                     bool &alert_visible,
                                     int &alert_index)
{
    bool changed = false;
    if (is_system_time_plausible(&local)) {
        changed |= update_time_ui(local, state.weather_clock, active_page);
        changed |= update_visible_work_page_body(local, state);
        changed |= update_work_page_day_progress(active_page, local);
        changed |= update_weather_alert_state(local,
                                              state,
                                              status_due,
                                              alert_visible,
                                              alert_index);
        return changed;
    }

    changed |= update_invalid_time_labels_for_active_page(active_page);
    invalidate_clock_date_draw_cache();
    update_alert_pill(false);
    if (alert_visible) {
        alert_visible = false;
        alert_index = -1;
        changed = true;
    }
    return changed;
}
} // namespace

void ui_task(void *)
{
    TickType_t last_status_update =
        xTaskGetTickCount() - pdMS_TO_TICKS(kUiStatusFallbackRefreshMs);
    UiStatusRefreshSnapshot last_status_snapshot = {};
    bool last_status_snapshot_valid = false;
    uint32_t last_battery_version = (uint32_t)-1;
    bool info_page_visible = false;
    bool network_diag_page_visible = false;
    bool settings_page_visible = false;
    bool setup_panel_visible = false;
    bool low_mode_visible = false;
    bool alert_visible = false;
    int visible_work_page = kWorkPageWeatherClock;
    int alert_index = -1;
    bool last_battery_charging = false;
    int last_battery_blink_phase = -1;
    uint32_t last_settings_action_seq = settings_activity_action_sequence();
    VisibleSyncRetryState<TickType_t> weather_sync_retry;
    VisibleSyncRetryState<TickType_t> saying_sync_retry;
    TickType_t xiaozhi_last_activity_tick = 0;
    uint32_t last_xiaozhi_activity_sequence = 0;

    for (;;) {
        time_t now;
        time(&now);
        struct tm local = {};
        localtime_r(&now, &local);
        BatteryRuntimeSnapshot battery;
        battery_runtime_snapshot_load(&battery);
        if (!battery.low_battery_mode && !setup_portal_active_load()) {
            ensure_active_work_page_enabled();
        }
        int active_page = active_work_page_load();
        xiaozhi_ai_set_page_active(active_page == kWorkPageXiaozhiAI &&
                                    !battery.low_battery_mode &&
                                    !setup_portal_active_load() &&
                                    !ui_runtime_auxiliary_page_requested());
        radio_set_page_active(active_page == kWorkPageRadio &&
                              !battery.low_battery_mode &&
                              !setup_portal_active_load() &&
                              !ui_runtime_auxiliary_page_requested());
        // 音乐模式由Gallery子模式控制，离开Gallery页面时停用并重置标志
        if (active_page != kWorkPageGallery) {
            g_gallery_music_mode.store(false);
            music_set_page_active(false);
        }

        TickType_t tick_now = xTaskGetTickCount();
        if (active_page == kWorkPageXiaozhiAI &&
            ui_runtime_auxiliary_page_requested()) {
            xiaozhi_last_activity_tick = tick_now;
        }
        UiStatusRefreshSnapshot current_status_snapshot = {
            local_sensor_state_version(),
            alarm_state_version(),
            g_hourly_chime_enabled || g_hourly_chime_all_day,
            wifi_radio_on_for_status_icon(),
        };
        bool status_fallback_elapsed = app_tick_interval_elapsed(
            tick_now,
            last_status_update,
            pdMS_TO_TICKS(kUiStatusFallbackRefreshMs));
        bool status_due = ui_status_refresh_due(current_status_snapshot,
                                                last_status_snapshot,
                                                last_status_snapshot_valid,
                                                status_fallback_elapsed);
        bool battery_due = battery.version != last_battery_version;
        UiBatteryBlinkState battery_blink = ui_battery_blink_state({
            battery.charging,
            battery.animation_complete,
            battery.percent,
            kBatteryChargingAnimationStopPercent,
            active_page,
            kWorkPageCount,
            setup_portal_active_load(),
            ui_runtime_auxiliary_page_requested(),
            is_tm_plausible(local),
            local.tm_sec,
            xTaskGetTickCount() / pdMS_TO_TICKS(kAppMsPerSecond),
        });
        bool battery_blink_visible = battery_blink.visible;
        bool battery_blink_on = battery_blink.on;
        int battery_blink_phase = battery_blink.phase;
        bool battery_blink_due = battery_blink_visible != last_battery_charging ||
                                 (battery_blink_visible &&
                                  battery_blink_phase != last_battery_blink_phase);
        bool setup_due = setup_portal_active_load() != setup_panel_visible;
        bool mode_due = battery.low_battery_mode != low_mode_visible;

        if (Lvgl_lock(kUiLvglLockTimeoutMs)) {
            bool refresh_now = false;
            InfoPageStateSnapshot info_state;
            info_page_state_load(&info_state);
            bool info_requested = info_state.requested;
            bool network_diag_requested = network_diag_page_requested();
            bool settings_requested = settings_page_requested();
            TickType_t info_until = info_state.hold_until_tick;
            auto restore_active_work_page_after_aux = [&](bool clear_info_timeout) {
                show_active_work_page();
                if (clear_info_timeout) {
                    info_page_hold_until_store(0);
                }
                active_page = active_work_page_load();
                visible_work_page = active_page;
                setup_panel_visible = false;
                low_mode_visible = battery.low_battery_mode;
                apply_clock_mode_visibility(false);
                status_due = true;
                battery_due = true;
                battery_blink_due = true;
                invalidate_clock_time_draw_cache();
                refresh_now = true;
            };
            if (info_requested && info_until != 0 &&
                app_tick_deadline_reached(tick_now, info_until) &&
                !ota_flow_active()) {
                info_page_clear();
                info_requested = false;
            }
            if (battery.low_battery_mode && !ota_flow_active() &&
                (info_requested ||
                 network_diag_requested ||
                 settings_requested ||
                 active_page != kWorkPageWeatherClock)) {
                info_page_clear();
                network_diag_page_clear();
                settings_page_clear();
                reset_settings_navigation_state();
                info_requested = false;
                network_diag_requested = false;
                settings_requested = false;
                info_page_visible = false;
                network_diag_page_visible = false;
                settings_page_visible = false;
                active_work_page_store(kWorkPageWeatherClock);
                active_page = kWorkPageWeatherClock;
                show_active_work_page();
                visible_work_page = kWorkPageWeatherClock;
                setup_panel_visible = false;
                low_mode_visible = battery.low_battery_mode;
                apply_clock_mode_visibility(setup_portal_active_load());
                update_alert_pill(false);
                alert_visible = false;
                alert_index = -1;
                status_due = true;
                battery_due = true;
                battery_blink_due = true;
                invalidate_clock_time_draw_cache();
                refresh_now = true;
            }
            if (info_requested && !settings_requested) {
                if (!info_page_visible) {
                    show_boot_info_aux_page(info_page_visible,
                                            settings_page_visible);
                }
                update_boot_info_page();
                lv_refr_now(nullptr);
                Lvgl_unlock();
                vTaskDelay(pdMS_TO_TICKS(ota_runtime_state_load() == kOtaUpdating
                                             ? kOtaStatusMinIntervalMs
                                             : kUiInfoPagePollMs));
                continue;
            }
            if (info_page_visible) {
                info_page_visible = false;
                restore_active_work_page_after_aux(true);
            }

            int network_diag_state = network_diag_state_load();
            if (network_diag_requested &&
                network_diag_state == kNetworkDiagDone &&
                ui_runtime_settings_timeout_elapsed(
                    settings_activity_last_tick())) {
                network_diag_page_clear();
                network_diag_requested = false;
            }
            if (network_diag_requested && !settings_requested) {
                if (!network_diag_page_visible) {
                    show_network_diag_aux_page(network_diag_page_visible,
                                               info_page_visible,
                                               settings_page_visible);
                }
                if (update_network_diag_page()) {
                    lv_refr_now(nullptr);
                }
                Lvgl_unlock();
                vTaskDelay(pdMS_TO_TICKS(network_diag_state_load() == kNetworkDiagRunning
                                            ? kUiNetworkDiagRunningPollMs
                                            : kUiNetworkDiagIdlePollMs));
                continue;
            }
            if (network_diag_page_visible) {
                network_diag_page_visible = false;
                restore_active_work_page_after_aux(false);
            }

            if (settings_requested) {
                bool settings_changed = false;
                bool settings_action_handled = false;
                if (!settings_page_visible) {
                    build_settings_page();
                    show_page(g_settings_root);
                    settings_page_visible = true;
                    info_page_visible = false;
                    network_diag_page_visible = false;
                    setup_panel_visible = false;
                    settings_changed = true;
                }
                uint32_t settings_action_seq = settings_activity_action_sequence();
                if (settings_action_seq != last_settings_action_seq) {
                    last_settings_action_seq = settings_action_seq;
                    handle_settings_action();
                    settings_changed = true;
                    settings_action_handled = true;
                    settings_requested = settings_page_requested();
                    if (!settings_requested && info_page_requested()) {
                        show_boot_info_aux_page(info_page_visible,
                                                settings_page_visible);
                        update_boot_info_page();
                        lv_refr_now(nullptr);
                        Lvgl_unlock();
                        vTaskDelay(pdMS_TO_TICKS(kUiPostPageSwitchPollMs));
                        continue;
                    }
                    if (!settings_requested && network_diag_page_requested()) {
                        show_network_diag_aux_page(network_diag_page_visible,
                                                   info_page_visible,
                                                   settings_page_visible);
                        update_network_diag_page();
                        lv_refr_now(nullptr);
                        Lvgl_unlock();
                        vTaskDelay(pdMS_TO_TICKS(kUiPostPageSwitchPollMs));
                        continue;
                    }
                }
                if (settings_requested && finish_settings_sync_if_timed_out(tick_now)) {
                    settings_changed = true;
                }
                if (settings_requested) {
                    TickType_t last_activity = settings_activity_last_tick();
                    bool button_pressed = gpio_get_level(kBootButtonGpio) == 0 ||
                                          gpio_get_level(kKeyButtonGpio) == 0;
                    if (!settings_action_handled &&
                        !button_pressed &&
                        !is_settings_sync_busy() && !ota_flow_active() &&
                        ui_runtime_settings_timeout_elapsed(last_activity)) {
                        ESP_LOGI(TAG, "%s", UI_SETTINGS_TIMEOUT_RETURN_LOG);
                        if (g_settings_page_order_mode) {
                            if (save_work_page_order()) {
                                active_work_page_store(first_enabled_work_page());
                            }
                        }
                        settings_page_clear();
                        reset_settings_navigation_state();
                        settings_requested = false;
                    }
                }
                if (settings_requested) {
                    if (update_settings_page() || settings_changed) {
                        lv_refr_now(nullptr);
                    }
                    Lvgl_unlock();
                    vTaskDelay(pdMS_TO_TICKS(ota_runtime_state_load() == kOtaUpdating
                                                 ? kOtaStatusMinIntervalMs
                                                 : kUiSettingsPollMs));
                    continue;
                }
            }

            if (settings_page_visible) {
                settings_page_visible = false;
                restore_active_work_page_after_aux(false);
            }

            if (battery.low_battery_mode || setup_portal_active_load()) {
                if (active_page != kWorkPageWeatherClock) {
                    active_work_page_store(kWorkPageWeatherClock);
                }
            } else {
                ensure_active_work_page_enabled();
            }
            active_page = active_work_page_load();
            ui_runtime_update_xiaozhi_auto_return(
                tick_now,
                xiaozhi_last_activity_tick,
                last_xiaozhi_activity_sequence);
            active_page = active_work_page_load();
            if (visible_work_page != active_page) {
                show_active_work_page();
                visible_work_page = active_page;
                xiaozhi_ai_set_page_active(visible_work_page == kWorkPageXiaozhiAI);
                radio_set_page_active(visible_work_page == kWorkPageRadio);
                if (visible_work_page != kWorkPageGallery) {
                    g_gallery_music_mode.store(false);
                    music_set_page_active(false);
                }
                status_due = true;
                battery_due = true;
                battery_blink_due = true;
                invalidate_history_draw_cache();

                invalidate_clock_date_draw_cache();
                refresh_now = true;
            }
            const ActiveWorkPageState active_pages = active_work_page_state(active_page);
            update_visible_weather_sync(active_pages,
                                        now,
                                        tick_now,
                                        weather_sync_retry);
            update_visible_daily_saying_sync(active_pages,
                                             local,
                                             now,
                                             tick_now,
                                             saying_sync_retry);

            refresh_now |= update_active_work_page_content(local,
                                                           active_pages,
                                                           active_page,
                                                           status_due,
                                                           alert_visible,
                                                           alert_index);

            if (status_due || battery_due || battery_blink_due || setup_due || mode_due) {
                EventBits_t bits = xEventGroupGetBits(g_app_events);
                bool setup_active = setup_portal_active_load();
                bool content_changed = false;
                if (setup_active != setup_panel_visible || mode_due) {
                    apply_clock_mode_visibility(setup_active);
                    setup_panel_visible = setup_active;
                    low_mode_visible = battery.low_battery_mode;
                    status_due = true;
                    invalidate_clock_time_draw_cache();
                    invalidate_clock_second_progress_draw_cache();
                    update_alert_pill(false);
                    alert_visible = false;
                    alert_index = -1;
                    refresh_now = true;
                }
                if (setup_active) {
                    content_changed |= update_setup_status_panel();
                }
                if (!setup_active && !battery.low_battery_mode && active_pages.weather_clock) {
                    content_changed |= update_weather_clock_sensor_status();
                    content_changed |= update_weather_clock_network_status(bits,
                                                                            now,
                                                                            tick_now,
                                                                            weather_sync_retry);
                }
                if (battery_due || battery_blink_due) {
                    update_work_page_battery_icon(active_page,
                                                  battery.percent,
                                                  battery_blink_visible,
                                                  battery_blink_on);
                    last_battery_version = battery.version;
                    last_battery_charging = battery_blink_visible;
                    last_battery_blink_phase = battery_blink_phase;
                    content_changed = true;
                }
                if (status_due) {
                    if (!active_pages.weather_clock) {
                        content_changed |= update_non_clock_work_page_sensor_status(active_page);
                    }
                    if (active_pages.weather_clock) {
                        content_changed |= update_top_status_icons(alert_visible);
                    } else {
                        content_changed |= update_work_page_status_icons(active_page);
                    }
                    last_status_update = tick_now;
                    last_status_snapshot = current_status_snapshot;
                    last_status_snapshot_valid = true;
                }
                refresh_now |= content_changed;
            }
            if (refresh_now) {
                lv_refr_now(nullptr);
            }
            Lvgl_unlock();
        }
        TickType_t delay_ticks = ui_runtime_next_loop_delay_ticks(
            local,
            battery_blink_visible);
        ulTaskNotifyTake(pdTRUE, delay_ticks);
    }
}
