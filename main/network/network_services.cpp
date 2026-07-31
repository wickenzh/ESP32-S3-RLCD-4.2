// 调度 NTP、天气、预警、每日文字和 OTA 等联网同步流程。
#include "network_services.h"

#include "ota_runtime_state.h"

#include "network_https_resources.h"
#include "network_credentials_state.h"
#include "network_diagnostics_catalog.h"
#include "network_sync_requests.h"
#include "network_sync_schedule.h"
#include "network_task_guards.h"
#include "sensor_services.h"
#include "sensor_time.h"
#include "startup_state.h"
#include "ui_views.h"
#include "wifi_portal_state.h"
#include "wifi_radio_state.h"

static constexpr time_t kSecondsPerMinute = 60;
static constexpr time_t kMinutesPerHour = 60;
static constexpr time_t kHoursPerDay = 24;
static constexpr time_t kSecondsPerHour = kMinutesPerHour * kSecondsPerMinute;
static constexpr time_t kSecondsPerDay = kHoursPerDay * kSecondsPerHour;
static constexpr uint32_t kNetworkNoWorkWaitMs = 30000;
static constexpr uint32_t kNetworkShortRetryWaitMs = 1000;
static constexpr uint32_t kNetworkWifiConnectTimeoutMs = 45000;
static constexpr uint32_t kNetworkTaskStartupDelayMs = 2500;
static constexpr uint32_t kNetworkBootSyncGateWarningMs = 1000;
static constexpr EventBits_t kNetworkSyncWakeBits = kProvisioningSyncBit |
                                                     kManualNtpSyncBit |
                                                     kManualWeatherSyncBit |
                                                     kManualSayingSyncBit |
                                                     kNetworkDiagBit |
                                                     kNetworkStateChangedBit;
static constexpr time_t kNetworkNtpRetryDelaySec = 5 * kSecondsPerMinute;
static constexpr time_t kBootWeatherRefreshDelaySec = 10;
static constexpr time_t kBootSayingRefreshDelaySec = 25;
static constexpr time_t kBootHttpsInterRequestGapSec = 8;
static constexpr uint32_t kBootHttpsMemoryRetryMs = 10000;
static_assert(kBootWeatherRefreshDelaySec > 0,
              "boot weather refresh delay must be positive");
static_assert(kBootSayingRefreshDelaySec > kBootWeatherRefreshDelaySec,
              "boot saying refresh delay must stay after boot weather refresh");
static_assert(kBootHttpsInterRequestGapSec > 0,
              "boot HTTPS inter-request gap must be positive");
static_assert(kBootHttpsMemoryRetryMs >= 1000,
              "boot HTTPS memory retry must avoid a tight loop");
static_assert(kNetworkBootSyncGateWarningMs > 0,
              "boot sync gate warning delay must be positive");
static_assert(kNetworkDiagOtaLine == kNetworkDiagLineCount - 1,
              "network service diagnostics line mapping must match diagnostics line count");
static constexpr const char *kNetworkDiagIpLocationWifiStartFailed = "IP定位: WiFi启动失败";
static constexpr const char *kNetworkDiagIpLocationPowerLockUnavailable = "IP定位: 系统繁忙";
static constexpr const char *kNetworkDiagIpLocationWifiConnectTimeout = "IP定位: WiFi连接超时";
static constexpr const char *kNetworkSyncWeatherKeyMissing = "未配置 API Key";
static constexpr const char *kNetworkSyncLowBatterySkipped = "电量低，已跳过";
static constexpr const char *kNetworkWifiWaitSkippedLog = "wifi wait skipped: app events unavailable";
static constexpr const char *kNetworkCacheTimeConversionSkippedLog = "cache time conversion skipped: output is null";
static constexpr const char *kNetworkCacheUnknownLabel = "unknown";
#define NETWORK_CACHE_TIME_CONVERSION_FAILED_FORMAT "%s cache time conversion failed"
#define NETWORK_BOOT_REFRESH_SCHEDULED_FORMAT "boot network refresh scheduled: weather=%d saying=%d"
#define NETWORK_BOOT_HTTPS_MEMORY_DEFERRED_FORMAT \
    "background boot HTTPS deferred: internal_free=%u internal_largest=%u dma_largest=%u"
#define NETWORK_BOOT_SAYING_STAGGERED_FORMAT \
    "boot daily saying deferred %lld seconds after weather"
#define NETWORK_BOOT_WEATHER_RESOURCE_RETRY_FORMAT \
    "boot weather resource retry deferred %lld seconds"
static constexpr const char *kNetworkDiagWifiOnLog = "wifi radio on for network diagnostics";
#define NETWORK_SYNC_WIFI_ON_FORMAT "wifi radio on for sync: ntp=%d weather=%d saying=%d boot_weather=%d boot_saying=%d"
static constexpr const char *kNetworkSyncWifiStartFailedLog = "wifi start failed during sync window";
static constexpr const char *kNetworkSyncPowerLockUnavailableLog =
    "network PM lock unavailable during sync window";
static constexpr const char *kNetworkSyncWifiConnectTimeoutLog = "wifi connect timeout during sync window";
static constexpr const char *kNetworkBootSyncGateWaitLog = "network sync waiting for boot connectivity task";

struct NetworkRuntimeAvailabilitySnapshot {
    bool have_wifi_creds;
    bool have_weather_key;
    bool offline_mode;
    bool low_battery_mode;
};

static NetworkRuntimeAvailabilitySnapshot capture_network_runtime_availability()
{
    const NetworkCredentialsAvailability credentials = network_credentials_availability();
    return {
        credentials.wifi_configured,
        credentials.weather_api_key_configured,
        g_offline_mode_ui_enabled.load(std::memory_order_acquire),
        battery_low_mode_load(),
    };
}

bool wait_for_wifi_connected(uint32_t timeout_ms)
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "%s", kNetworkWifiWaitSkippedLog);
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        g_app_events,
        kWifiConnectedBit,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & kWifiConnectedBit) != 0;
}

bool is_time_valid(struct tm *local_out)
{
    return is_system_time_plausible(local_out);
}

bool enabled_weather_data_page_exists()
{
    return is_work_page_enabled(kWorkPageWeatherClock) ||
           is_work_page_enabled(kWorkPageWeatherBoard);
}

bool enabled_daily_saying_page_exists()
{
    return is_work_page_enabled(kWorkPageGallery);
}

void wait_for_network_sync_event(uint32_t timeout_ms)
{
    xEventGroupWaitBits(g_app_events,
                        kNetworkSyncWakeBits,
                        pdFALSE,
                        pdFALSE,
                        pdMS_TO_TICKS(timeout_ms));
}

static void wait_for_network_runtime_request()
{
    xEventGroupWaitBits(g_app_events,
                        kNetworkSyncWakeBits,
                        pdFALSE,
                        pdFALSE,
                        portMAX_DELAY);
}

static void wait_for_ota_network_block_change()
{
    // Pending level-triggered sync bits remain queued while OTA owns HTTPS and
    // Wi-Fi. Wait only for the edge-like runtime-state bit so those requests do
    // not turn the protection branch into a busy loop.
    xEventGroupWaitBits(g_app_events,
                        kNetworkStateChangedBit,
                        pdTRUE,
                        pdFALSE,
                        portMAX_DELAY);
}

void schedule_ntp_retry(time_t *next_ntp_retry_at)
{
    if (!next_ntp_retry_at) {
        return;
    }
    time(next_ntp_retry_at);
    *next_ntp_retry_at += kNetworkNtpRetryDelaySec;
}

static void update_ntp_retry_deadline(bool retry_required,
                                      time_t *next_ntp_retry_at)
{
    if (!next_ntp_retry_at) {
        return;
    }
    if (retry_required) {
        schedule_ntp_retry(next_ntp_retry_at);
    } else {
        *next_ntp_retry_at = 0;
    }
}

static bool localtime_for_cache_check(time_t value, struct tm *out, const char *label)
{
    if (!out) {
        ESP_LOGW(TAG, "%s", kNetworkCacheTimeConversionSkippedLog);
        return false;
    }
    if (!localtime_r(&value, out)) {
        ESP_LOGW(TAG, NETWORK_CACHE_TIME_CONVERSION_FAILED_FORMAT, label ? label : kNetworkCacheUnknownLabel);
        return false;
    }
    return true;
}

static bool weather_cache_current_hour(time_t now)
{
    const time_t last_sync_time = get_last_weather_sync_time();
    if (last_sync_time <= 0) {
        return false;
    }
    struct tm now_local = {};
    struct tm last_local = {};
    if (!localtime_for_cache_check(now, &now_local, "weather now") ||
        !localtime_for_cache_check(last_sync_time, &last_local, "weather last") ||
        !is_tm_plausible(now_local) ||
        !is_tm_plausible(last_local)) {
        return network_cache_age_is_fresh(now, last_sync_time, kSecondsPerHour);
    }
    return network_cache_local_hour_matches(now_local, last_local);
}

static bool saying_cache_current_day(time_t now)
{
    char saying[kDailySayingLen] = {};
    time_t last_sync_time = 0;
    if (!get_daily_saying_snapshot(saying, sizeof(saying), &last_sync_time) ||
        last_sync_time <= 0) {
        return false;
    }
    struct tm now_local = {};
    struct tm last_local = {};
    if (!localtime_for_cache_check(now, &now_local, "saying now") ||
        !localtime_for_cache_check(last_sync_time, &last_local, "saying last") ||
        !is_tm_plausible(now_local) ||
        !is_tm_plausible(last_local)) {
        return network_cache_age_is_fresh(now, last_sync_time, kSecondsPerDay);
    }
    return network_cache_local_day_matches(now_local, last_local);
}

static void clear_ready_boot_sync_flags(bool weather_ready, bool saying_ready, bool *weather_due, bool *saying_due)
{
    if (weather_ready && weather_due) {
        *weather_due = false;
    }
    if (saying_ready && saying_due) {
        *saying_due = false;
    }
}

static void finish_network_radio_session(NetworkAwakeLockGuard &awake_lock,
                                         bool force_setup_portal = false)
{
    // Keep the CPU awake through esp_wifi_stop(), then service any deferred
    // close request after this session releases its final PM-lock ownership.
    if (awake_lock.locked()) {
        stop_wifi_radio(force_setup_portal);
    }
    awake_lock.release();
    service_wifi_radio_stop_when_idle();
}

static void settle_between_network_operations(bool more_work_pending)
{
    if (more_work_pending) {
        // The preceding client has already released its TLS buffers. Give the
        // allocator and UI task a scheduling window before the next operation.
        // The first minute uses a longer gap because boot UI, sensor and cache
        // initialization still compete for internal/DMA memory.
        bool startup_pressure = network_startup_pressure_window_active(
            startup_screen_active(),
            esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(
            network_inter_operation_settle_delay_ms(startup_pressure)));
    }
}

static bool background_boot_https_memory_ready()
{
    const NetworkHttpsMemorySnapshot memory = capture_network_https_memory_snapshot();
    bool allowed = network_automatic_boot_https_allowed(startup_screen_active(),
                                                        esp_timer_get_time(),
                                                        memory.internal_free,
                                                        memory.internal_largest,
                                                        memory.dma_largest);
    if (!allowed) {
        ESP_LOGW(TAG,
                 NETWORK_BOOT_HTTPS_MEMORY_DEFERRED_FORMAT,
                 static_cast<unsigned>(memory.internal_free),
                 static_cast<unsigned>(memory.internal_largest),
                 static_cast<unsigned>(memory.dma_largest));
    }
    return allowed;
}

static bool defer_automatic_boot_https_for_memory(NetworkSyncSchedule *schedule,
                                                  const NetworkSyncRequestSnapshot &requests,
                                                  time_t now,
                                                  time_t *boot_weather_due_at,
                                                  time_t *boot_saying_due_at)
{
    if (!schedule) {
        return false;
    }
    bool auto_weather = schedule->boot_weather_ready &&
                        !requests.provisioning && !requests.manual_weather;
    bool auto_saying = schedule->boot_saying_ready &&
                       !requests.provisioning && !requests.manual_saying;
    if ((!auto_weather && !auto_saying) || background_boot_https_memory_ready()) {
        return false;
    }
    time_t retry_at = now + static_cast<time_t>(kBootHttpsMemoryRetryMs / 1000);
    if (auto_weather) {
        schedule->weather_due = false;
        schedule->boot_weather_ready = false;
        schedule->stagger_boot_saying_after_weather = false;
        if (boot_weather_due_at) {
            *boot_weather_due_at = retry_at;
        }
    }
    if (auto_saying) {
        schedule->saying_due = false;
        schedule->boot_saying_ready = false;
        if (boot_saying_due_at) {
            *boot_saying_due_at = retry_at;
        }
    }
    return true;
}

static void stagger_boot_saying_after_weather(const NetworkSyncSchedule &schedule,
                                              bool boot_saying_due,
                                              time_t *boot_saying_due_at)
{
    if (!schedule.stagger_boot_saying_after_weather ||
        !boot_saying_due || !boot_saying_due_at) {
        return;
    }
    time_t now = 0;
    time(&now);
    *boot_saying_due_at = now + kBootHttpsInterRequestGapSec;
    ESP_LOGI(TAG,
             NETWORK_BOOT_SAYING_STAGGERED_FORMAT,
             static_cast<long long>(kBootHttpsInterRequestGapSec));
}

static void finalize_failed_network_sync_window(const NetworkSyncSchedule &schedule,
                                                const NetworkSyncRequestSnapshot &requests,
                                                bool *boot_weather_due,
                                                bool *boot_saying_due,
                                                time_t *next_ntp_retry_at)
{
    clear_ready_boot_sync_flags(schedule.boot_weather_ready,
                                schedule.boot_saying_ready,
                                boot_weather_due,
                                boot_saying_due);
    finish_failed_sync_requests(requests);
    if (schedule.ntp_due) {
        update_ntp_retry_deadline(schedule.ntp_retry_required,
                                  next_ntp_retry_at);
    }
}

static void execute_connected_sync_window(const NetworkSyncSchedule &schedule,
                                          const NetworkSyncRequestSnapshot &requests,
                                          bool &boot_ntp_due,
                                          bool &boot_weather_due,
                                          bool &boot_saying_due,
                                          time_t &boot_weather_due_at,
                                          time_t &next_ntp_retry_at,
                                          bool &daily_ntp_pending,
                                          time_t &next_daily_ntp_at)
{
    bool ntp_ok = false;
    bool weather_ok = false;
    bool weather_resource_deferred = false;
    bool saying_ok = false;
    NetworkDisplayDmaGuard display_guard(schedule.weather_due || schedule.saying_due);
    if (schedule.ntp_due) {
        if (perform_ntp_sync()) {
            ntp_ok = true;
            boot_ntp_due = false;
            daily_ntp_pending = false;
            next_ntp_retry_at = 0;
            next_daily_ntp_at = next_local_midnight_time(time(nullptr));
        } else {
            update_ntp_retry_deadline(schedule.ntp_retry_required,
                                      &next_ntp_retry_at);
        }
        settle_between_network_operations(schedule.weather_due ||
                                          schedule.saying_due);
    }
    if (schedule.weather_due) {
        WeatherUpdateResult result = perform_weather_update();
        weather_ok = result == WeatherUpdateResult::kSuccess;
        weather_resource_deferred = result == WeatherUpdateResult::kResourceDeferred;
        settle_between_network_operations(schedule.saying_due);
    }
    if (schedule.saying_due) {
        saying_ok = perform_daily_saying_update();
    }
    boot_weather_due = network_boot_weather_due_after_update(
        boot_weather_due,
        schedule.boot_weather_ready,
        weather_resource_deferred);
    if (schedule.boot_weather_ready && weather_resource_deferred) {
        time_t now = 0;
        time(&now);
        boot_weather_due_at = now + static_cast<time_t>(kBootHttpsMemoryRetryMs / 1000);
        ESP_LOGI(TAG,
                 NETWORK_BOOT_WEATHER_RESOURCE_RETRY_FORMAT,
                 static_cast<long long>(kBootHttpsMemoryRetryMs / 1000));
    }
    clear_ready_boot_sync_flags(false,
                                schedule.boot_saying_ready,
                                nullptr,
                                &boot_saying_due);
    finish_successful_sync_requests(requests,
                                    ntp_ok,
                                    weather_ok,
                                    saying_ok);
}

static void execute_network_diagnostics_window()
{
    ESP_LOGI(TAG, "%s", kNetworkDiagWifiOnLog);
    NetworkAwakeLockGuard awake_lock;
    network_diag_begin();
    if (!awake_lock.locked()) {
        set_network_diag_unavailable(kNetworkDiagIpLocationPowerLockUnavailable);
    } else if (!start_wifi_radio(false)) {
        set_network_diag_unavailable(kNetworkDiagIpLocationWifiStartFailed);
    } else if (!wait_for_wifi_connected(kNetworkWifiConnectTimeoutMs)) {
        set_network_diag_unavailable(kNetworkDiagIpLocationWifiConnectTimeout);
    } else {
        run_network_diagnostic_checks();
    }
    finish_network_radio_session(awake_lock);
    network_diag_finish();
    finish_network_diagnostics_sync();
}

static void wait_for_boot_sync_completion()
{
    EventBits_t bits = xEventGroupWaitBits(g_app_events,
                                          kBootSyncDoneBit,
                                          pdFALSE,
                                          pdTRUE,
                                          pdMS_TO_TICKS(kNetworkBootSyncGateWarningMs));
    if ((bits & kBootSyncDoneBit) != 0) {
        return;
    }
    ESP_LOGW(TAG, "%s", kNetworkBootSyncGateWaitLog);
    xEventGroupWaitBits(g_app_events,
                        kBootSyncDoneBit,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);
}

void network_sync_task(void *)
{
    wait_for_boot_sync_completion();
    vTaskDelay(pdMS_TO_TICKS(kNetworkTaskStartupDelayMs));
    EventBits_t initial_bits = xEventGroupGetBits(g_app_events);
    bool boot_ntp_due = (initial_bits & kTimeSyncedBit) == 0;
    time_t next_ntp_retry_at = 0;
    bool daily_ntp_pending = false;
    time_t boot_schedule_now = 0;
    time(&boot_schedule_now);
    time_t next_daily_ntp_at = boot_ntp_due
                                   ? 0
                                   : next_local_midnight_time(boot_schedule_now);
    const NetworkRuntimeAvailabilitySnapshot initial_runtime =
        capture_network_runtime_availability();
    bool boot_weather_due = initial_runtime.have_wifi_creds &&
                            initial_runtime.have_weather_key &&
                            !initial_runtime.offline_mode &&
                            !initial_runtime.low_battery_mode &&
                            enabled_weather_data_page_exists();
    bool boot_saying_due = initial_runtime.have_wifi_creds &&
                           !initial_runtime.offline_mode &&
                           !initial_runtime.low_battery_mode &&
                           enabled_daily_saying_page_exists();
    time_t boot_weather_due_at = boot_schedule_now + kBootWeatherRefreshDelaySec;
    time_t boot_saying_due_at = boot_schedule_now + kBootSayingRefreshDelaySec;
    if (boot_weather_due || boot_saying_due) {
        ESP_LOGI(TAG, NETWORK_BOOT_REFRESH_SCHEDULED_FORMAT, boot_weather_due, boot_saying_due);
    }

    for (;;) {
        // Consume only the edge-like state notification before reading the
        // latest runtime state. Sync request bits stay level-triggered.
        xEventGroupClearBits(g_app_events, kNetworkStateChangedBit);
        NetworkSyncRequestSnapshot requests = snapshot_network_sync_requests();
        const NetworkRuntimeAvailabilitySnapshot runtime =
            capture_network_runtime_availability();
        int ota_state = ota_runtime_state_load();
        if (ota_blocks_background_network_sync(ota_state)) {
            wait_for_ota_network_block_change();
            continue;
        }
        if (runtime.offline_mode) {
            boot_weather_due = false;
            boot_saying_due = false;
            finish_offline_network_requests(requests);
            wait_for_network_runtime_request();
            continue;
        }
        if (!runtime.have_wifi_creds) {
            boot_weather_due = false;
            boot_saying_due = false;
            finish_unconfigured_network_requests(requests);
            wait_for_network_runtime_request();
            continue;
        }
        if (requests.manual_weather && !runtime.have_weather_key) {
            finish_settings_sync_and_clear_bit(kSettingsSyncWeather, kNetworkSyncWeatherKeyMissing, kManualWeatherSyncBit);
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }
        if (setup_portal_active_load() && requests.none_for_setup_portal()) {
            wait_for_network_sync_event(kNetworkNoWorkWaitMs);
            continue;
        }

        if (requests.diagnostics) {
            execute_network_diagnostics_window();
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }

        time_t now;
        time(&now);
        struct tm local = {};
        bool time_valid = localtime_r(&now, &local) && is_tm_plausible(local);
        if (!daily_ntp_pending && next_daily_ntp_at > 0 && now >= next_daily_ntp_at) {
            daily_ntp_pending = true;
            next_daily_ntp_at = 0;
        }
        if (!daily_ntp_pending && next_daily_ntp_at == 0 &&
            !boot_ntp_due && time_valid) {
            next_daily_ntp_at = next_local_midnight_time(now);
        }
        // A short boot request may obtain current conditions while forecast or
        // air quality times out. Keep the staggered full refresh scheduled in
        // that partial state so the weather board is ready before first entry.
        if (boot_weather_due &&
            weather_cache_current_hour(now) &&
            weather_extended_data_ready()) {
            boot_weather_due = false;
        }
        if (boot_saying_due && saying_cache_current_day(now)) {
            boot_saying_due = false;
        }
        if (runtime.low_battery_mode) {
            boot_weather_due = false;
            boot_saying_due = false;
        }
        NetworkSyncScheduleInput schedule_input = {};
        schedule_input.now = now;
        schedule_input.next_ntp_retry_at = next_ntp_retry_at;
        schedule_input.boot_weather_due_at = boot_weather_due_at;
        schedule_input.boot_saying_due_at = boot_saying_due_at;
        schedule_input.have_weather_key = runtime.have_weather_key;
        schedule_input.low_battery_mode = runtime.low_battery_mode;
        schedule_input.provisioning_sync_due = requests.provisioning;
        schedule_input.manual_ntp_due = requests.manual_ntp;
        schedule_input.manual_weather_due = requests.manual_weather;
        schedule_input.manual_saying_due = requests.manual_saying;
        schedule_input.boot_ntp_due = boot_ntp_due;
        schedule_input.daily_ntp_due = daily_ntp_pending;
        schedule_input.boot_weather_due = boot_weather_due;
        schedule_input.boot_saying_due = boot_saying_due;
        NetworkSyncSchedule schedule = calculate_network_sync_schedule(schedule_input);
        bool boot_https_memory_deferred = defer_automatic_boot_https_for_memory(
            &schedule,
            requests,
            now,
            &boot_weather_due_at,
            &boot_saying_due_at);
        if (runtime.low_battery_mode && requests.manual_weather) {
            finish_settings_sync_and_clear_bit(kSettingsSyncWeather, kNetworkSyncLowBatterySkipped, kManualWeatherSyncBit);
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }
        if (runtime.low_battery_mode && requests.manual_saying) {
            finish_settings_sync_and_clear_bit(kSettingsSyncSaying, kNetworkSyncLowBatterySkipped, kManualSayingSyncBit);
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }

        if (!schedule.ntp_due && !schedule.weather_due && !schedule.saying_due) {
            uint32_t wait_ms = boot_https_memory_deferred
                                   ? kBootHttpsMemoryRetryMs
                                   : network_idle_wait_ms(now,
                                                          schedule.next_boot_due_at,
                                                          next_ntp_retry_at,
                                                          next_daily_ntp_at);
            wait_for_network_sync_event(wait_ms);
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
        if (!awake_lock.locked() || !start_wifi_radio(false)) {
            ESP_LOGW(TAG,
                     "%s",
                     awake_lock.locked()
                         ? kNetworkSyncWifiStartFailedLog
                         : kNetworkSyncPowerLockUnavailableLog);
            finalize_failed_network_sync_window(schedule,
                                                requests,
                                                &boot_weather_due,
                                                &boot_saying_due,
                                                &next_ntp_retry_at);
            stagger_boot_saying_after_weather(schedule,
                                              boot_saying_due,
                                              &boot_saying_due_at);
            finish_network_radio_session(awake_lock, requests.provisioning);
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }
        if (wait_for_wifi_connected(kNetworkWifiConnectTimeoutMs)) {
            execute_connected_sync_window(schedule,
                                          requests,
                                          boot_ntp_due,
                                          boot_weather_due,
                                          boot_saying_due,
                                          boot_weather_due_at,
                                          next_ntp_retry_at,
                                          daily_ntp_pending,
                                          next_daily_ntp_at);
            stagger_boot_saying_after_weather(schedule,
                                              boot_saying_due,
                                              &boot_saying_due_at);
        } else {
            ESP_LOGW(TAG, "%s", kNetworkSyncWifiConnectTimeoutLog);
            finalize_failed_network_sync_window(schedule,
                                                requests,
                                                &boot_weather_due,
                                                &boot_saying_due,
                                                &next_ntp_retry_at);
            stagger_boot_saying_after_weather(schedule,
                                              boot_saying_due,
                                              &boot_saying_due_at);
        }
        finish_network_radio_session(awake_lock, requests.provisioning);
    }
}
