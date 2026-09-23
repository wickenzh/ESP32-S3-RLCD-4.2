// 调度 NTP、天气、预警、每日文字和 OTA 等联网同步流程。
#include "network_sync_task.h"

#include "app_constexpr.h"
#include "app_event_group.h"
#include "app_metadata.h"
#include "chime_runtime_state.h"
#include "daily_saying_state.h"
#include "daily_saying_service.h"
#include "ota_runtime_state.h"

#include "network_https_resources.h"
#include "network_https_resources_internal.h"
#include "network_boot_refresh_state.h"
#include "network_cache_policy.h"
#include "network_diagnostics_internal.h"
#include "ntp_services.h"
#include "network_provisioning_session_internal.h"
#include "provisioning_validation.h"
#include "network_diagnostics_catalog.h"
#include "network_ntp_schedule_state.h"
#include "network_sync_requests.h"
#include "network_sync_requests_internal.h"
#include "network_sync_runtime.h"
#include "network_sync_schedule.h"
#include "network_sync_wait.h"
#include "network_task_guards.h"
#include "sensor_time.h"
#include "setup_portal_control.h"
#include "startup_state.h"
#include "ui_work_page_catalog.h"
#include "weather_state.h"
#include "weather_update.h"
#include "weather_wifi_ab_test.h"
#include "wifi_idle_stop_policy.h"
#include "wifi_portal_state.h"
#include "wifi_radio_services_internal.h"

#include <esp_log.h>
#include <esp_timer.h>

static constexpr uint32_t kNetworkShortRetryWaitMs = 1000;
static constexpr uint32_t kNetworkTaskStartupDelayMs = 2500;
static constexpr time_t kBootWeatherRefreshDelaySec = 10;
static constexpr time_t kBootSayingRefreshDelaySec = 25;
static constexpr time_t kBootHttpsInterRequestGapSec = 8;
static_assert(kBootWeatherRefreshDelaySec > 0,
              "boot weather refresh delay must be positive");
static_assert(kBootSayingRefreshDelaySec > kBootWeatherRefreshDelaySec,
              "boot saying refresh delay must stay after boot weather refresh");
static_assert(kBootHttpsInterRequestGapSec > 0,
              "boot HTTPS inter-request gap must be positive");
static_assert(kNetworkDiagOtaLine == kNetworkDiagLineCount - 1,
              "network service diagnostics line mapping must match diagnostics line count");
static constexpr const char *kNetworkDiagIpLocationWifiStartFailed = "IP定位: WiFi启动失败";
static constexpr const char *kNetworkDiagIpLocationPowerLockUnavailable = "IP定位: 系统繁忙";
static constexpr const char *kNetworkDiagIpLocationWifiConnectTimeout = "IP定位: WiFi连接超时";
static constexpr const char *kNetworkSyncWeatherConfigMissing = "未配置 API Key";
#define NETWORK_BOOT_REFRESH_SCHEDULED_FORMAT "boot network refresh scheduled: weather=%d saying=%d"
#define NETWORK_BOOT_HTTPS_MEMORY_DEFERRED_FORMAT \
    "background boot HTTPS deferred: delay=%llds deferrals=%u internal_free=%u internal_largest=%u dma_largest=%u"
#define NETWORK_BOOT_SAYING_STAGGERED_FORMAT \
    "boot daily saying deferred %lld seconds after weather"
#define NETWORK_BOOT_WEATHER_RESOURCE_RETRY_FORMAT \
    "boot weather resource retry deferred %lld seconds deferrals=%u"
#define NETWORK_BOOT_REFRESH_FAILURE_RETRY_FORMAT \
    "boot %s retry scheduled: delay=%llds failures=%u"
#define NETWORK_BOOT_REFRESH_FAILURE_EXHAUSTED_FORMAT \
    "boot %s retry exhausted: failures=%u"
#define NETWORK_NTP_RETRY_SCHEDULED_FORMAT \
    "ntp retry scheduled: delay=%llds time_valid=%d failures=%u"
static constexpr const char *kNetworkDiagWifiOnLog = "wifi radio on for network diagnostics";
#define NETWORK_SYNC_WIFI_ON_FORMAT "wifi radio on for sync: ntp=%d weather=%d saying=%d boot_weather=%d boot_saying=%d"
static constexpr const char *kNetworkSyncWifiStartFailedLog = "wifi start failed during sync window";
static constexpr const char *kNetworkSyncPowerLockUnavailableLog =
    "network PM lock unavailable during sync window";
static constexpr const char *kNetworkSyncWifiConnectTimeoutLog = "wifi connect timeout during sync window";
static constexpr const char *kNetworkSyncContextChangedLog =
    "network sync deferred after runtime state change";
#define PROVISIONING_BACKGROUND_REFRESH_FORMAT \
    "provisioning background refresh scheduled: ntp=1 weather=%d saying=%d"

struct NetworkSyncRuntimeState {
    NetworkNtpScheduleState ntp;
    NetworkBootRefreshState boot_refresh;
    uint8_t wifi_stop_retry_failures = 0;
    uint8_t setup_portal_start_retry_failures = 0;
    uint8_t setup_portal_stop_retry_failures = 0;
};

static WorkPageDataRequirements capture_network_refresh_page_availability()
{
    return enabled_work_page_data_requirements(
        work_page_enabled_mask_load());
}

static_assert(kWorkPageCount <= 8,
              "network refresh page snapshot must fit the enabled-page mask");

static void finish_ntp_attempt(bool succeeded,
                               bool retry_required,
                               NetworkSyncRuntimeState &runtime)
{
    const bool time_valid =
        succeeded || !retry_required || is_system_time_plausible();
    time_t now = 0;
    if (succeeded || retry_required) {
        time(&now);
    }
    const NetworkNtpRetryUpdate update = finish_network_ntp_attempt(
        &runtime.ntp,
        succeeded,
        retry_required,
        time_valid,
        now);
    if (update.scheduled) {
        ESP_LOGI(TAG,
                 NETWORK_NTP_RETRY_SCHEDULED_FORMAT,
                 static_cast<long long>(update.delay_seconds),
                 time_valid,
                 static_cast<unsigned>(runtime.ntp.retry_failures));
    }
}

static bool weather_cache_complete_for_current_hour(time_t now,
                                                    bool require_extended_weather,
                                                    bool require_air)
{
    WeatherCacheStatusSnapshot snapshot;
    return weather_cache_status_snapshot_load(&snapshot) &&
           network_weather_cache_current_hour(now, snapshot.last_sync_time) &&
           (!require_extended_weather ||
            (require_air ? snapshot.extended_data_ready
                                                       : snapshot.forecast_data_ready));
}

static bool saying_cache_current_day(time_t now)
{
    DailySayingCacheSnapshot snapshot;
    if (!daily_saying_cache_snapshot_load(&snapshot) || !snapshot.available) {
        return false;
    }
    return network_daily_saying_cache_current_day(now, snapshot.last_sync_time);
}

static void reconcile_automatic_boot_refreshes(
    time_t now,
    bool blocked,
    NetworkSyncRuntimeState &runtime)
{
    if (!runtime.boot_refresh.weather_due &&
        !runtime.boot_refresh.saying_due) {
        return;
    }
    if (blocked) {
        clear_network_boot_refreshes(&runtime.boot_refresh);
        return;
    }
    const WorkPageDataRequirements pages =
        capture_network_refresh_page_availability();
    const bool weather_cache_current =
        runtime.boot_refresh.weather_due &&
        weather_cache_complete_for_current_hour(now,
                                                pages.extended_weather, pages.air_quality);
    const bool saying_cache_current =
        runtime.boot_refresh.saying_due &&
        saying_cache_current_day(now);
    reconcile_network_boot_refresh_state(
        &runtime.boot_refresh,
        pages.weather,
        pages.daily_saying,
        weather_cache_current,
        saying_cache_current,
        false);
}

static bool automatic_boot_refresh_page_disabled(
    const NetworkSyncSchedule &schedule,
    const NetworkSyncRequestSnapshot &requests)
{
    if (requests.provisioning ||
        (!schedule.boot_weather_ready && !schedule.boot_saying_ready)) {
        return false;
    }
    NetworkAutomaticBootPageInput input = {};
    const WorkPageDataRequirements pages =
        capture_network_refresh_page_availability();
    input.provisioning_sync_due = requests.provisioning;
    input.explicit_weather_due = requests.weather_due();
    input.explicit_saying_due = requests.saying_due();
    input.weather_page_enabled = pages.weather;
    input.saying_page_enabled = pages.daily_saying;
    return network_automatic_boot_refresh_page_disabled(schedule, input);
}

static void finish_network_radio_session(NetworkAwakeLockGuard &awake_lock,
                                         bool force_setup_portal = false)
{
    // Keep the CPU awake through esp_wifi_stop(). If another protected owner
    // blocks this attempt, retain a real close request until that owner exits.
    if (awake_lock.locked()) {
        stop_wifi_radio(force_setup_portal);
        awake_lock.release();
        request_wifi_radio_stop_if_running();
    } else {
        // This caller never acquired session ownership, so it must not create a
        // new stop request; it may only service one left by an earlier owner.
        service_wifi_radio_stop_when_idle();
    }
}

static void schedule_background_refresh_after_provisioning(
    bool have_weather_config,
    NetworkSyncRuntimeState &runtime)
{
    time_t now = 0;
    time(&now);
    const WorkPageDataRequirements pages =
        capture_network_refresh_page_availability();
    schedule_network_ntp_after_provisioning(&runtime.ntp);
    schedule_network_boot_refreshes(
        &runtime.boot_refresh,
        have_weather_config && pages.weather,
        pages.daily_saying,
        now + kBootWeatherRefreshDelaySec,
        now + kBootSayingRefreshDelaySec);
    ESP_LOGI(TAG,
             PROVISIONING_BACKGROUND_REFRESH_FORMAT,
             runtime.boot_refresh.weather_due,
             runtime.boot_refresh.saying_due);
}

static bool settle_between_network_operations(bool more_work_pending)
{
    if (!more_work_pending) {
        return true;
    }
    // The preceding client has already released its TLS buffers. Give the
    // allocator and UI task a scheduling window before the next operation.
    // The first minute uses a longer gap because boot UI, sensor and cache
    // initialization still compete for internal/DMA memory.
    const bool startup_pressure = network_startup_pressure_window_active(
        startup_screen_active(),
        esp_timer_get_time());
    return wait_for_network_sync_settle(
        network_inter_operation_settle_delay_ms(startup_pressure));
}

static bool defer_automatic_boot_https_for_memory(NetworkSyncSchedule *schedule,
                                                  const NetworkSyncRequestSnapshot &requests,
                                                  time_t now,
                                                  NetworkSyncRuntimeState &runtime)
{
    if (!schedule) {
        return false;
    }
    NetworkBootHttpsDeferralInput input = {};
    input.now = now;
    input.provisioning_sync_due = requests.provisioning;
    input.manual_weather_due = requests.weather_due();
    input.manual_saying_due = requests.saying_due();
    if (!network_automatic_boot_https_pending(*schedule, input)) {
        defer_network_boot_refreshes_for_memory(
            &runtime.boot_refresh,
            schedule,
            input,
            true);
        return false;
    }
    const NetworkHttpsMemorySnapshot memory = capture_network_https_memory_snapshot();
    const bool memory_allowed = network_automatic_boot_https_allowed(
        startup_screen_active(),
        esp_timer_get_time(),
        memory.internal_free,
        memory.internal_largest,
        memory.dma_largest);
    const NetworkBootHttpsMemoryDeferralUpdate update =
        defer_network_boot_refreshes_for_memory(
            &runtime.boot_refresh,
            schedule,
            input,
            memory_allowed);
    if (!update.deferred) {
        return false;
    }
    ESP_LOGW(TAG,
             NETWORK_BOOT_HTTPS_MEMORY_DEFERRED_FORMAT,
             static_cast<long long>(update.delay_seconds),
             static_cast<unsigned>(update.deferral_count),
             static_cast<unsigned>(memory.internal_free),
             static_cast<unsigned>(memory.internal_largest),
             static_cast<unsigned>(memory.dma_largest));
    return true;
}

static void stagger_boot_saying_after_weather(const NetworkSyncSchedule &schedule,
                                              NetworkSyncRuntimeState &runtime)
{
    time_t now = 0;
    time(&now);
    if (!stagger_network_boot_saying(
            &runtime.boot_refresh,
            schedule.stagger_boot_saying_after_weather,
            now,
            kBootHttpsInterRequestGapSec)) {
        return;
    }
    ESP_LOGI(TAG,
             NETWORK_BOOT_SAYING_STAGGERED_FORMAT,
             static_cast<long long>(kBootHttpsInterRequestGapSec));
}

static void log_boot_refresh_attempt_update(
    const char *service,
    const NetworkBootWeatherAttemptUpdate &update)
{
    if (update.failure_count == 0) {
        return;
    }
    if (update.retry_scheduled) {
        ESP_LOGW(TAG,
                 NETWORK_BOOT_REFRESH_FAILURE_RETRY_FORMAT,
                 service,
                 static_cast<long long>(update.delay_seconds),
                 static_cast<unsigned>(update.failure_count));
    } else if (update.retry_exhausted) {
        ESP_LOGW(TAG,
                 NETWORK_BOOT_REFRESH_FAILURE_EXHAUSTED_FORMAT,
                 service,
                 static_cast<unsigned>(update.failure_count));
    }
}

static void finalize_failed_network_sync_attempt(
    const NetworkSyncSchedule &schedule,
    const NetworkSyncRequestSnapshot &requests,
    NetworkSyncRuntimeState &runtime)
{
    time_t now = 0;
    if (schedule.boot_weather_ready || schedule.boot_saying_ready) {
        time(&now);
    }
    const NetworkBootWeatherAttemptUpdate weather_update =
        finish_network_boot_weather_attempt(
            &runtime.boot_refresh,
            schedule.boot_weather_ready,
            false,
            false,
            now);
    const NetworkBootSayingAttemptUpdate saying_update =
        finish_network_boot_saying_attempt(
            &runtime.boot_refresh,
            schedule.boot_saying_ready,
            false,
            now);
    log_boot_refresh_attempt_update("weather", weather_update);
    log_boot_refresh_attempt_update("daily saying", saying_update);
    finish_failed_sync_requests(requests);
    if (schedule.ntp_due) {
        finish_ntp_attempt(false, schedule.ntp_retry_required, runtime);
    }
    stagger_boot_saying_after_weather(schedule, runtime);
}

static void finish_failed_network_sync_session(
    const NetworkSyncSchedule &schedule,
    const NetworkSyncRequestSnapshot &requests,
    NetworkSyncRuntimeState &runtime,
    NetworkAwakeLockGuard &awake_lock,
    WifiPortalSaveResult provisioning_result)
{
    finalize_failed_network_sync_attempt(schedule, requests, runtime);
    if (requests.provisioning) {
        keep_setup_portal_after_provisioning_failure(awake_lock,
                                                     provisioning_result,
                                                     requests.provisioning_generation);
        return;
    }
    finish_network_radio_session(awake_lock);
}

static bool execute_connected_sync_window(const NetworkSyncSchedule &schedule,
                                          const NetworkSyncRequestSnapshot &requests,
                                          NetworkSyncRuntimeState &runtime)
{
    bool ntp_ok = false;
    bool weather_ok = false;
    bool weather_resource_deferred = false;
    bool saying_ok = false;
    NetworkDisplayDmaGuard display_guard(schedule.weather_due || schedule.saying_due);
    if (schedule.ntp_due) {
        const bool ntp_succeeded = perform_ntp_sync();
        if (!network_sync_continuation_allowed()) {
            return false;
        }
        if (ntp_succeeded) {
            ntp_ok = true;
        }
        finish_ntp_attempt(ntp_succeeded,
                           schedule.ntp_retry_required,
                           runtime);
        if (!settle_between_network_operations(schedule.weather_due ||
                                               schedule.saying_due)) {
            return false;
        }
    }
    if (schedule.weather_due) {
        const WorkPageDataRequirements pages =
            capture_network_refresh_page_availability();
        const WeatherUpdateScope scope =
            requests.manual_weather || pages.extended_weather
                ? WeatherUpdateScope::kFull
                : WeatherUpdateScope::kCurrentAndAlerts;
        const int64_t weather_started_us = esp_timer_get_time();
        WeatherUpdateResult result;
        bool performance_enabled = false;
#ifdef WEATHER_CLOCK_WIFI_AB_TEST
        const int ab_slot = weather_wifi_ab_slot(weather_started_us);
        if (ab_slot >= 0) {
            performance_enabled = ab_slot % 2 != 0;
            ESP_LOGI(TAG, "weather AB: slot=%d mode=%s", ab_slot,
                     performance_enabled ? "NONE" : "MAX_MODEM");
        }
#endif
        {
            WeatherWifiPerformanceGuard performance_guard(performance_enabled);
            result = perform_weather_update(scope);
        }
#ifdef WEATHER_CLOCK_WIFI_AB_TEST
        if (ab_slot >= 0) {
            weather_wifi_ab_last_slot.store(ab_slot);
        }
#endif
        ESP_LOGI(TAG, "weather request window: elapsed_ms=%lld result=%d",
                 static_cast<long long>((esp_timer_get_time() - weather_started_us) / 1000),
                 static_cast<int>(result));
        if (!network_sync_continuation_allowed()) {
            return false;
        }
        weather_ok = result == WeatherUpdateResult::kSuccess;
        weather_resource_deferred = result == WeatherUpdateResult::kResourceDeferred;
        if (!settle_between_network_operations(schedule.saying_due)) {
            return false;
        }
    }
    if (schedule.saying_due) {
        saying_ok = perform_daily_saying_update();
        if (!network_sync_continuation_allowed()) {
            return false;
        }
    }
    time_t now = 0;
    if (schedule.boot_weather_ready || schedule.boot_saying_ready) {
        time(&now);
    }
    const NetworkBootWeatherAttemptUpdate weather_update =
        finish_network_boot_weather_attempt(
            &runtime.boot_refresh,
            schedule.boot_weather_ready,
            weather_ok,
            weather_resource_deferred,
            now);
    if (weather_update.retry_scheduled) {
        ESP_LOGI(
            TAG,
            NETWORK_BOOT_WEATHER_RESOURCE_RETRY_FORMAT,
            static_cast<long long>(weather_update.delay_seconds),
            static_cast<unsigned>(weather_update.deferral_count));
    }
    log_boot_refresh_attempt_update("weather", weather_update);
    const NetworkBootSayingAttemptUpdate saying_update =
        finish_network_boot_saying_attempt(
            &runtime.boot_refresh,
            schedule.boot_saying_ready,
            saying_ok,
            now);
    log_boot_refresh_attempt_update("daily saying", saying_update);
    finish_successful_sync_requests(requests,
                                    ntp_ok,
                                    weather_ok,
                                    saying_ok);
    return true;
}

static bool execute_network_diagnostics_window(
    const NetworkSyncAvailability &scheduled_runtime,
    const NetworkSyncRequestSnapshot &requests)
{
    ESP_LOGI(TAG, "%s", kNetworkDiagWifiOnLog);
    const char *unavailable_reason = nullptr;
    network_diag_begin();
    if (!network_diagnostics_request_pending()) {
        network_diag_reset();
        return false;
    }
    NetworkAwakeLockGuard awake_lock;
    if (!awake_lock.locked()) {
        unavailable_reason = kNetworkDiagIpLocationPowerLockUnavailable;
    } else if (!start_wifi_radio(false)) {
        unavailable_reason = kNetworkDiagIpLocationWifiStartFailed;
    } else {
        const NetworkSyncConnectionWaitResult connection_wait =
            wait_for_valid_network_sync_connection(scheduled_runtime,
                                                   requests,
                                                   network_sync_connection_timeout_ms(true));
        if (connection_wait == NetworkSyncConnectionWaitResult::kRuntimeChanged) {
            ESP_LOGI(TAG, "%s", kNetworkSyncContextChangedLog);
            finish_network_radio_session(awake_lock);
            return false;
        }
        if (connection_wait == NetworkSyncConnectionWaitResult::kConnected) {
            if (!network_sync_request_snapshot_still_current(requests)) {
                ESP_LOGI(TAG, "%s", kNetworkSyncContextChangedLog);
                finish_network_radio_session(awake_lock);
                return false;
            }
            if (!run_network_diagnostic_checks(
                    requests.diagnostics_generation)) {
                ESP_LOGI(TAG, "%s", kNetworkSyncContextChangedLog);
                finish_network_radio_session(awake_lock);
                return false;
            }
        } else {
            unavailable_reason = kNetworkDiagIpLocationWifiConnectTimeout;
        }
    }
    finish_network_radio_session(awake_lock);
    if (unavailable_reason) {
        network_diag_finish_unavailable(unavailable_reason);
    } else {
        network_diag_finish();
    }
    finish_network_diagnostics_sync(requests);
    return true;
}

static void wait_for_network_runtime_or_wifi_stop_retry(
    WifiRadioIdleStopResult wifi_stop_result,
    uint8_t wifi_stop_retry_failures)
{
    if (wifi_stop_result == WifiRadioIdleStopResult::kRetryRequired) {
        wait_for_network_sync_event(
            wifi_idle_stop_retry_delay_ms(wifi_stop_retry_failures));
    } else {
        wait_for_network_runtime_request();
    }
}

void network_sync_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(kNetworkTaskStartupDelayMs));
    EventBits_t initial_bits = app_event_group_get_bits();
    time_t boot_schedule_now = 0;
    time(&boot_schedule_now);
    NetworkSyncRuntimeState sync_runtime;
    sync_runtime.ntp = initialize_network_ntp_schedule(
        (initial_bits & kTimeSyncedBit) != 0,
        boot_schedule_now);
    const NetworkSyncAvailability initial_runtime =
        capture_network_runtime_availability();
    const WorkPageDataRequirements initial_pages =
        capture_network_refresh_page_availability();
    sync_runtime.boot_refresh = initialize_network_boot_refresh_state(
        initial_runtime.have_wifi_creds &&
            initial_runtime.have_weather_config &&
            !initial_runtime.offline_mode &&
            !initial_runtime.low_battery_mode &&
            initial_pages.weather,
        initial_runtime.have_wifi_creds &&
            !initial_runtime.offline_mode &&
            !initial_runtime.low_battery_mode &&
            initial_pages.daily_saying,
        boot_schedule_now + kBootWeatherRefreshDelaySec,
        boot_schedule_now + kBootSayingRefreshDelaySec);
    if (sync_runtime.boot_refresh.weather_due ||
        sync_runtime.boot_refresh.saying_due) {
        ESP_LOGI(TAG,
                 NETWORK_BOOT_REFRESH_SCHEDULED_FORMAT,
                 sync_runtime.boot_refresh.weather_due,
                 sync_runtime.boot_refresh.saying_due);
    }

    for (;;) {
        // Consume only the edge-like state notification before reading the
        // latest runtime state. Sync request bits stay level-triggered.
        app_event_group_clear_bits(kNetworkStateChangedBit);
        const WifiRadioIdleStopResult wifi_stop_result =
            service_wifi_radio_stop_when_idle();
        if (wifi_stop_result == WifiRadioIdleStopResult::kRetryRequired) {
            sync_runtime.wifi_stop_retry_failures =
                saturating_increment_u8(
                    sync_runtime.wifi_stop_retry_failures);
        } else {
            sync_runtime.wifi_stop_retry_failures = 0;
        }
        const SetupPortalStopResult setup_portal_stop =
            service_setup_portal_stop_request();
        if (setup_portal_stop != SetupPortalStopResult::kNoRequest) {
            if (setup_portal_stop == SetupPortalStopResult::kRetryPending) {
                sync_runtime.setup_portal_stop_retry_failures =
                    saturating_increment_u8(
                        sync_runtime.setup_portal_stop_retry_failures);
                wait_for_setup_portal_retry(
                    sync_runtime.setup_portal_stop_retry_failures);
            } else {
                sync_runtime.setup_portal_stop_retry_failures = 0;
            }
            continue;
        }
        sync_runtime.setup_portal_stop_retry_failures = 0;
        NetworkSyncRequestSnapshot requests = snapshot_network_sync_requests();
        const NetworkSyncAvailability runtime =
            capture_network_runtime_availability();
        int ota_state = ota_runtime_state_load();
        if (ota_blocks_background_network_sync(ota_state)) {
            wait_for_ota_network_block_change();
            continue;
        }
        const SetupPortalStartResult setup_portal_start =
            service_setup_portal_start_request();
        if (setup_portal_start != SetupPortalStartResult::kNoRequest) {
            if (setup_portal_start == SetupPortalStartResult::kRetryPending) {
                sync_runtime.setup_portal_start_retry_failures =
                    saturating_increment_u8(
                        sync_runtime.setup_portal_start_retry_failures);
                wait_for_setup_portal_retry(
                    sync_runtime.setup_portal_start_retry_failures);
            } else {
                sync_runtime.setup_portal_start_retry_failures = 0;
                wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            }
            continue;
        }
        sync_runtime.setup_portal_start_retry_failures = 0;
        if (runtime.offline_mode) {
            clear_network_boot_refreshes(&sync_runtime.boot_refresh);
            finish_offline_network_requests(requests);
            wait_for_network_runtime_or_wifi_stop_retry(
                wifi_stop_result,
                sync_runtime.wifi_stop_retry_failures);
            continue;
        }
        if (!runtime.have_wifi_creds) {
            clear_network_boot_refreshes(&sync_runtime.boot_refresh);
            finish_unconfigured_network_requests(requests);
            wait_for_network_runtime_or_wifi_stop_retry(
                wifi_stop_result,
                sync_runtime.wifi_stop_retry_failures);
            continue;
        }
        if (requests.weather_due() && !runtime.have_weather_config) {
            if (requests.manual_weather) {
                finish_settings_sync_and_clear_bit(kSettingsSyncWeather,
                                                   kNetworkSyncWeatherConfigMissing,
                                                   kManualWeatherSyncBit,
                                                   requests.manual_weather_generation,
                                                   requests.manual_weather_settings_generation);
            }
            if (requests.visible_weather) {
                app_event_group_clear_bits(kVisibleWeatherSyncBit);
            }
        }
        // A diagnostics request may have been queued just before low-battery
        // mode became active. Finish nonessential requests before selecting a
        // network window, then resnapshot so cleared bits cannot run stale.
        if (runtime.low_battery_mode) {
            finish_low_battery_network_requests(requests);
            requests = snapshot_network_sync_requests();
        }
        if (setup_portal_active_load()) {
            if (!requests.provisioning) {
                wait_for_active_setup_portal_request();
                continue;
            }
            // A request can be raised between clearing stale work and actually
            // starting the AP. Preserve those level bits for later, but keep
            // this connected window exclusively owned by provisioning.
            requests = requests.provisioning_only();
        }

        if (requests.diagnostics) {
            const NetworkSyncAvailability diagnostics_runtime =
                capture_network_runtime_availability();
            if (network_sync_start_context_changed(runtime, diagnostics_runtime)) {
                ESP_LOGI(TAG, "%s", kNetworkSyncContextChangedLog);
                continue;
            }
            if (!execute_network_diagnostics_window(diagnostics_runtime,
                                                    requests)) {
                continue;
            }
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }

        time_t now;
        time(&now);
        struct tm local = {};
        bool time_valid = localtime_r(&now, &local) && is_tm_plausible(local);
        refresh_network_ntp_daily_due(&sync_runtime.ntp,
                                      now,
                                      time_valid);
        // A short boot request may obtain current conditions while forecast or
        // air quality times out. Keep the staggered full refresh scheduled in
        // that partial state so the weather board is ready before first entry.
        // Disabled pages and blocked runtime states clear only automatic boot
        // work; explicit settings requests remain independently level-triggered.
        reconcile_automatic_boot_refreshes(
            now,
            runtime.low_battery_mode,
            sync_runtime);
        NetworkSyncScheduleInput schedule_input = {};
        schedule_input.now = now;
        schedule_input.next_ntp_retry_at = sync_runtime.ntp.next_retry_at;
        schedule_input.boot_weather_due_at =
            sync_runtime.boot_refresh.weather_due_at;
        schedule_input.boot_saying_due_at =
            sync_runtime.boot_refresh.saying_due_at;
        schedule_input.have_weather_config = runtime.have_weather_config;
        schedule_input.low_battery_mode = runtime.low_battery_mode;
        schedule_input.provisioning_sync_due = requests.provisioning;
        schedule_input.manual_ntp_due = requests.manual_ntp;
        schedule_input.manual_weather_due = requests.weather_due();
        schedule_input.manual_saying_due = requests.saying_due();
        schedule_input.boot_ntp_due = sync_runtime.ntp.boot_due;
        schedule_input.daily_ntp_due = sync_runtime.ntp.daily_pending;
        schedule_input.boot_weather_due =
            sync_runtime.boot_refresh.weather_due;
        schedule_input.boot_saying_due =
            sync_runtime.boot_refresh.saying_due;
        NetworkSyncSchedule schedule = calculate_network_sync_schedule(schedule_input);
        defer_automatic_boot_https_for_memory(
            &schedule,
            requests,
            now,
            sync_runtime);

        if (!schedule.ntp_due && !schedule.weather_due && !schedule.saying_due) {
            uint32_t wait_ms = network_idle_wait_ms(
                now,
                schedule.next_boot_due_at,
                sync_runtime.ntp.next_retry_at,
                sync_runtime.ntp.next_daily_at);
            const uint32_t wifi_stop_retry_wait_ms =
                wifi_idle_stop_retry_delay_ms(
                    sync_runtime.wifi_stop_retry_failures);
            if (wifi_stop_result == WifiRadioIdleStopResult::kRetryRequired &&
                wait_ms > wifi_stop_retry_wait_ms) {
                wait_ms = wifi_stop_retry_wait_ms;
            }
            wait_for_network_sync_event(wait_ms);
            continue;
        }

        const bool interactive_request = requests.provisioning ||
                                         requests.manual_ntp ||
                                         requests.manual_weather ||
                                         requests.manual_saying;
        const ChimeRuntimeSnapshot chime = chime_runtime_snapshot_load();
        const uint32_t hourly_weather_stagger_ms =
            network_hourly_weather_stagger_delay_ms(
                schedule.weather_due && !interactive_request,
                chime.hourly_enabled,
                time_valid ? local.tm_min : -1,
                time_valid ? local.tm_sec : -1);
        if (hourly_weather_stagger_ms > 0) {
            wait_for_network_stagger_interrupt(hourly_weather_stagger_ms);
            continue;
        }

        const NetworkSyncAvailability current_runtime =
            capture_network_runtime_availability();
        if (network_sync_start_context_changed(runtime, current_runtime)) {
            ESP_LOGI(TAG, "%s", kNetworkSyncContextChangedLog);
            continue;
        }
        if (!network_sync_request_snapshot_still_current(requests)) {
            continue;
        }
        if (automatic_boot_refresh_page_disabled(schedule, requests)) {
            continue;
        }

        ESP_LOGI(TAG,
                 NETWORK_SYNC_WIFI_ON_FORMAT,
                 schedule.ntp_due,
                 schedule.weather_due,
                 schedule.saying_due,
                 schedule.boot_weather_ready,
                 schedule.boot_saying_ready);
        NetworkAwakeLockGuard awake_lock;
        if (!awake_lock.locked() || !start_wifi_radio(requests.provisioning)) {
            ESP_LOGW(TAG,
                     "%s",
                     awake_lock.locked()
                         ? kNetworkSyncWifiStartFailedLog
                         : kNetworkSyncPowerLockUnavailableLog);
            finish_failed_network_sync_session(
                schedule,
                requests,
                sync_runtime,
                awake_lock,
                WifiPortalSaveResult::kWifiConnectionFailed);
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }
        const NetworkSyncConnectionWaitResult connection_wait =
            wait_for_valid_network_sync_connection(current_runtime,
                                                   requests,
                                                   network_sync_connection_timeout_ms(
                                                       interactive_request));
        if (connection_wait == NetworkSyncConnectionWaitResult::kRuntimeChanged) {
            ESP_LOGI(TAG, "%s", kNetworkSyncContextChangedLog);
            finish_network_radio_session(awake_lock);
            continue;
        }
        if (connection_wait == NetworkSyncConnectionWaitResult::kConnected) {
            if (!network_sync_request_snapshot_still_current(requests)) {
                ESP_LOGI(TAG, "%s", kNetworkSyncContextChangedLog);
                finish_network_radio_session(awake_lock);
                continue;
            }
            if (automatic_boot_refresh_page_disabled(schedule, requests)) {
                finish_network_radio_session(awake_lock);
                continue;
            }
            if (requests.provisioning) {
                WifiPortalSaveResult validation_result =
                    validate_saved_provisioning_weather_configuration();
                if (validation_result != WifiPortalSaveResult::kSuccess) {
                    finish_failed_network_sync_session(schedule,
                                                       requests,
                                                       sync_runtime,
                                                       awake_lock,
                                                       validation_result);
                    wait_for_network_sync_event(kNetworkShortRetryWaitMs);
                    continue;
                }
                if (!publish_setup_portal_result(
                        WifiPortalSaveResult::kSuccess,
                        requests.provisioning_generation)) {
                    complete_provisioning_sync_request(
                        requests.provisioning_generation);
                    finish_network_radio_session(awake_lock);
                    continue;
                }
                complete_provisioning_sync_request(
                    requests.provisioning_generation);
                if (!wait_for_provisioning_result_feedback(
                        requests.provisioning_generation)) {
                    finish_network_radio_session(awake_lock);
                    continue;
                }
                finish_network_radio_session(awake_lock, true);
                schedule_background_refresh_after_provisioning(
                    runtime.have_weather_config,
                    sync_runtime);
                continue;
            }
            if (!execute_connected_sync_window(schedule,
                                               requests,
                                               sync_runtime)) {
                ESP_LOGI(TAG, "%s", kNetworkSyncContextChangedLog);
                finish_network_radio_session(awake_lock);
                continue;
            }
            stagger_boot_saying_after_weather(schedule, sync_runtime);
        } else {
            ESP_LOGW(TAG, "%s", kNetworkSyncWifiConnectTimeoutLog);
            finish_failed_network_sync_session(
                schedule,
                requests,
                sync_runtime,
                awake_lock,
                WifiPortalSaveResult::kWifiConnectionFailed);
            if (requests.provisioning) {
                wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            }
            continue;
        }
        finish_network_radio_session(awake_lock, requests.provisioning);
    }
}
