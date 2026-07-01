// 调度 NTP、天气、预警、每日文字和 OTA 等联网同步流程。
#include "network_services.h"

#include "display_bsp.h"
#include "sensor_services.h"
#include "ui_views.h"

static constexpr uint32_t kNetworkOtaActiveWaitMs = 10000;
static constexpr uint32_t kMsPerSecond = 1000;
static constexpr int64_t kUsPerMs = 1000;
static constexpr time_t kSecondsPerMinute = 60;
static constexpr time_t kMinutesPerHour = 60;
static constexpr time_t kHoursPerDay = 24;
static constexpr time_t kSecondsPerHour = kMinutesPerHour * kSecondsPerMinute;
static constexpr time_t kSecondsPerDay = kHoursPerDay * kSecondsPerHour;
static constexpr uint32_t kNetworkIdleDefaultWaitMs = 5 * kSecondsPerMinute * kMsPerSecond;
static constexpr uint32_t kNetworkIdleMinWaitMs = 1000;
static constexpr uint32_t kNetworkNoWorkWaitMs = 30000;
static constexpr uint32_t kNetworkShortRetryWaitMs = 1000;
static constexpr uint32_t kNetworkWifiConnectTimeoutMs = 45000;
static constexpr uint32_t kBootScreenShortDelayMs = 200;
static constexpr uint32_t kBootScreenOfflineDelayMs = 600;
static constexpr uint32_t kBootScreenSetupDelayMs = 1500;
static constexpr int kBootScreenCompletePercent = 100;
static constexpr int kBootWeatherMinRemainingMs = 250;
static constexpr int kBootSayingMinRemainingMs = 700;
static constexpr int kBootNtpMinRemainingMs = 600;
static constexpr uint32_t kNetworkTaskStartupDelayMs = 2500;
static constexpr time_t kNetworkNtpRetryDelaySec = 5 * kSecondsPerMinute;
static constexpr time_t kBootWeatherRefreshDelaySec = 8;
static constexpr time_t kBootSayingRefreshDelaySec = 16;
static constexpr size_t kBootSetupDetailTextSize = 64;
static constexpr const char *kNetworkStatusOfflineModeEnabled = "离线模式已开启";
static constexpr const char *kNetworkStatusWifiNotConfigured = "未配置 WiFi";
static constexpr const char *kNetworkDiagLocalIpPlaceholder = "本地IP: --";
static constexpr const char *kNetworkDiagPublicIpPlaceholder = "公网IP: --";
static constexpr const char *kNetworkDiagDnsUnchecked = "DNS: 未检测";
static constexpr const char *kNetworkDiagWeatherUnchecked = "天气: 未检测";
static constexpr const char *kNetworkDiagNtpUnchecked = "NTP: 未检测";
static constexpr const char *kNetworkDiagSayingUnchecked = "一言: 未检测";
static constexpr const char *kNetworkDiagInternetUnchecked = "公网: 未检测";
static constexpr const char *kNetworkDiagOtaSourceUnchecked = "OTA源: 未检测";
static constexpr const char *kNetworkSyncTimeFailed = "时间同步失败";
static constexpr const char *kNetworkSyncWeatherFailed = "天气同步失败";
static constexpr const char *kNetworkSyncSayingFailed = "一言更新失败";
static constexpr const char *kBootDetailStartingClock = "Starting clock";
static constexpr const char *kBootDetailSynchronizingTime = "Synchronizing time";
static constexpr const char *kBootSetupDetailFormat = "Setup AP: %s";
static constexpr const char *kNetworkWifiWaitSkippedLog = "wifi wait skipped: app events unavailable";
static constexpr const char *kNetworkCacheTimeConversionSkippedLog = "cache time conversion skipped: output is null";
static constexpr const char *kNetworkCacheUnknownLabel = "unknown";
#define NETWORK_CACHE_TIME_CONVERSION_FAILED_FORMAT "%s cache time conversion failed"
#define NETWORK_BOOT_REFRESH_SCHEDULED_FORMAT "boot network refresh scheduled: weather=%d saying=%d"
static constexpr const char *kNetworkDiagWifiOnLog = "wifi radio on for network diagnostics";
#define NETWORK_SYNC_WIFI_ON_FORMAT "wifi radio on for sync: ntp=%d weather=%d saying=%d boot_weather=%d boot_saying=%d"
static constexpr const char *kNetworkSyncWifiStartFailedLog = "wifi start failed during sync window";
static constexpr const char *kNetworkSyncWifiConnectTimeoutLog = "wifi connect timeout during sync window";

class NetworkDisplayDmaGuard {
public:
    explicit NetworkDisplayDmaGuard(bool active) : active_(active)
    {
        if (active_) {
            Display_AcquireDmaConservativeMode();
        }
    }

    ~NetworkDisplayDmaGuard()
    {
        if (active_) {
            Display_ReleaseDmaConservativeMode();
        }
    }

private:
    bool active_ = false;
};

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

void run_boot_connectivity_sync()
{
    if (g_offline_mode_ui_enabled) {
        update_boot_screen(kBootScreenCompletePercent, "Offline mode", "Using RTC time");
        vTaskDelay(pdMS_TO_TICKS(kBootScreenOfflineDelayMs));
        return;
    }
    if (!g_have_wifi_creds) {
        char detail[kBootSetupDetailTextSize];
        snprintf(detail, sizeof(detail), kBootSetupDetailFormat, g_ap_ssid);
        update_boot_screen(kBootScreenCompletePercent, "Setup mode", detail);
        vTaskDelay(pdMS_TO_TICKS(kBootScreenSetupDelayMs));
        return;
    }

    update_boot_screen(18, "Connecting Wi-Fi", g_wifi_ssid);
    acquire_network_awake_lock();
    g_boot_sync_deadline_us = esp_timer_get_time() + (int64_t)kBootStartupBudgetMs * kUsPerMs;
    if (!start_wifi_radio(false)) {
        update_boot_screen(kBootScreenCompletePercent, "Wi-Fi start failed", kBootDetailStartingClock);
        vTaskDelay(pdMS_TO_TICKS(kBootScreenShortDelayMs));
        release_network_awake_lock();
        g_boot_sync_deadline_us = 0;
        return;
    }
    int remaining_ms = boot_sync_remaining_ms();
    uint32_t wifi_timeout_ms = remaining_ms > 0 && remaining_ms < kBootWifiConnectTimeoutMs
                                   ? remaining_ms
                                   : kBootWifiConnectTimeoutMs;
    if (!wait_for_wifi_connected(wifi_timeout_ms)) {
        update_boot_screen(kBootScreenCompletePercent, "Wi-Fi timeout", "Check SSID or password");
        vTaskDelay(pdMS_TO_TICKS(kBootScreenShortDelayMs));
        stop_wifi_radio();
        release_network_awake_lock();
        g_boot_sync_deadline_us = 0;
        return;
    }

    bool boot_weather_page_visible = g_active_work_page == kWorkPageWeatherClock ||
                                     g_active_work_page == kWorkPageWeatherBoard;
    bool boot_gallery_page_visible = g_active_work_page == kWorkPageGallery;
    update_boot_screen(42, "Wi-Fi connected", boot_weather_page_visible ? "Loading weather" : "Checking time");
    remaining_ms = boot_sync_remaining_ms();
    if (boot_weather_page_visible && g_have_weather_key && !g_low_battery_mode && remaining_ms > kBootWeatherMinRemainingMs) {
        bool weather_ok = false;
        int previous_timeout = g_http_timeout_ms;
        g_http_timeout_ms = kHttpBootTimeoutMs;
        update_boot_screen(58, "Loading weather", "Fetching API data");
        {
            NetworkDisplayDmaGuard display_guard(true);
            weather_ok = perform_weather_update();
        }
        g_http_timeout_ms = previous_timeout;
        update_boot_screen(weather_ok ? 76 : 68,
                           weather_ok ? "Weather ready" : "Weather retry later",
                           weather_ok ? kBootDetailSynchronizingTime : "Will sync in background");
    } else if (boot_weather_page_visible && g_have_weather_key && !g_low_battery_mode) {
        update_boot_screen(68, "Weather retry later", kBootDetailStartingClock);
    } else if (g_low_battery_mode) {
        update_boot_screen(58, "Weather skipped", "Low battery");
    } else if (!boot_weather_page_visible) {
        update_boot_screen(58, "Weather deferred", "Open weather page");
    } else {
        update_boot_screen(58, "Weather skipped", "API Key not configured");
    }

    remaining_ms = boot_sync_remaining_ms();
    if (boot_gallery_page_visible && !g_low_battery_mode && remaining_ms > kBootSayingMinRemainingMs) {
        int previous_timeout = g_http_timeout_ms;
        g_http_timeout_ms = kHttpBootTimeoutMs;
        update_boot_screen(78, "Loading quote", "Fetching daily text");
        bool saying_ok = false;
        {
            NetworkDisplayDmaGuard display_guard(true);
            saying_ok = perform_daily_saying_update();
        }
        g_http_timeout_ms = previous_timeout;
        update_boot_screen(saying_ok ? 80 : 78,
                           saying_ok ? "Quote ready" : "Quote retry later",
                           kBootDetailSynchronizingTime);
    } else if (!boot_gallery_page_visible && !g_low_battery_mode) {
        update_boot_screen(78, "Quote deferred", "Open image page");
    }

    bool ntp_ok = false;
    remaining_ms = boot_sync_remaining_ms();
    if (remaining_ms > kBootNtpMinRemainingMs) {
        update_boot_screen(82, kBootDetailSynchronizingTime, "Short NTP check");
        ntp_ok = perform_ntp_sync(kBootNtpRetries);
    }
    update_boot_screen(kBootScreenCompletePercent,
                       ntp_ok ? "Time synchronized" : "NTP retry later",
                       kBootDetailStartingClock);

    vTaskDelay(pdMS_TO_TICKS(kBootScreenShortDelayMs));
    stop_wifi_radio();
    release_network_awake_lock();
    g_boot_sync_deadline_us = 0;
}

void boot_connectivity_task(void *)
{
    run_boot_connectivity_sync();
    xEventGroupSetBits(g_app_events, kBootSyncDoneBit);
    g_boot_sync_task_handle = nullptr;
    vTaskDelete(nullptr);
}

void wait_for_network_sync_event(uint32_t timeout_ms)
{
    xEventGroupWaitBits(g_app_events,
                        kProvisioningSyncBit | kManualNtpSyncBit | kManualWeatherSyncBit | kManualSayingSyncBit | kNetworkDiagBit,
                        pdFALSE,
                        pdFALSE,
                        pdMS_TO_TICKS(timeout_ms));
}

uint32_t network_idle_wait_ms(time_t now, time_t next_weather_at, time_t next_ntp_retry_at)
{
    uint32_t wait_ms = kNetworkIdleDefaultWaitMs;
    if (next_weather_at > now) {
        uint32_t weather_wait = (uint32_t)((next_weather_at - now) * kMsPerSecond);
        if (weather_wait < wait_ms) {
            wait_ms = weather_wait;
        }
    }
    if (next_ntp_retry_at > now) {
        uint32_t ntp_wait = (uint32_t)((next_ntp_retry_at - now) * kMsPerSecond);
        if (ntp_wait < wait_ms) {
            wait_ms = ntp_wait;
        }
    }
    if (wait_ms < kNetworkIdleMinWaitMs) {
        wait_ms = kNetworkIdleMinWaitMs;
    } else if (wait_ms > kNetworkIdleDefaultWaitMs) {
        wait_ms = kNetworkIdleDefaultWaitMs;
    }
    return wait_ms;
}

void schedule_ntp_retry(time_t *next_ntp_retry_at)
{
    if (!next_ntp_retry_at) {
        return;
    }
    time(next_ntp_retry_at);
    *next_ntp_retry_at += kNetworkNtpRetryDelaySec;
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
    if (g_last_weather_sync_time <= 0) {
        return false;
    }
    struct tm now_local = {};
    struct tm last_local = {};
    if (!localtime_for_cache_check(now, &now_local, "weather now") ||
        !localtime_for_cache_check(g_last_weather_sync_time, &last_local, "weather last") ||
        !is_tm_plausible(now_local) ||
        !is_tm_plausible(last_local)) {
        return now - g_last_weather_sync_time < kSecondsPerHour;
    }
    return now_local.tm_year == last_local.tm_year &&
           now_local.tm_yday == last_local.tm_yday &&
           now_local.tm_hour == last_local.tm_hour;
}

static bool saying_cache_current_day(time_t now)
{
    if (g_daily_saying[0] == '\0' || g_last_saying_sync_time <= 0) {
        return false;
    }
    struct tm now_local = {};
    struct tm last_local = {};
    if (!localtime_for_cache_check(now, &now_local, "saying now") ||
        !localtime_for_cache_check(g_last_saying_sync_time, &last_local, "saying last") ||
        !is_tm_plausible(now_local) ||
        !is_tm_plausible(last_local)) {
        return now - g_last_saying_sync_time < kSecondsPerDay;
    }
    return now_local.tm_year == last_local.tm_year &&
           now_local.tm_yday == last_local.tm_yday;
}

static time_t earliest_pending_boot_sync(time_t current, bool weather_pending, time_t weather_at, bool saying_pending, time_t saying_at)
{
    time_t next = 0;
    if (weather_pending && weather_at > current) {
        next = weather_at;
    }
    if (saying_pending && saying_at > current && (next == 0 || saying_at < next)) {
        next = saying_at;
    }
    return next;
}

void network_sync_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(kNetworkTaskStartupDelayMs));
    EventBits_t initial_bits = xEventGroupGetBits(g_app_events);
    bool boot_ntp_due = (initial_bits & kTimeSyncedBit) == 0;
    time_t next_ntp_retry_at = 0;
    int last_midnight_ntp_yday = -1;
    time_t boot_schedule_now = 0;
    time(&boot_schedule_now);
    bool boot_weather_due = g_have_wifi_creds &&
                            g_have_weather_key &&
                            !g_offline_mode_ui_enabled &&
                            !g_low_battery_mode &&
                            (is_work_page_enabled(kWorkPageWeatherClock) ||
                             is_work_page_enabled(kWorkPageWeatherBoard));
    bool boot_saying_due = g_have_wifi_creds &&
                           !g_offline_mode_ui_enabled &&
                           !g_low_battery_mode &&
                           is_work_page_enabled(kWorkPageGallery);
    time_t boot_weather_due_at = boot_schedule_now + kBootWeatherRefreshDelaySec;
    time_t boot_saying_due_at = boot_schedule_now + kBootSayingRefreshDelaySec;
    if (boot_weather_due || boot_saying_due) {
        ESP_LOGI(TAG, NETWORK_BOOT_REFRESH_SCHEDULED_FORMAT, boot_weather_due, boot_saying_due);
    }

    for (;;) {
        EventBits_t loop_bits = xEventGroupGetBits(g_app_events);
        bool provisioning_sync_due = (loop_bits & kProvisioningSyncBit) != 0;
        bool manual_ntp_due = (loop_bits & kManualNtpSyncBit) != 0;
        bool manual_weather_due = (loop_bits & kManualWeatherSyncBit) != 0;
        bool manual_saying_due = (loop_bits & kManualSayingSyncBit) != 0;
        bool network_diag_due = (loop_bits & kNetworkDiagBit) != 0;
        if (g_ota_state == kOtaChecking || g_ota_state == kOtaUpdating) {
            wait_for_network_sync_event(kNetworkOtaActiveWaitMs);
            continue;
        }
        if (g_offline_mode_ui_enabled) {
            boot_weather_due = false;
            boot_saying_due = false;
            if (g_wifi_radio_on && !g_setup_portal_active) {
                stop_wifi_radio(true);
            }
            if (manual_ntp_due) {
                finish_settings_sync(kSettingsSyncNtp, kNetworkStatusOfflineModeEnabled);
            }
            if (manual_weather_due) {
                finish_settings_sync(kSettingsSyncWeather, kNetworkStatusOfflineModeEnabled);
            }
            if (manual_saying_due) {
                finish_settings_sync(kSettingsSyncSaying, kNetworkStatusOfflineModeEnabled);
            }
            if (network_diag_due) {
                network_diag_begin();
                for (int i = 0; i < kNetworkDiagLineCount; ++i) {
                    network_diag_set_line(i, kNetworkStatusOfflineModeEnabled);
                }
                network_diag_finish();
                finish_settings_sync(kSettingsSyncNetworkDiag, kNetworkStatusOfflineModeEnabled);
            }
            clear_network_request_bits();
            wait_for_network_sync_event(kNetworkNoWorkWaitMs);
            continue;
        }
        if (!g_have_wifi_creds) {
            boot_weather_due = false;
            boot_saying_due = false;
            if (manual_ntp_due) {
                finish_settings_sync(kSettingsSyncNtp, kNetworkStatusWifiNotConfigured);
                xEventGroupClearBits(g_app_events, kManualNtpSyncBit);
            }
            if (manual_weather_due) {
                finish_settings_sync(kSettingsSyncWeather, kNetworkStatusWifiNotConfigured);
                xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            }
            if (manual_saying_due) {
                finish_settings_sync(kSettingsSyncSaying, kNetworkStatusWifiNotConfigured);
                xEventGroupClearBits(g_app_events, kManualSayingSyncBit);
            }
            if (network_diag_due) {
                network_diag_begin();
                network_diag_set_line(0, kNetworkDiagLocalIpPlaceholder);
                network_diag_set_line(1, kNetworkDiagPublicIpPlaceholder);
                network_diag_set_line(2, "IP定位: WiFi未配置");
                network_diag_set_line(3, kNetworkDiagDnsUnchecked);
                network_diag_set_line(4, kNetworkDiagWeatherUnchecked);
                network_diag_set_line(5, kNetworkDiagNtpUnchecked);
                network_diag_set_line(6, kNetworkDiagSayingUnchecked);
                network_diag_set_line(7, kNetworkDiagInternetUnchecked);
                network_diag_set_line(8, kNetworkDiagOtaSourceUnchecked);
                network_diag_finish();
                finish_settings_sync(kSettingsSyncNetworkDiag, "网络检测完成");
                xEventGroupClearBits(g_app_events, kNetworkDiagBit);
            }
            wait_for_network_sync_event(kNetworkNoWorkWaitMs);
            continue;
        }
        if (manual_weather_due && !g_have_weather_key) {
            finish_settings_sync(kSettingsSyncWeather, "未配置 API Key");
            xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }
        if (g_setup_portal_active && !provisioning_sync_due && !manual_ntp_due && !manual_weather_due && !manual_saying_due && !network_diag_due) {
            wait_for_network_sync_event(kNetworkNoWorkWaitMs);
            continue;
        }

        if (network_diag_due) {
            ESP_LOGI(TAG, "%s", kNetworkDiagWifiOnLog);
            acquire_network_awake_lock();
            network_diag_begin();
            if (!start_wifi_radio(false)) {
                network_diag_set_line(0, kNetworkDiagLocalIpPlaceholder);
                network_diag_set_line(1, kNetworkDiagPublicIpPlaceholder);
                network_diag_set_line(2, "IP定位: WiFi启动失败");
                network_diag_set_line(3, kNetworkDiagDnsUnchecked);
                network_diag_set_line(4, kNetworkDiagWeatherUnchecked);
                network_diag_set_line(5, kNetworkDiagNtpUnchecked);
                network_diag_set_line(6, kNetworkDiagSayingUnchecked);
                network_diag_set_line(7, kNetworkDiagInternetUnchecked);
                network_diag_set_line(8, kNetworkDiagOtaSourceUnchecked);
            } else if (!wait_for_wifi_connected(kNetworkWifiConnectTimeoutMs)) {
                network_diag_set_line(0, kNetworkDiagLocalIpPlaceholder);
                network_diag_set_line(1, kNetworkDiagPublicIpPlaceholder);
                network_diag_set_line(2, "IP定位: WiFi连接超时");
                network_diag_set_line(3, kNetworkDiagDnsUnchecked);
                network_diag_set_line(4, kNetworkDiagWeatherUnchecked);
                network_diag_set_line(5, kNetworkDiagNtpUnchecked);
                network_diag_set_line(6, kNetworkDiagSayingUnchecked);
                network_diag_set_line(7, kNetworkDiagInternetUnchecked);
                network_diag_set_line(8, kNetworkDiagOtaSourceUnchecked);
            } else {
                run_network_diagnostics();
            }
            stop_wifi_radio();
            release_network_awake_lock();
            network_diag_finish();
            finish_settings_sync(kSettingsSyncNetworkDiag, "网络检测完成");
            xEventGroupClearBits(g_app_events, kNetworkDiagBit);
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }

        struct tm local = {};
        bool time_valid = is_time_valid(&local);
        bool midnight_ntp_due = time_valid &&
                                local.tm_hour == 0 &&
                                local.tm_min == 0 &&
                                local.tm_yday != last_midnight_ntp_yday;
        time_t now;
        time(&now);
        if (boot_weather_due && weather_cache_current_hour(now)) {
            boot_weather_due = false;
        }
        if (boot_saying_due && saying_cache_current_day(now)) {
            boot_saying_due = false;
        }
        if (g_low_battery_mode) {
            boot_weather_due = false;
            boot_saying_due = false;
        }
        bool boot_weather_ready = boot_weather_due && now >= boot_weather_due_at;
        bool boot_saying_ready = boot_saying_due && now >= boot_saying_due_at;
        bool weather_due = g_have_weather_key && !g_low_battery_mode &&
                           (manual_weather_due || provisioning_sync_due || boot_weather_ready);
        if (g_low_battery_mode && manual_weather_due) {
            finish_settings_sync(kSettingsSyncWeather, "电量低，已跳过");
            xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }
        bool ntp_due = (manual_ntp_due || provisioning_sync_due || boot_ntp_due || midnight_ntp_due) && now >= next_ntp_retry_at;
        bool saying_due = !g_low_battery_mode &&
                           (manual_saying_due || provisioning_sync_due || boot_saying_ready);
        if (g_low_battery_mode && manual_saying_due) {
            finish_settings_sync(kSettingsSyncSaying, "电量低，已跳过");
            xEventGroupClearBits(g_app_events, kManualSayingSyncBit);
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }

        if (!ntp_due && !weather_due && !saying_due) {
            time_t next_boot_due_at = earliest_pending_boot_sync(now,
                                                                 boot_weather_due,
                                                                 boot_weather_due_at,
                                                                 boot_saying_due,
                                                                 boot_saying_due_at);
            wait_for_network_sync_event(network_idle_wait_ms(now, next_boot_due_at, next_ntp_retry_at));
            continue;
        }

        ESP_LOGI(TAG,
                 NETWORK_SYNC_WIFI_ON_FORMAT,
                 ntp_due,
                 weather_due,
                 saying_due,
                 boot_weather_ready,
                 boot_saying_ready);
        acquire_network_awake_lock();
        if (!start_wifi_radio(false)) {
            ESP_LOGW(TAG, "%s", kNetworkSyncWifiStartFailedLog);
            if (boot_weather_ready) {
                boot_weather_due = false;
            }
            if (boot_saying_ready) {
                boot_saying_due = false;
            }
            if (provisioning_sync_due) {
                xEventGroupClearBits(g_app_events, kProvisioningSyncBit);
            }
            if (manual_ntp_due) {
                finish_settings_sync(kSettingsSyncNtp, kNetworkSyncTimeFailed);
                xEventGroupClearBits(g_app_events, kManualNtpSyncBit);
            }
            if (manual_weather_due) {
                finish_settings_sync(kSettingsSyncWeather, kNetworkSyncWeatherFailed);
                xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            }
            if (manual_saying_due) {
                finish_settings_sync(kSettingsSyncSaying, kNetworkSyncSayingFailed);
                xEventGroupClearBits(g_app_events, kManualSayingSyncBit);
            }
            if (ntp_due) {
                schedule_ntp_retry(&next_ntp_retry_at);
            }
            stop_wifi_radio(provisioning_sync_due);
            release_network_awake_lock();
            wait_for_network_sync_event(kNetworkShortRetryWaitMs);
            continue;
        }
        if (wait_for_wifi_connected(kNetworkWifiConnectTimeoutMs)) {
            bool ntp_ok = false;
            bool weather_ok = false;
            bool saying_ok = false;
            NetworkDisplayDmaGuard display_guard(weather_due || saying_due);
            if (ntp_due) {
                if (perform_ntp_sync()) {
                    ntp_ok = true;
                    boot_ntp_due = false;
                    next_ntp_retry_at = 0;
                    if (is_time_valid(&local) && local.tm_hour == 0) {
                        last_midnight_ntp_yday = local.tm_yday;
                    }
                } else {
                    schedule_ntp_retry(&next_ntp_retry_at);
                }
            }
            if (weather_due) {
                weather_ok = perform_weather_update();
                if (boot_weather_ready) {
                    boot_weather_due = false;
                }
            }
            if (saying_due) {
                saying_ok = perform_daily_saying_update();
                if (boot_saying_ready) {
                    boot_saying_due = false;
                }
            }
            if (provisioning_sync_due) {
                xEventGroupClearBits(g_app_events, kProvisioningSyncBit);
            }
            if (manual_ntp_due) {
                finish_settings_sync(kSettingsSyncNtp, ntp_ok ? "时间同步完成" : kNetworkSyncTimeFailed);
                xEventGroupClearBits(g_app_events, kManualNtpSyncBit);
            }
            if (manual_weather_due) {
                finish_settings_sync(kSettingsSyncWeather, weather_ok ? "天气同步完成" : kNetworkSyncWeatherFailed);
                xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
                notify_ui_task();
            }
            if (manual_saying_due) {
                finish_settings_sync(kSettingsSyncSaying, saying_ok ? "一言更新完成" : kNetworkSyncSayingFailed);
                xEventGroupClearBits(g_app_events, kManualSayingSyncBit);
                notify_ui_task();
            }
        } else {
            ESP_LOGW(TAG, "%s", kNetworkSyncWifiConnectTimeoutLog);
            if (boot_weather_ready) {
                boot_weather_due = false;
            }
            if (boot_saying_ready) {
                boot_saying_due = false;
            }
            if (provisioning_sync_due) {
                xEventGroupClearBits(g_app_events, kProvisioningSyncBit);
            }
            if (manual_ntp_due) {
                finish_settings_sync(kSettingsSyncNtp, kNetworkSyncTimeFailed);
                xEventGroupClearBits(g_app_events, kManualNtpSyncBit);
            }
            if (manual_weather_due) {
                finish_settings_sync(kSettingsSyncWeather, kNetworkSyncWeatherFailed);
                xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            }
            if (manual_saying_due) {
                finish_settings_sync(kSettingsSyncSaying, kNetworkSyncSayingFailed);
                xEventGroupClearBits(g_app_events, kManualSayingSyncBit);
            }
            if (ntp_due) {
                schedule_ntp_retry(&next_ntp_retry_at);
            }
        }
        stop_wifi_radio(provisioning_sync_due);
        release_network_awake_lock();
    }
}
