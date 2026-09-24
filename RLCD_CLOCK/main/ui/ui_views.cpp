// 运行 LVGL UI 主任务并统一调度各页面刷新。
#include "ui_views.h"

#include "active_work_page_state_internal.h"
#include "app_constexpr.h"
#include "app_metadata.h"
#include "app_runtime_timing.h"
#include "app_task_readiness.h"

#include "alarm_services.h"
#include "app_event_group.h"
#include "app_tick_time.h"
#include "audio_services.h"
#include "battery_policy.h"
#include "battery_runtime_state.h"
#include "chime_runtime_state.h"
#include "daily_saying_state.h"
#include "local_sensor_state.h"
#include "input_button_config.h"
#include "lvgl_bsp.h"
#include "network_diagnostics_state.h"
#include "ota_download_policy.h"
#include "ota_runtime_state.h"
#include "ota_services.h"
#include "runtime_health.h"
#include "sensor_time.h"
#include "ui_battery.h"
#include "ui_battery_blink.h"
#include "ui_aux_pages.h"
#include "ui_settings_page.h"
#include "ui_draw_cache.h"
#include "ui_info_page_state_internal.h"
#include "ui_loop_schedule.h"
#include "ui_runtime_schedule.h"
#include "ui_setup_status.h"
#include "ui_status_refresh_policy.h"
#include "ui_settings_activity_state.h"
#include "ui_settings_feedback.h"
#include "ui_visible_cache.h"
#include "ui_visible_data_sync.h"
#include "ui_work_page_catalog.h"
#include "ui_work_page_update.h"
#include "weather_state.h"
#include "wifi_radio_state.h"
#include "xiaozhi_ai.h"

#include <esp_log.h>
#include <stdint.h>

namespace {
constexpr int kUiInfoPageOtaFallbackPollMs = 1000;
constexpr int kUiNetworkDiagRunningFallbackMs = 1000;
constexpr int kUiNetworkDiagIdleFallbackMs = 500;
constexpr int kUiPostPageSwitchPollMs = 250;
constexpr int kUiLvglLockTimeoutMs = 80;
constexpr EventBits_t kUiWeatherNetworkStatusBits =
    kWifiConnectedBit | kWeatherReadyBit;
#define UI_SETTINGS_TIMEOUT_RETURN_LOG "settings timeout, returning to clock"
#define UI_LOW_BATTERY_PAGE_CAPTURE_LOG "low battery mode entered, remembering page=%d"
#define UI_LOW_BATTERY_PAGE_RESTORE_LOG "low battery mode cleared, restoring page=%d remembered=%d"

enum class VisibleAuxiliaryPage {
    kNone,
    kSystemInfo,
    kNetworkDiagnostics,
    kSettings,
};

static_assert(kUiInfoPageOtaFallbackPollMs > 0,
              "UI info page OTA fallback poll interval must be positive");
static_assert(kUiNetworkDiagRunningFallbackMs > 0,
              "network diagnostics running fallback interval must be positive");
static_assert(kUiNetworkDiagIdleFallbackMs > 0,
              "network diagnostics idle fallback interval must be positive");
static_assert(kUiPostPageSwitchPollMs > 0, "post page switch poll interval must be positive");
static_assert(kUiLvglLockTimeoutMs > 0, "UI LVGL lock timeout must be positive");
static_assert(kUiWeatherNetworkStatusBits != 0,
              "weather network status bits must be nonzero");
} // namespace

namespace {
void show_boot_info_aux_page(VisibleAuxiliaryPage &visible_auxiliary_page)
{
    build_boot_info_page();
    show_page(auxiliary_page_root(AuxiliaryPage::kSystemInfo));
    visible_auxiliary_page = VisibleAuxiliaryPage::kSystemInfo;
}

void show_network_diag_aux_page(
    VisibleAuxiliaryPage &visible_auxiliary_page)
{
    build_network_diag_page();
    show_page(auxiliary_page_root(AuxiliaryPage::kNetworkDiagnostics));
    visible_auxiliary_page = VisibleAuxiliaryPage::kNetworkDiagnostics;
}

void show_settings_aux_page(VisibleAuxiliaryPage &visible_auxiliary_page)
{
    build_settings_page();
    if (!auxiliary_page_root(AuxiliaryPage::kSettings)) return;
    show_page(auxiliary_page_root(AuxiliaryPage::kSettings));
    visible_auxiliary_page = VisibleAuxiliaryPage::kSettings;
}

void apply_xiaozhi_page_activation(bool requested_active,
                                   bool allow_active_retry,
                                   bool &previous_requested_active,
                                   bool &previous_valid)
{
    if (!ui_xiaozhi_activation_update_due(requested_active,
                                          previous_requested_active,
                                          previous_valid,
                                          allow_active_retry)) {
        return;
    }
    xiaozhi_ai_set_page_active(requested_active);
    previous_requested_active = requested_active;
    previous_valid = true;
}

} // namespace

void ui_task(void *)
{
    TickType_t last_status_update =
        xTaskGetTickCount() - pdMS_TO_TICKS(kUiStatusFallbackRefreshMs);
    UiStatusRefreshSnapshot last_status_snapshot = {};
    bool last_status_snapshot_valid = false;
    uint32_t last_battery_version = (uint32_t)-1;
    VisibleAuxiliaryPage visible_auxiliary_page =
        VisibleAuxiliaryPage::kNone;
    bool setup_panel_visible = false;
    bool low_mode_visible = false;
    bool alert_visible = false;
    int visible_work_page = kWorkPageWeatherClock;
    int alert_index = -1;
    uint32_t alert_version = 0;
    bool last_battery_charging = false;
    int last_battery_blink_phase = -1;
    uint32_t last_settings_action_seq = settings_activity_action_sequence();
    VisibleSyncRetryState<TickType_t> weather_sync_retry;
    VisibleSyncRetryState<TickType_t> saying_sync_retry;
    WeatherCacheStatusSnapshot weather_cache_status;
    bool weather_cache_status_valid = false;
    DailySayingCacheSnapshot saying_cache_status;
    bool saying_cache_status_valid = false;
    TickType_t xiaozhi_last_activity_tick = 0;
    uint32_t last_xiaozhi_activity_sequence = 0;
    BatteryRuntimeSnapshot battery = {};
    bool battery_snapshot_valid = false;
    time_t cached_local_second = 0;
    struct tm cached_local = {};
    bool cached_local_valid = false;
    bool xiaozhi_activation_requested = false;
    bool xiaozhi_activation_request_valid = false;
    int low_battery_resume_page = kWorkPageWeatherClock;
    bool low_battery_resume_pending = false;
    uint8_t lvgl_lock_failures = 0;
    regular_app_task_mark_ready(RegularAppTaskId::kUi);

    for (;;) {
        runtime_health_record_current_task_stack(RegularAppTaskId::kUi);
        time_t now;
        time(&now);
        if (ui_local_time_cache_refresh_due(now,
                                            cached_local_second,
                                            cached_local_valid)) {
            struct tm converted = {};
            if (localtime_r(&now, &converted)) {
                cached_local = converted;
                cached_local_second = now;
                cached_local_valid = true;
            } else {
                cached_local = {};
                cached_local_valid = false;
            }
        }
        const struct tm &local = cached_local;
        UiRuntimeSurfaceSnapshot runtime_surfaces =
            ui_runtime_surface_snapshot_load();
        const uint32_t battery_version = battery_runtime_version_load();
        if (!battery_snapshot_valid || battery.version != battery_version) {
            if (battery_runtime_snapshot_load(&battery)) {
                battery_snapshot_valid = true;
            }
        }
        if (!battery.low_battery_mode && !runtime_surfaces.setup_portal_active) {
            ensure_active_work_page_enabled();
        }
        int active_page = active_work_page_load();
        apply_xiaozhi_page_activation(
            active_page == kWorkPageXiaozhiAI &&
                !battery.low_battery_mode &&
                !runtime_surfaces.setup_portal_active &&
                !runtime_surfaces.auxiliary_page_requested(),
            true,
            xiaozhi_activation_requested,
            xiaozhi_activation_request_valid);

        TickType_t tick_now = xTaskGetTickCount();
        if (active_page == kWorkPageXiaozhiAI &&
            runtime_surfaces.auxiliary_page_requested()) {
            xiaozhi_last_activity_tick = tick_now;
        }
        uint32_t weather_network_bits = 0;
        if (ui_weather_network_status_required(
                active_page == kWorkPageWeatherClock,
                battery.low_battery_mode,
                runtime_surfaces.setup_portal_active)) {
            weather_network_bits = static_cast<uint32_t>(
                app_event_group_get_bits() & kUiWeatherNetworkStatusBits);
        }
        UiStatusRefreshSnapshot current_status_snapshot = {
            local_sensor_state_version(),
            weather_network_bits,
            chime_runtime_any_enabled(),
            wifi_radio_on_load(),
            alarm_is_enabled(),
        };
        bool status_fallback_elapsed = app_tick_interval_elapsed(
            tick_now,
            last_status_update,
            pdMS_TO_TICKS(kUiStatusFallbackRefreshMs));
        bool status_due = ui_status_refresh_due(current_status_snapshot,
                                                last_status_snapshot,
                                                last_status_snapshot_valid,
                                                status_fallback_elapsed);
        bool sensor_status_due = ui_sensor_status_refresh_due(
            current_status_snapshot,
            last_status_snapshot,
            last_status_snapshot_valid,
            false);
        bool battery_due = battery.version != last_battery_version;
        UiBatteryBlinkState battery_blink = ui_battery_blink_state({
            battery.charging,
            battery.animation_complete,
            battery.percent,
            kBatteryChargingAnimationStopPercent,
            active_page,
            kWorkPageCount,
            runtime_surfaces.setup_portal_active,
            runtime_surfaces.auxiliary_page_requested(),
            is_tm_plausible(local),
            local.tm_sec,
            tick_now / pdMS_TO_TICKS(kAppMsPerSecond),
        });
        bool battery_blink_visible = battery_blink.visible;
        bool battery_blink_on = battery_blink.on;
        int battery_blink_phase = battery_blink.phase;
        bool battery_blink_due = battery_blink_visible != last_battery_charging ||
                                 (battery_blink_visible &&
                                  battery_blink_phase != last_battery_blink_phase);
        bool setup_due = runtime_surfaces.setup_portal_active != setup_panel_visible;
        bool mode_due = battery.low_battery_mode != low_mode_visible;

        bool start_setup_prompt_after_ui = false;
        const bool lvgl_locked = Lvgl_lock(kUiLvglLockTimeoutMs);
        if (lvgl_locked) {
            lvgl_lock_failures = 0;
            bool refresh_now = false;
            bool info_requested = runtime_surfaces.info_requested;
            InfoPageStateSnapshot info_state = {};
            OtaRuntimeTimingSnapshot info_ota = {};
            bool info_ota_snapshot_valid = false;
            bool info_ota_flow_active = false;
            if (info_requested) {
                if (info_page_state_load(&info_state)) {
                    info_requested = info_state.requested;
                }
                if (info_requested) {
                    ota_runtime_timing_snapshot_load(&info_ota);
                    info_ota_snapshot_valid = true;
                    info_ota_flow_active = ota_flow_active_for_tick(
                        info_ota.state,
                        info_ota.status_hold_set,
                        tick_now,
                        info_ota.status_until_tick);
                }
            }
            bool network_diag_requested = runtime_surfaces.network_diag_requested;
            bool settings_requested = runtime_surfaces.settings_requested;
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
                apply_clock_mode_visibility(false,
                                            battery.low_battery_mode);
                status_due = true;
                sensor_status_due = true;
                battery_due = true;
                battery_blink_due = true;
                invalidate_clock_time_draw_cache();
                refresh_now = true;
            };
            if (mode_due && battery.low_battery_mode &&
                !low_battery_resume_pending) {
                low_battery_resume_page = active_page;
                low_battery_resume_pending = true;
                ESP_LOGI(TAG,
                         UI_LOW_BATTERY_PAGE_CAPTURE_LOG,
                         low_battery_resume_page);
            }
            if (info_requested && info_until != 0 &&
                app_tick_deadline_reached(tick_now, info_until) &&
                !info_ota_flow_active) {
                if (info_page_clear_if_current(info_state)) {
                    info_requested = false;
                    runtime_surfaces.info_requested = false;
                }
            }
            if (battery.low_battery_mode &&
                !(info_ota_snapshot_valid
                      ? info_ota_flow_active
                      : ota_flow_active()) &&
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
                runtime_surfaces.info_requested = false;
                runtime_surfaces.network_diag_requested = false;
                runtime_surfaces.settings_requested = false;
                visible_auxiliary_page = VisibleAuxiliaryPage::kNone;
                active_work_page_store(kWorkPageWeatherClock);
                active_page = kWorkPageWeatherClock;
                show_active_work_page();
                visible_work_page = kWorkPageWeatherClock;
                setup_panel_visible = false;
                low_mode_visible = battery.low_battery_mode;
                apply_clock_mode_visibility(runtime_surfaces.setup_portal_active,
                                            battery.low_battery_mode);
                update_alert_pill(false,
                                  0,
                                  current_status_snapshot,
                                  battery.low_battery_mode,
                                  runtime_surfaces.setup_portal_active);
                alert_visible = false;
                alert_index = -1;
                status_due = true;
                sensor_status_due = true;
                battery_due = true;
                battery_blink_due = true;
                invalidate_clock_time_draw_cache();
                refresh_now = true;
            }
            if (info_requested && !settings_requested) {
                bool info_changed = false;
                if (visible_auxiliary_page !=
                    VisibleAuxiliaryPage::kSystemInfo) {
                    show_boot_info_aux_page(visible_auxiliary_page);
                    info_changed = true;
                }
                info_changed |= update_boot_info_page();
                if (info_changed) {
                    lv_refr_now(nullptr);
                }
                Lvgl_unlock();
                const TickType_t info_wait_now = xTaskGetTickCount();
                const bool info_ota_active_for_wait =
                    info_ota_snapshot_valid &&
                    ota_flow_active_for_tick(info_ota.state,
                                             info_ota.status_hold_set,
                                             info_wait_now,
                                             info_ota.status_until_tick);
                UiInfoPageWaitInput info_wait_input;
                info_wait_input.now_tick = info_wait_now;
                info_wait_input.hold_until_tick = info_until;
                info_wait_input.ota_flow_active = info_ota_active_for_wait;
                info_wait_input.ota_updating =
                    info_ota_snapshot_valid &&
                    info_ota.state == kOtaUpdating;
                const TickType_t info_wait = static_cast<TickType_t>(
                    ui_info_page_wait_ticks(
                        info_wait_input,
                        pdMS_TO_TICKS(kUiInfoPageOtaFallbackPollMs),
                        pdMS_TO_TICKS(kOtaStatusMinIntervalMs)));
                (void)ulTaskNotifyTake(pdTRUE, info_wait);
                continue;
            }
            if (visible_auxiliary_page == VisibleAuxiliaryPage::kSystemInfo) {
                visible_auxiliary_page = VisibleAuxiliaryPage::kNone;
                restore_active_work_page_after_aux(true);
            }

            NetworkDiagState network_diag_state = kNetworkDiagIdle;
            NetworkDiagPageRequestSnapshot network_diag_page_state = {};
            SettingsActivitySnapshot network_diag_activity = {};
            if (network_diag_requested) {
                network_diag_state = network_diag_state_load();
                network_diag_page_state = network_diag_page_snapshot_load();
                network_diag_activity = settings_activity_snapshot();
            }
            if (network_diag_requested &&
                network_diag_state == kNetworkDiagDone &&
                ui_runtime_settings_timeout_elapsed(
                    network_diag_activity.last_activity_tick) &&
                settings_activity_claim_if_current(network_diag_activity) &&
                network_diag_page_clear_if_current(network_diag_page_state)) {
                network_diag_requested = false;
                runtime_surfaces.network_diag_requested = false;
            }
            if (network_diag_requested && !settings_requested) {
                bool network_diag_changed = false;
                if (visible_auxiliary_page !=
                    VisibleAuxiliaryPage::kNetworkDiagnostics) {
                    show_network_diag_aux_page(visible_auxiliary_page);
                    network_diag_changed = true;
                }
                network_diag_changed |= update_network_diag_page();
                if (network_diag_changed) {
                    lv_refr_now(nullptr);
                }
                Lvgl_unlock();
                const NetworkDiagState wait_state = network_diag_state_load();
                TickType_t network_diag_wait = pdMS_TO_TICKS(
                    wait_state == kNetworkDiagRunning
                        ? kUiNetworkDiagRunningFallbackMs
                        : kUiNetworkDiagIdleFallbackMs);
                if (wait_state == kNetworkDiagDone) {
                    network_diag_wait = static_cast<TickType_t>(
                        ui_inactivity_wait_ticks(xTaskGetTickCount(),
                                                 network_diag_activity.last_activity_tick,
                                                 pdMS_TO_TICKS(kSettingsTimeoutMs)));
                }
                (void)ulTaskNotifyTake(pdTRUE, network_diag_wait);
                continue;
            }
            if (visible_auxiliary_page == VisibleAuxiliaryPage::kNetworkDiagnostics) {
                visible_auxiliary_page = VisibleAuxiliaryPage::kNone;
                restore_active_work_page_after_aux(false);
            }

            if (settings_requested) {
                bool settings_changed = false;
                bool settings_action_handled = false;
                if (visible_auxiliary_page !=
                    VisibleAuxiliaryPage::kSettings) {
                    show_settings_aux_page(visible_auxiliary_page);
                    setup_panel_visible = false;
                    settings_changed = true;
                }
                uint32_t settings_action_seq = settings_activity_action_sequence();
                if (settings_action_seq != last_settings_action_seq) {
                    last_settings_action_seq = settings_action_seq;
                    handle_settings_action();
                    settings_changed = true;
                    settings_action_handled = true;
                    runtime_surfaces = ui_runtime_surface_snapshot_load();
                    settings_requested = runtime_surfaces.settings_requested;
                    if (!settings_requested && runtime_surfaces.info_requested) {
                        show_boot_info_aux_page(visible_auxiliary_page);
                        update_boot_info_page();
                        lv_refr_now(nullptr);
                        Lvgl_unlock();
                        vTaskDelay(pdMS_TO_TICKS(kUiPostPageSwitchPollMs));
                        continue;
                    }
                    if (!settings_requested && runtime_surfaces.network_diag_requested) {
                        show_network_diag_aux_page(visible_auxiliary_page);
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
                    const SettingsActivitySnapshot timeout_activity =
                        settings_activity_snapshot();
                    bool button_pressed = gpio_get_level(kBootButtonGpio) == 0 ||
                                          gpio_get_level(kKeyButtonGpio) == 0;
                    if (!settings_action_handled &&
                        !button_pressed &&
                        !is_settings_sync_busy() && !ota_flow_active() &&
                        ui_runtime_settings_timeout_elapsed(
                            timeout_activity.last_activity_tick) &&
                        settings_page_clear_if_activity_current(
                            timeout_activity)) {
                        ESP_LOGI(TAG, "%s", UI_SETTINGS_TIMEOUT_RETURN_LOG);
                        if (settings_navigation_snapshot().page_order_mode) {
                            active_work_page_store(first_enabled_work_page());
                        }
                        reset_settings_navigation_state();
                        settings_requested = false;
                        runtime_surfaces.settings_requested = false;
                    }
                }
                if (settings_requested) {
                    if (update_settings_page() || settings_changed) {
                        lv_refr_now(nullptr);
                    }
                    const TickType_t settings_wait_now = xTaskGetTickCount();
                    const SettingsUiTimingSnapshot settings_timing =
                        settings_ui_timing_snapshot_load();
                    OtaRuntimeTimingSnapshot ota_timing = {};
                    ota_runtime_timing_snapshot_load(&ota_timing);
                    UiSettingsWaitInput wait_input;
                    wait_input.now_tick = settings_wait_now;
                    wait_input.last_activity_tick = settings_activity_last_tick();
                    wait_input.feedback_until_tick =
                        settings_timing.feedback_until_tick;
                    wait_input.sync_deadline_tick =
                        settings_timing.sync_deadline_tick;
                    wait_input.ota_status_until_tick =
                        ota_timing.status_until_tick;
                    wait_input.sync_busy = settings_timing.sync_busy;
                    wait_input.ota_flow_active = ota_flow_active_for_tick(
                        ota_timing.state,
                        ota_timing.status_hold_set,
                        settings_wait_now,
                        ota_timing.status_until_tick);
                    wait_input.ota_updating = ota_timing.state == kOtaUpdating;
                    wait_input.ota_status_hold_set =
                        ota_timing.status_hold_set;
                    const TickType_t settings_wait =
                        static_cast<TickType_t>(ui_settings_wait_ticks(
                            wait_input,
                            pdMS_TO_TICKS(kSettingsTimeoutMs),
                            pdMS_TO_TICKS(kOtaStatusMinIntervalMs)));
                    Lvgl_unlock();
                    (void)ulTaskNotifyTake(pdTRUE, settings_wait);
                    continue;
                }
            }

            if (visible_auxiliary_page == VisibleAuxiliaryPage::kSettings) {
                visible_auxiliary_page = VisibleAuxiliaryPage::kNone;
                restore_active_work_page_after_aux(false);
            }

            if (low_battery_resume_pending &&
                !battery.low_battery_mode &&
                !runtime_surfaces.setup_portal_active) {
                const int remembered_page = low_battery_resume_page;
                const int resume_page = is_work_page_enabled(remembered_page)
                                            ? remembered_page
                                            : first_enabled_work_page();
                active_work_page_store(resume_page);
                ensure_active_work_page_enabled();
                active_page = active_work_page_load();
                low_battery_resume_pending = false;
                ESP_LOGI(TAG,
                         UI_LOW_BATTERY_PAGE_RESTORE_LOG,
                         active_page,
                         remembered_page);
            }
            if (battery.low_battery_mode || runtime_surfaces.setup_portal_active) {
                if (active_page != kWorkPageWeatherClock) {
                    active_work_page_store(kWorkPageWeatherClock);
                }
            }
            active_page = active_work_page_load();
            ui_runtime_update_xiaozhi_auto_return(
                active_page,
                battery.low_battery_mode,
                runtime_surfaces,
                tick_now,
                xiaozhi_last_activity_tick,
                last_xiaozhi_activity_sequence);
            active_page = active_work_page_load();
            if (visible_work_page != active_page) {
                show_active_work_page();
                visible_work_page = active_page;
                apply_xiaozhi_page_activation(
                    visible_work_page == kWorkPageXiaozhiAI &&
                        !battery.low_battery_mode &&
                        !runtime_surfaces.setup_portal_active &&
                        !runtime_surfaces.auxiliary_page_requested(),
                    false,
                    xiaozhi_activation_requested,
                    xiaozhi_activation_request_valid);
                status_due = true;
                sensor_status_due = true;
                battery_due = true;
                battery_blink_due = true;
                invalidate_history_draw_cache();
                invalidate_flip_clock_time_sensor_draw_cache();
                invalidate_clock_date_draw_cache();
                refresh_now = true;
            }
            const bool setup_active_for_frame = runtime_surfaces.setup_portal_active;
            const bool normal_mode_for_frame = !battery.low_battery_mode &&
                                               !setup_active_for_frame;
            const ActiveWorkPageState active_pages = active_work_page_state_for_mode(
                active_page,
                normal_mode_for_frame);
            const WeatherCacheStatusSnapshot *visible_weather_cache = nullptr;
            const DailySayingCacheSnapshot *visible_saying_cache = nullptr;
            if (!setup_active_for_frame) {
                if (active_pages.uses_weather_data) {
                    const uint32_t weather_version = weather_state_version_load();
                    if (ui_visible_cache_status_refresh_due(
                            weather_cache_status_valid,
                            weather_cache_status.version,
                            weather_version)) {
                        (void)ui_visible_cache_snapshot_try_refresh(
                            &weather_cache_status,
                            &weather_cache_status_valid,
                            weather_cache_status_snapshot_load);
                    }
                    if (weather_cache_status_valid) {
                        visible_weather_cache = &weather_cache_status;
                    }
                }
                if (active_pages.uses_daily_saying) {
                    const uint32_t saying_version =
                        daily_saying_state_version_load();
                    if (ui_visible_cache_status_refresh_due(
                            saying_cache_status_valid,
                            saying_cache_status.version,
                            saying_version)) {
                        (void)ui_visible_cache_snapshot_try_refresh(
                            &saying_cache_status,
                            &saying_cache_status_valid,
                            daily_saying_cache_snapshot_load);
                    }
                    if (saying_cache_status_valid) {
                        visible_saying_cache = &saying_cache_status;
                    }
                }
                update_visible_weather_sync(active_pages,
                                            now,
                                            tick_now,
                                            visible_weather_cache,
                                            weather_sync_retry);
                update_visible_daily_saying_sync(active_pages,
                                                 now,
                                                 tick_now,
                                                 visible_saying_cache,
                                                 saying_sync_retry);

                refresh_now |= update_active_work_page_content(local,
                                                               active_pages,
                                                               active_page,
                                                               status_due,
                                                               current_status_snapshot,
                                                               battery.low_battery_mode,
                                                               setup_active_for_frame,
                                                               alert_visible,
                                                               alert_index,
                                                               alert_version);
            } else if (is_tm_plausible(local)) {
                refresh_now |= update_setup_clock_header_time_ui(local);
            } else {
                refresh_now |= update_active_work_page_invalid_time_labels(
                    active_page,
                    battery.low_battery_mode || setup_active_for_frame);
            }

            if (status_due || battery_due || battery_blink_due || setup_due || mode_due) {
                bool setup_active = runtime_surfaces.setup_portal_active;
                bool content_changed = false;
                if (setup_active != setup_panel_visible || mode_due) {
                    apply_clock_mode_visibility(setup_active,
                                                battery.low_battery_mode);
                    setup_panel_visible = setup_active;
                    low_mode_visible = battery.low_battery_mode;
                    status_due = true;
                    sensor_status_due = true;
                    invalidate_clock_time_draw_cache();
                    invalidate_clock_second_progress_draw_cache();
                    update_alert_pill(false,
                                      0,
                                      current_status_snapshot,
                                      battery.low_battery_mode,
                                      setup_active);
                    alert_visible = false;
                    alert_index = -1;
                    refresh_now = true;
                }
                if (setup_active) {
                    content_changed |= update_setup_status_panel();
                }
                if (!setup_active && !battery.low_battery_mode && active_pages.weather_clock) {
                    content_changed |= update_weather_clock_network_status(
                        current_status_snapshot.weather_network_bits);
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
                    if (sensor_status_due) {
                        if (active_pages.weather_clock) {
                            if (!setup_active && !battery.low_battery_mode) {
                                content_changed |= update_weather_clock_sensor_status();
                            }
                        } else {
                            content_changed |= update_non_clock_work_page_sensor_status(active_page);
                        }
                    }
                    if (active_pages.weather_clock) {
                        content_changed |= update_top_status_icons(
                            alert_visible,
                            current_status_snapshot,
                            battery.low_battery_mode,
                            setup_active);
                    } else {
                        content_changed |= update_work_page_status_icons(
                            active_page,
                            current_status_snapshot,
                            battery.low_battery_mode,
                            setup_active);
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
            start_setup_prompt_after_ui = setup_panel_visible &&
                                          setup_prompt_playback_pending();
            Lvgl_unlock();
        } else {
            lvgl_lock_failures =
                saturating_increment_u8(lvgl_lock_failures);
        }
        if (start_setup_prompt_after_ui) {
            (void)start_setup_prompt_playback();
        }
        TickType_t delay_ticks = lvgl_locked
                                     ? ui_runtime_next_loop_delay_ticks(
                                           now,
                                           battery,
                                           battery_blink_visible,
                                           active_page,
                                           runtime_surfaces)
                                     : pdMS_TO_TICKS(ui_lvgl_lock_retry_delay_ms(
                                           lvgl_lock_failures));
        delay_ticks = app_tick_nonzero_delay(delay_ticks);
        ulTaskNotifyTake(pdTRUE, delay_ticks);
    }
}
