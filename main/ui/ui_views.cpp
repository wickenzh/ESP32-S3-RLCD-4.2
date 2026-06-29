// 运行 LVGL UI 主任务并统一调度各页面刷新。
#include "ui_views.h"

#include "audio_services.h"
#include "network_services.h"
#include "ota_services.h"
#include "sensor_services.h"

namespace {
constexpr int kUiStatusRefreshMs = 10000;
constexpr int kUiInfoPagePollMs = 250;
constexpr int kUiNetworkDiagRunningPollMs = 250;
constexpr int kUiNetworkDiagIdlePollMs = 500;
constexpr int kUiSettingsPollMs = 100;
constexpr int kUiPostPageSwitchPollMs = 250;
constexpr int64_t kUiUsPerSecond = 1000000;
constexpr int kUiMsPerSecond = 1000;
constexpr int kUiSecondsPerMinute = 60;
constexpr int kUiBoundaryWakeSlackMs = 5;
constexpr int kUiNextSecondDelayMinMs = 10;
constexpr int kUiNextSecondDelayMaxMs = kUiMsPerSecond + kUiBoundaryWakeSlackMs;
} // namespace

void ui_task(void *)
{
    TickType_t last_status_update = xTaskGetTickCount() - pdMS_TO_TICKS(kUiStatusRefreshMs);
    uint32_t last_battery_version = (uint32_t)-1;
    bool info_page_visible = false;
    bool network_diag_page_visible = false;
    bool settings_page_visible = false;
    bool setup_panel_visible = false;
    bool low_mode_visible = false;
    bool alert_visible = false;
    int visible_work_page = 0;
    TickType_t history_page_shown_since = 0;
    int alert_index = -1;
    bool last_battery_charging = false;
    int last_battery_blink_phase = -1;
    uint32_t last_settings_action_seq = g_settings_action_seq;
    bool clock_weather_sync_requested = false;
    TickType_t clock_weather_sync_request_tick = 0;
    int clock_weather_sync_attempts = 0;
    TickType_t clock_weather_sync_backoff_until_tick = 0;
    bool gallery_saying_sync_requested = false;
    TickType_t gallery_saying_sync_request_tick = 0;
    int gallery_saying_sync_attempts = 0;
    TickType_t gallery_saying_sync_backoff_until_tick = 0;

    auto delay_to_next_second = []() {
        int64_t us = esp_timer_get_time();
        int64_t until_next = kUiUsPerSecond - (us % kUiUsPerSecond);
        int delay_ms = (int)(until_next / kUiMsPerSecond) + kUiBoundaryWakeSlackMs;
        if (delay_ms < kUiNextSecondDelayMinMs) {
            delay_ms = kUiNextSecondDelayMinMs;
        } else if (delay_ms > kUiNextSecondDelayMaxMs) {
            delay_ms = kUiNextSecondDelayMaxMs;
        }
        return pdMS_TO_TICKS(delay_ms);
    };
    auto delay_to_next_minute = [](const struct tm &local) {
        int seconds_to_next = kUiSecondsPerMinute - local.tm_sec;
        if (seconds_to_next <= 0 || seconds_to_next > kUiSecondsPerMinute) {
            seconds_to_next = kUiSecondsPerMinute;
        }
        return pdMS_TO_TICKS(seconds_to_next * kUiMsPerSecond + kUiBoundaryWakeSlackMs);
    };
    auto weather_cache_stale = [](time_t now_value) {
        if (g_last_weather_sync_time <= 0) {
            return true;
        }
        struct tm now_local = {};
        struct tm last_local = {};
        localtime_r(&now_value, &now_local);
        localtime_r(&g_last_weather_sync_time, &last_local);
        if (!is_tm_plausible(now_local) || !is_tm_plausible(last_local)) {
            return now_value - g_last_weather_sync_time >= 3600;
        }
        return now_local.tm_year != last_local.tm_year ||
               now_local.tm_yday != last_local.tm_yday ||
               now_local.tm_hour != last_local.tm_hour;
    };
    auto weather_board_details_missing = []() {
        WeatherForecastData forecast = {};
        WeatherAirData air = {};
        get_weather_forecast_snapshot(&forecast);
        get_weather_air_snapshot(&air);
        return !forecast.ready ||
               forecast.count <= 0 ||
               !forecast.days[0].valid ||
               !air.ready;
    };
    auto saying_cache_stale = [](const struct tm &local_value, time_t now_value) {
        if (g_daily_saying[0] == '\0' || g_last_saying_sync_time <= 0) {
            return true;
        }
        struct tm saying_local = {};
        localtime_r(&g_last_saying_sync_time, &saying_local);
        if (!is_tm_plausible(saying_local) || !is_tm_plausible(local_value)) {
            return now_value - g_last_saying_sync_time >= 24 * 3600;
        }
        return saying_local.tm_year != local_value.tm_year ||
               saying_local.tm_yday != local_value.tm_yday;
    };
    auto request_weather_sync_if_needed = [&](TickType_t tick_value, bool sync_in_flight, const char *reason) {
        if (clock_weather_sync_backoff_until_tick != 0 &&
            tick_value >= clock_weather_sync_backoff_until_tick) {
            clock_weather_sync_attempts = 0;
            clock_weather_sync_backoff_until_tick = 0;
        }
        bool backoff_active = clock_weather_sync_backoff_until_tick != 0 &&
                              tick_value < clock_weather_sync_backoff_until_tick;
        if (clock_weather_sync_requested && !sync_in_flight &&
            clock_weather_sync_request_tick != 0 &&
            tick_value - clock_weather_sync_request_tick >= pdMS_TO_TICKS(kWeatherClockAutoRetryMs)) {
            clock_weather_sync_requested = false;
            clock_weather_sync_request_tick = 0;
            if (clock_weather_sync_attempts >= kWeatherClockAutoSyncMaxAttempts) {
                clock_weather_sync_backoff_until_tick = tick_value + pdMS_TO_TICKS(kWeatherClockAutoBackoffMs);
                backoff_active = true;
            }
        }
        if (!clock_weather_sync_requested &&
            clock_weather_sync_attempts < kWeatherClockAutoSyncMaxAttempts &&
            !backoff_active &&
            !ota_flow_active() &&
            !sync_in_flight) {
            clock_weather_sync_requested = true;
            clock_weather_sync_request_tick = tick_value;
            ++clock_weather_sync_attempts;
            ESP_LOGI(TAG, "weather clock visible with %s weather, requesting sync", reason);
            xEventGroupSetBits(g_app_events, kManualWeatherSyncBit);
        }
    };

    for (;;) {
        time_t now;
        time(&now);
        struct tm local = {};
        localtime_r(&now, &local);
        ensure_active_work_page_enabled();

        TickType_t tick_now = xTaskGetTickCount();
        bool status_due = tick_now - last_status_update >= pdMS_TO_TICKS(kUiStatusRefreshMs);
        bool battery_due = g_battery_version != last_battery_version;
        bool battery_blink_visible = g_battery_charging &&
                                     (g_active_work_page == kWorkPageWeatherClock ||
                                      g_active_work_page == kWorkPageGallery ||
                                      g_active_work_page == kWorkPageFlipClock) &&
                                     !g_setup_portal_active &&
                                     !g_settings_requested &&
                                     !g_boot_info_requested &&
                                     !g_network_diag_page_requested;
        bool battery_blink_on = battery_blink_visible && is_tm_plausible(local) && (local.tm_sec % 2 == 0);
        int battery_blink_phase = battery_blink_on ? 1 : 0;
        bool battery_blink_due = battery_blink_visible != last_battery_charging ||
                                 (battery_blink_visible &&
                                  battery_blink_phase != last_battery_blink_phase);
        bool setup_due = g_setup_portal_active != setup_panel_visible;
        bool mode_due = g_low_battery_mode != low_mode_visible;

        if (Lvgl_lock(80)) {
            bool refresh_now = false;
            bool info_requested = g_boot_info_requested;
            bool network_diag_requested = g_network_diag_page_requested;
            bool settings_requested = g_settings_requested;
            TickType_t info_until = g_info_page_until_tick;
            if (info_requested && info_until != 0 && tick_now >= info_until && !ota_flow_active()) {
                g_boot_info_requested = false;
                g_info_page_until_tick = 0;
                info_requested = false;
            }
            if (g_low_battery_mode && !ota_flow_active() &&
                (info_requested ||
                 network_diag_requested ||
                 settings_requested ||
                 g_active_work_page != kWorkPageWeatherClock)) {
                g_boot_info_requested = false;
                g_network_diag_page_requested = false;
                g_settings_requested = false;
                g_info_page_until_tick = 0;
                info_requested = false;
                network_diag_requested = false;
                settings_requested = false;
                info_page_visible = false;
                network_diag_page_visible = false;
                settings_page_visible = false;
                g_active_work_page = 0;
                show_active_work_page();
                visible_work_page = 0;
                history_page_shown_since = 0;
                setup_panel_visible = false;
                low_mode_visible = g_low_battery_mode;
                apply_clock_mode_visibility(g_setup_portal_active);
                update_alert_pill(false);
                alert_visible = false;
                alert_index = -1;
                status_due = true;
                battery_due = true;
                battery_blink_due = true;
                g_last_ui_second = -1;
                g_last_ui_minute = -1;
                g_last_ui_date_key = -1;
                g_last_ui_date_page = -1;
                refresh_now = true;
            }
            if (info_requested && !settings_requested) {
                if (!info_page_visible) {
                    build_boot_info_page();
                    show_page(g_info_root);
                    info_page_visible = true;
                    settings_page_visible = false;
                }
                update_boot_info_page();
                lv_refr_now(nullptr);
                Lvgl_unlock();
                vTaskDelay(pdMS_TO_TICKS(g_ota_state == kOtaUpdating ? kOtaStatusMinIntervalMs : kUiInfoPagePollMs));
                continue;
            }
            if (info_page_visible) {
                show_active_work_page();
                info_page_visible = false;
                g_info_page_until_tick = 0;
                visible_work_page = g_active_work_page;
                history_page_shown_since = g_active_work_page == kWorkPageHistory ? tick_now : 0;
                setup_panel_visible = false;
                low_mode_visible = g_low_battery_mode;
                apply_clock_mode_visibility(false);
                status_due = true;
                battery_due = true;
                battery_blink_due = true;
                g_last_ui_second = -1;
                g_last_ui_minute = -1;
                g_last_ui_date_key = -1;
                g_last_ui_date_page = -1;
                refresh_now = true;
            }

            if (network_diag_requested &&
                g_network_diag_state == kNetworkDiagDone &&
                g_settings_last_activity_tick != 0 &&
                tick_now - g_settings_last_activity_tick >= pdMS_TO_TICKS(kSettingsTimeoutMs)) {
                g_network_diag_page_requested = false;
                network_diag_requested = false;
            }
            if (network_diag_requested && !settings_requested) {
                if (!network_diag_page_visible) {
                    build_network_diag_page();
                    show_page(g_network_diag_root);
                    network_diag_page_visible = true;
                    info_page_visible = false;
                    settings_page_visible = false;
                }
                if (update_network_diag_page()) {
                    lv_refr_now(nullptr);
                }
                Lvgl_unlock();
                vTaskDelay(pdMS_TO_TICKS(g_network_diag_state == kNetworkDiagRunning ? kUiNetworkDiagRunningPollMs : kUiNetworkDiagIdlePollMs));
                continue;
            }
            if (network_diag_page_visible) {
                show_active_work_page();
                network_diag_page_visible = false;
                visible_work_page = g_active_work_page;
                history_page_shown_since = g_active_work_page == kWorkPageHistory ? tick_now : 0;
                setup_panel_visible = false;
                low_mode_visible = g_low_battery_mode;
                apply_clock_mode_visibility(false);
                status_due = true;
                battery_due = true;
                battery_blink_due = true;
                g_last_ui_second = -1;
                g_last_ui_minute = -1;
                g_last_ui_date_key = -1;
                g_last_ui_date_page = -1;
                refresh_now = true;
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
                if (g_settings_action_seq != last_settings_action_seq) {
                    last_settings_action_seq = g_settings_action_seq;
                    handle_settings_action();
                    settings_changed = true;
                    settings_action_handled = true;
                    settings_requested = g_settings_requested;
                    if (!settings_requested && g_boot_info_requested) {
                        build_boot_info_page();
                        show_page(g_info_root);
                        info_page_visible = true;
                        settings_page_visible = false;
                        update_boot_info_page();
                        lv_refr_now(nullptr);
                        Lvgl_unlock();
                        vTaskDelay(pdMS_TO_TICKS(kUiPostPageSwitchPollMs));
                        continue;
                    }
                    if (!settings_requested && g_network_diag_page_requested) {
                        build_network_diag_page();
                        show_page(g_network_diag_root);
                        network_diag_page_visible = true;
                        info_page_visible = false;
                        settings_page_visible = false;
                        update_network_diag_page();
                        lv_refr_now(nullptr);
                        Lvgl_unlock();
                        vTaskDelay(pdMS_TO_TICKS(kUiPostPageSwitchPollMs));
                        continue;
                    }
                }
                if (settings_requested && is_settings_sync_busy()) {
                    TickType_t deadline = g_settings_sync_deadline_tick;
                    if (deadline != 0 && tick_now >= deadline) {
                        int op = g_settings_sync_op;
                        ESP_LOGW(TAG, "settings manual sync timeout: op=%d", op);
                        if (op == kSettingsSyncNtp) {
                            xEventGroupClearBits(g_app_events, kManualNtpSyncBit);
                            finish_settings_sync(kSettingsSyncNtp, "时间同步超时");
                            settings_changed = true;
                        } else if (op == kSettingsSyncWeather) {
                            xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
                            finish_settings_sync(kSettingsSyncWeather, "天气同步超时");
                            settings_changed = true;
                        } else if (op == kSettingsSyncSaying) {
                            xEventGroupClearBits(g_app_events, kManualSayingSyncBit);
                            finish_settings_sync(kSettingsSyncSaying, "一言更新超时");
                            settings_changed = true;
                        } else {
                            g_settings_sync_op = kSettingsSyncNone;
                            g_settings_sync_deadline_tick = 0;
                            settings_changed = true;
                        }
                    }
                }
                if (settings_requested) {
                    TickType_t last_activity = g_settings_last_activity_tick;
                    bool button_pressed = gpio_get_level(kBootButtonGpio) == 0 ||
                                          gpio_get_level(kKeyButtonGpio) == 0;
                    if (!settings_action_handled &&
                        !button_pressed &&
                        !is_settings_sync_busy() && !ota_flow_active() &&
                        last_activity != 0 && tick_now - last_activity >= pdMS_TO_TICKS(kSettingsTimeoutMs)) {
                        ESP_LOGI(TAG, "settings timeout, returning to clock");
                        if (g_settings_page_order_mode) {
                            if (save_work_page_order()) {
                                g_active_work_page = first_enabled_work_page();
                            }
                        }
                        g_settings_requested = false;
                        g_settings_focus_secondary = false;
                        g_settings_page_order_mode = false;
                        g_factory_reset_confirm_pending = false;
                        g_offline_disable_confirm_pending = false;
                        settings_requested = false;
                    }
                }
                if (settings_requested) {
                    if (update_settings_page() || settings_changed) {
                        lv_refr_now(nullptr);
                    }
                    Lvgl_unlock();
                    vTaskDelay(pdMS_TO_TICKS(g_ota_state == kOtaUpdating ? kOtaStatusMinIntervalMs : kUiSettingsPollMs));
                    continue;
                }
            }

            if (settings_page_visible) {
                show_active_work_page();
                settings_page_visible = false;
                visible_work_page = g_active_work_page;
                history_page_shown_since = g_active_work_page == kWorkPageHistory ? tick_now : 0;
                setup_panel_visible = false;
                low_mode_visible = g_low_battery_mode;
                apply_clock_mode_visibility(false);
                status_due = true;
                battery_due = true;
                battery_blink_due = true;
                g_last_ui_second = -1;
                g_last_ui_minute = -1;
                g_last_ui_date_key = -1;
                g_last_ui_date_page = -1;
                refresh_now = true;
            }

            if ((g_low_battery_mode || g_setup_portal_active) && g_active_work_page != kWorkPageWeatherClock) {
                g_active_work_page = kWorkPageWeatherClock;
            }
            ensure_active_work_page_enabled();
            if (visible_work_page != g_active_work_page) {
                show_active_work_page();
                visible_work_page = g_active_work_page;
                history_page_shown_since = g_active_work_page == kWorkPageHistory ? tick_now : 0;
                status_due = true;
                battery_due = true;
                battery_blink_due = true;
                g_last_history_drawn_version = (uint32_t)-1;
                g_last_history_drawn_hour = -1;
                g_last_flip_clock_hour = -1;
                g_last_flip_clock_minute = -1;
                g_last_flip_clock_second = -1;
                g_last_flip_day_progress_filled = -1;
                g_last_flip_second_progress_filled = -1;
                g_last_flip_sensor_minute = -1;
                g_last_ui_date_key = -1;
                g_last_ui_date_page = -1;
                refresh_now = true;
            }
            if (g_active_work_page == kWorkPageHistory && !g_low_battery_mode && !g_setup_portal_active) {
                if (history_page_shown_since == 0) {
                    history_page_shown_since = tick_now;
                } else if (tick_now - history_page_shown_since >= pdMS_TO_TICKS(kHistoryPageTimeoutMs)) {
                    g_active_work_page = first_enabled_work_page();
                    show_active_work_page();
                    visible_work_page = g_active_work_page;
                    history_page_shown_since = 0;
                    status_due = true;
                    battery_due = true;
                    battery_blink_due = true;
                    g_last_ui_second = -1;
                    g_last_ui_minute = -1;
                    g_last_ui_date_key = -1;
                    g_last_ui_date_page = -1;
                    refresh_now = true;
                }
            } else {
                history_page_shown_since = 0;
            }
            bool history_page_active = g_active_work_page == kWorkPageHistory && !g_low_battery_mode && !g_setup_portal_active;
            bool gallery_page_active = g_active_work_page == kWorkPageGallery && !g_low_battery_mode && !g_setup_portal_active;
            bool calendar_page_active = g_active_work_page == kWorkPageCalendar && !g_low_battery_mode && !g_setup_portal_active;
            bool weather_board_page_active = g_active_work_page == kWorkPageWeatherBoard && !g_low_battery_mode && !g_setup_portal_active;
            bool flip_clock_page_active = g_active_work_page == kWorkPageFlipClock && !g_low_battery_mode && !g_setup_portal_active;
            bool clock_page_active = g_active_work_page == kWorkPageWeatherClock;
            bool weather_data_page_active = clock_page_active || weather_board_page_active;
            if (!weather_data_page_active) {
                clock_weather_sync_requested = false;
                clock_weather_sync_request_tick = 0;
            } else if (!g_have_weather_key || g_offline_mode_ui_enabled || ota_flow_active()) {
                clock_weather_sync_requested = false;
                clock_weather_sync_request_tick = 0;
            } else {
                EventBits_t sync_bits = xEventGroupGetBits(g_app_events);
                bool weather_ready = (sync_bits & kWeatherReadyBit) != 0;
                bool sync_in_flight = (sync_bits & (kManualWeatherSyncBit | kProvisioningSyncBit)) != 0;
                bool details_missing = weather_board_page_active && weather_board_details_missing();
                if (weather_ready && !weather_cache_stale(now) && !details_missing) {
                    clock_weather_sync_requested = false;
                    clock_weather_sync_request_tick = 0;
                    clock_weather_sync_attempts = 0;
                    clock_weather_sync_backoff_until_tick = 0;
                } else {
                    request_weather_sync_if_needed(tick_now,
                                                   sync_in_flight,
                                                   !weather_ready ? "missing" : (details_missing ? "incomplete" : "stale"));
                }
            }
            bool gallery_saying_needs_sync = gallery_page_active &&
                                             !g_offline_mode_ui_enabled &&
                                             saying_cache_stale(local, now);
            if (!gallery_page_active) {
                gallery_saying_sync_requested = false;
                gallery_saying_sync_request_tick = 0;
            } else if (!gallery_saying_needs_sync) {
                gallery_saying_sync_requested = false;
                gallery_saying_sync_request_tick = 0;
                gallery_saying_sync_attempts = 0;
                gallery_saying_sync_backoff_until_tick = 0;
            } else {
                EventBits_t sync_bits = xEventGroupGetBits(g_app_events);
                bool sync_in_flight = (sync_bits & (kManualSayingSyncBit | kProvisioningSyncBit)) != 0;
                if (gallery_saying_sync_backoff_until_tick != 0 &&
                    tick_now >= gallery_saying_sync_backoff_until_tick) {
                    gallery_saying_sync_attempts = 0;
                    gallery_saying_sync_backoff_until_tick = 0;
                }
                bool backoff_active = gallery_saying_sync_backoff_until_tick != 0 &&
                                      tick_now < gallery_saying_sync_backoff_until_tick;
                if (gallery_saying_sync_requested && !sync_in_flight &&
                    gallery_saying_sync_request_tick != 0 &&
                    tick_now - gallery_saying_sync_request_tick >= pdMS_TO_TICKS(kWeatherClockAutoRetryMs)) {
                    gallery_saying_sync_requested = false;
                    gallery_saying_sync_request_tick = 0;
                    if (gallery_saying_sync_attempts >= kWeatherClockAutoSyncMaxAttempts) {
                        gallery_saying_sync_backoff_until_tick = tick_now + pdMS_TO_TICKS(kWeatherClockAutoBackoffMs);
                        backoff_active = true;
                    }
                }
                if (!gallery_saying_sync_requested &&
                    gallery_saying_sync_attempts < kWeatherClockAutoSyncMaxAttempts &&
                    !backoff_active &&
                    !ota_flow_active() &&
                    !sync_in_flight) {
                    gallery_saying_sync_requested = true;
                    gallery_saying_sync_request_tick = tick_now;
                    ++gallery_saying_sync_attempts;
                    ESP_LOGI(TAG, "gallery visible with missing/stale daily saying, requesting sync");
                    xEventGroupSetBits(g_app_events, kManualSayingSyncBit);
                }
            }

            if (is_system_time_plausible(&local)) {
                if (update_time_ui(local, clock_page_active, g_active_work_page)) {
                    refresh_now = true;
                }
                if (history_page_active && update_history_page(local)) {
                    refresh_now = true;
                }
                if (gallery_page_active && update_gallery_page(local)) {
                    refresh_now = true;
                }
                if (calendar_page_active && update_calendar_page(local)) {
                    refresh_now = true;
                }
                if (weather_board_page_active && update_weather_board_page(local)) {
                    refresh_now = true;
                }
                if (flip_clock_page_active && update_flip_clock_page(local)) {
                    refresh_now = true;
                }
                if (clock_page_active) {
                    WeatherAlertData alert = {};
                    get_weather_snapshot(nullptr, &alert);
                    bool next_alert_visible = !g_low_battery_mode && alert.active && alert.count > 0 && (local.tm_sec % 2 == 0);
                    int next_alert_index = alert.count > 0 ? ((local.tm_sec / 2) % alert.count) : 0;
                    if (next_alert_visible != alert_visible ||
                        (next_alert_visible && next_alert_index != alert_index) ||
                        (next_alert_visible && status_due)) {
                        update_alert_pill(next_alert_visible, next_alert_index);
                        alert_visible = next_alert_visible;
                        alert_index = next_alert_visible ? next_alert_index : -1;
                        refresh_now = true;
                    }
                } else if (alert_visible) {
                    update_alert_pill(false);
                    alert_visible = false;
                    alert_index = -1;
                    refresh_now = true;
                }
            } else {
                if (clock_page_active || g_low_battery_mode || g_setup_portal_active) {
                    refresh_now |= set_label_text_if_changed(g_date_label, "----/--/-- / 星期-");
                } else if (history_page_active) {
                    refresh_now |= set_label_text_if_changed(g_history_date_label, "----/--/-- / 星期-");
                    refresh_now |= set_label_text_if_changed(g_history_status_time_label, "--:--");
                } else if (gallery_page_active) {
                    refresh_now |= set_label_text_if_changed(g_gallery_date_label, "----/--/-- / 星期-");
                    refresh_now |= set_label_text_if_changed(g_gallery_status_time_label, "--:--");
                } else if (calendar_page_active) {
                    refresh_now |= set_label_text_if_changed(g_calendar_date_label, "----/--/-- / 星期-");
                    refresh_now |= set_label_text_if_changed(g_calendar_status_time_label, "--:--");
                } else if (weather_board_page_active) {
                    refresh_now |= set_label_text_if_changed(g_weather_board_date_label, "----/--/-- / 星期-");
                    refresh_now |= set_label_text_if_changed(g_weather_board_status_time_label, "--:--");
                } else if (flip_clock_page_active) {
                    refresh_now |= set_label_text_if_changed(g_flip_clock_date_label, "----/--/-- / 星期-");
                }
                g_last_ui_date_key = -1;
                g_last_ui_date_page = -1;
                update_alert_pill(false);
                if (alert_visible) {
                    alert_visible = false;
                    alert_index = -1;
                    refresh_now = true;
                }
            }

            if (status_due || battery_due || battery_blink_due || setup_due || mode_due) {
                EventBits_t bits = xEventGroupGetBits(g_app_events);
                bool setup_active = g_setup_portal_active;
                bool content_changed = false;
                if (setup_active != setup_panel_visible || mode_due) {
                    apply_clock_mode_visibility(setup_active);
                    setup_panel_visible = setup_active;
                    low_mode_visible = g_low_battery_mode;
                    status_due = true;
                    g_last_ui_second = -1;
                    g_last_ui_minute = -1;
                    g_last_ui_date_key = -1;
                    g_last_ui_date_page = -1;
                    g_last_day_progress_filled = -1;
                    g_last_second_progress_filled = -1;
                    update_alert_pill(false);
                    alert_visible = false;
                    alert_index = -1;
                    refresh_now = true;
                }
                if (setup_active) {
                    content_changed |= update_setup_status_panel();
                }
                char temp[32];
                char humi[32];
                if (g_sensor_ok) {
                    snprintf(temp, sizeof(temp), "%.1f℃", g_temperature);
                    snprintf(humi, sizeof(humi), "%.1f%%", g_humidity);
                } else {
                    snprintf(temp, sizeof(temp), "--.-℃");
                    snprintf(humi, sizeof(humi), "--.-%%");
                }

                if (!setup_active && !g_low_battery_mode && clock_page_active) {
                    content_changed |= set_label_text_if_changed(g_temp_label, temp);
                    content_changed |= set_label_text_if_changed(g_humi_label, humi);
                    content_changed |= update_trend_icon(g_temp_trend_canvas, g_sensor_ok ? g_temp_trend : 0, &g_last_temp_trend_drawn);
                    content_changed |= update_trend_icon(g_humi_trend_canvas, g_sensor_ok ? g_humi_trend : 0, &g_last_humi_trend_drawn);
                    if (bits & kWeatherReadyBit) {
                        WeatherData weather = {};
                        get_weather_snapshot(&weather, nullptr);
                        char city[48];
                        char weather_temp[24];
                        char weather_humi[24];
                        snprintf(city, sizeof(city), "%s", weather.city);
                        snprintf(weather_temp, sizeof(weather_temp), "%s℃", weather.temp);
                        snprintf(weather_humi, sizeof(weather_humi), "%s%%", weather.humidity);
                        content_changed |= set_label_text_if_changed(g_weather_city_label, city);
                        content_changed |= set_label_text_if_changed(g_weather_info_label, weather.text);
                        content_changed |= set_label_text_if_changed(g_weather_temp_label, weather_temp);
                        content_changed |= set_label_text_if_changed(g_weather_humi_label, weather_humi);
                        content_changed |= set_label_text_if_changed(g_weather_icon_label, weather_icon_text(weather.icon));
                        if (!weather_cache_stale(now)) {
                            clock_weather_sync_requested = false;
                            clock_weather_sync_request_tick = 0;
                            clock_weather_sync_attempts = 0;
                            clock_weather_sync_backoff_until_tick = 0;
                        } else if (g_have_weather_key && !g_offline_mode_ui_enabled) {
                            EventBits_t sync_bits = xEventGroupGetBits(g_app_events);
                            bool sync_in_flight = (sync_bits & (kManualWeatherSyncBit | kProvisioningSyncBit)) != 0;
                            request_weather_sync_if_needed(tick_now, sync_in_flight, "stale");
                        }
                    } else if (g_have_weather_key && !g_offline_mode_ui_enabled) {
                        EventBits_t sync_bits = xEventGroupGetBits(g_app_events);
                        bool sync_in_flight = (sync_bits & (kManualWeatherSyncBit | kProvisioningSyncBit)) != 0;
                        request_weather_sync_if_needed(tick_now, sync_in_flight, "missing");
                        content_changed |= set_label_text_if_changed(g_weather_city_label, "--");
                        content_changed |= set_label_text_if_changed(g_weather_info_label, (bits & kWifiConnectedBit) ? "天气同步中" : "等待数据");
                        content_changed |= set_label_text_if_changed(g_weather_temp_label, "--℃");
                        content_changed |= set_label_text_if_changed(g_weather_humi_label, "--%");
                        content_changed |= set_label_text_if_changed(g_weather_icon_label, weather_icon_text("999"));
                    } else if (g_offline_mode_ui_enabled) {
                        clock_weather_sync_requested = false;
                        clock_weather_sync_request_tick = 0;
                        clock_weather_sync_attempts = 0;
                        clock_weather_sync_backoff_until_tick = 0;
                        content_changed |= set_label_text_if_changed(g_weather_city_label, "--");
                        content_changed |= set_label_text_if_changed(g_weather_info_label, "等待数据");
                        content_changed |= set_label_text_if_changed(g_weather_temp_label, "--℃");
                        content_changed |= set_label_text_if_changed(g_weather_humi_label, "--%");
                        content_changed |= set_label_text_if_changed(g_weather_icon_label, weather_icon_text("999"));
                    } else {
                        clock_weather_sync_requested = false;
                        clock_weather_sync_request_tick = 0;
                        clock_weather_sync_attempts = 0;
                        clock_weather_sync_backoff_until_tick = 0;
                        content_changed |= set_label_text_if_changed(g_weather_city_label, "--");
                        content_changed |= set_label_text_if_changed(g_weather_info_label, "设置 API Key");
                        content_changed |= set_label_text_if_changed(g_weather_temp_label, "--℃");
                        content_changed |= set_label_text_if_changed(g_weather_humi_label, "--%");
                        content_changed |= set_label_text_if_changed(g_weather_icon_label, weather_icon_text("999"));
                    }
                }
                if (battery_due || battery_blink_due) {
                    update_battery_icon(g_battery_percent, battery_blink_visible, battery_blink_on);
                    last_battery_version = g_battery_version;
                    last_battery_charging = battery_blink_visible;
                    last_battery_blink_phase = battery_blink_phase;
                    content_changed = true;
                    if (history_page_active) {
                        update_battery_segments(g_history_battery_segments, g_battery_percent);
                    }
                    if (gallery_page_active) {
                        update_battery_segments(g_gallery_battery_segments, g_battery_percent, battery_blink_visible, battery_blink_on);
                    }
                    if (calendar_page_active) {
                        update_battery_segments(g_calendar_battery_segments, g_battery_percent);
                    }
                    if (weather_board_page_active) {
                        update_battery_segments(g_weather_board_battery_segments, g_battery_percent);
                    }
                    if (flip_clock_page_active) {
                        update_battery_segments(g_flip_clock_battery_segments, g_battery_percent, battery_blink_visible, battery_blink_on);
                    }
                }
                if (status_due) {
                    if (clock_page_active) {
                        update_top_status_icons(alert_visible);
                        content_changed = true;
                    } else {
                        content_changed |= update_work_page_status_icons(g_active_work_page);
                    }
                    last_status_update = tick_now;
                }
                refresh_now |= content_changed;
            }
            if (refresh_now) {
                lv_refr_now(nullptr);
            }
            Lvgl_unlock();
        }
        bool low_idle = g_low_battery_mode &&
                        !g_battery_charging &&
                        !g_settings_requested &&
                        !g_boot_info_requested &&
                        !g_network_diag_page_requested &&
                        is_tm_plausible(local);
        bool gallery_idle = g_active_work_page == kWorkPageGallery &&
                            !g_low_battery_mode &&
                            !g_battery_charging &&
                            !g_setup_portal_active &&
                            !g_settings_requested &&
                            !g_boot_info_requested &&
                            !g_network_diag_page_requested &&
                            is_tm_plausible(local);
        bool history_idle = g_active_work_page == kWorkPageHistory &&
                            !g_low_battery_mode &&
                            !g_battery_charging &&
                            !g_setup_portal_active &&
                            !g_settings_requested &&
                            !g_boot_info_requested &&
                            !g_network_diag_page_requested &&
                            is_tm_plausible(local);
        bool calendar_idle = g_active_work_page == kWorkPageCalendar &&
                             !g_low_battery_mode &&
                             !g_battery_charging &&
                             !g_setup_portal_active &&
                             !g_settings_requested &&
                             !g_boot_info_requested &&
                             !g_network_diag_page_requested &&
                             is_tm_plausible(local);
        bool weather_board_idle = g_active_work_page == kWorkPageWeatherBoard &&
                                  !g_low_battery_mode &&
                                  !g_battery_charging &&
                                  !g_setup_portal_active &&
                                  !g_settings_requested &&
                                  !g_boot_info_requested &&
                                  !g_network_diag_page_requested &&
                                  is_tm_plausible(local);
        TickType_t delay_ticks = (low_idle || gallery_idle || history_idle || calendar_idle || weather_board_idle)
                                     ? delay_to_next_minute(local)
                                     : delay_to_next_second();
        if (history_idle && history_page_shown_since != 0) {
            TickType_t elapsed = xTaskGetTickCount() - history_page_shown_since;
            TickType_t timeout = pdMS_TO_TICKS(kHistoryPageTimeoutMs);
            TickType_t remaining = elapsed < timeout ? timeout - elapsed : 1;
            if (remaining < delay_ticks) {
                delay_ticks = remaining;
            }
        }
        ulTaskNotifyTake(pdTRUE, delay_ticks);
    }
}

void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    struct FlushRange {
        int x1;
        int x2;
    };
    static FlushRange ranges[kMaxFlushRanges];
    static int range_count = 0;
    static bool force_full_refresh = false;
    static uint32_t full_reason_mask = 0;
    static uint32_t partial_cycles = 0;
    static uint32_t partial_ranges = 0;
    static uint32_t full_cycles = 0;
    static uint32_t full_single_wide = 0;
    static uint32_t full_covered_wide = 0;
    static uint32_t full_too_many_ranges = 0;
    static TickType_t last_diag_tick = 0;
    static int last_diag_page = -1;
    constexpr uint32_t kFullReasonSingleWide = 1U << 0;
    constexpr uint32_t kFullReasonTooManyRanges = 1U << 1;

    int clipped_x1 = area->x1 < 0 ? 0 : area->x1;
    int clipped_x2 = area->x2 >= kDisplayWidth ? kDisplayWidth - 1 : area->x2;
    int clipped_y1 = area->y1 < 0 ? 0 : area->y1;
    int clipped_y2 = area->y2 >= kDisplayHeight ? kDisplayHeight - 1 : area->y2;
    bool touches_visible_area = clipped_x1 <= clipped_x2 && clipped_y1 <= clipped_y2;
    if (touches_visible_area) {
        int area_x1 = clipped_x1;
        int area_x2 = clipped_x2;
        area_x1 &= ~1;
        area_x2 |= 1;
        if (area_x2 >= kDisplayWidth) {
            area_x2 = kDisplayWidth - 1;
        }
        if (area_x2 - area_x1 + 1 >= kDisplayPartialMaxWidth) {
            force_full_refresh = true;
            full_reason_mask |= kFullReasonSingleWide;
        } else {
            bool merged = false;
            for (int i = 0; i < range_count; ++i) {
                if (area_x1 <= ranges[i].x2 + kFlushRangeMergeGap &&
                    area_x2 >= ranges[i].x1 - kFlushRangeMergeGap) {
                    if (area_x1 < ranges[i].x1) ranges[i].x1 = area_x1;
                    if (area_x2 > ranges[i].x2) ranges[i].x2 = area_x2;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                if (range_count < kMaxFlushRanges) {
                    ranges[range_count++] = {area_x1, area_x2};
                } else {
                    force_full_refresh = true;
                    full_reason_mask |= kFullReasonTooManyRanges;
                }
            }
        }
    }

    constexpr uint16_t kRlcdBlackThreshold = 0xC618;
    if (touches_visible_area) {
        int area_width = area->x2 - area->x1 + 1;
        uint16_t *buffer = (uint16_t *)color_map;
        for (int y = clipped_y1; y <= clipped_y2; ++y) {
            uint16_t *row = buffer + (y - area->y1) * area_width + (clipped_x1 - area->x1);
            for (int x = clipped_x1; x <= clipped_x2; ++x) {
                uint8_t color = (*row < kRlcdBlackThreshold) ? ColorBlack : ColorWhite;
                g_display.RLCD_SetPixel(x, y, color);
                ++row;
            }
        }
    }
    if (lv_disp_flush_is_last(drv)) {
        int covered_width = 0;
        for (int i = 0; i < range_count; ++i) {
            covered_width += ranges[i].x2 - ranges[i].x1 + 1;
        }
        bool covered_wide = covered_width >= kDisplayPartialMaxWidth;
        if (force_full_refresh || covered_wide) {
            ++full_cycles;
            if (full_reason_mask & kFullReasonSingleWide) {
                ++full_single_wide;
            }
            if (full_reason_mask & kFullReasonTooManyRanges) {
                ++full_too_many_ranges;
            }
            if (covered_wide) {
                ++full_covered_wide;
            }
            g_display.RLCD_Display();
        } else if (range_count > 0) {
            ++partial_cycles;
            partial_ranges += range_count;
            for (int i = 0; i < range_count; ++i) {
                g_display.RLCD_DisplayXRange(ranges[i].x1, ranges[i].x2);
            }
        }
        TickType_t now_tick = xTaskGetTickCount();
        bool page_changed = last_diag_page != g_active_work_page;
        bool diag_due = last_diag_tick == 0 ||
                        now_tick - last_diag_tick >= pdMS_TO_TICKS(kDisplayFlushDiagIntervalMs) ||
                        page_changed;
        if (diag_due && g_ota_state != kOtaUpdating) {
            ESP_LOGI(TAG,
                     "display flush diag: page=%d partial=%lu ranges=%lu full=%lu reason_single=%lu reason_covered=%lu reason_ranges=%lu",
                     g_active_work_page,
                     (unsigned long)partial_cycles,
                     (unsigned long)partial_ranges,
                     (unsigned long)full_cycles,
                     (unsigned long)full_single_wide,
                     (unsigned long)full_covered_wide,
                     (unsigned long)full_too_many_ranges);
            partial_cycles = 0;
            partial_ranges = 0;
            full_cycles = 0;
            full_single_wide = 0;
            full_covered_wide = 0;
            full_too_many_ranges = 0;
            last_diag_tick = now_tick;
            last_diag_page = g_active_work_page;
        }
        range_count = 0;
        force_full_refresh = false;
        full_reason_mask = 0;
    }
    lv_disp_flush_ready(drv);
}
