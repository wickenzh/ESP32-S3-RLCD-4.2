// 执行启动页 Wi-Fi 连接与时间校准，页面 HTTPS 数据统一留给后台错峰同步。
#include "network_services.h"

#include "app_text_format.h"
#include "network_credentials_state.h"
#include "network_sync_schedule.h"
#include "network_task_guards.h"
#include "ui_views.h"

#include <stdio.h>
#include <string.h>

namespace {
int64_t s_boot_sync_deadline_us = 0;
constexpr int64_t kMicrosecondsPerMillisecond = 1000;
constexpr uint32_t kBootScreenShortDelayMs = 200;
constexpr uint32_t kBootScreenOfflineDelayMs = 600;
constexpr uint32_t kBootScreenSetupDelayMs = 1500;
constexpr int kBootScreenCompletePercent = 100;
constexpr int kBootNtpMinRemainingMs = 600;
constexpr size_t kBootSetupDetailTextSize = 64;
constexpr const char *kBootDetailStartingClock = "Starting clock";
constexpr const char *kBootDetailPowerLockUnavailable = "Power lock unavailable";
constexpr const char *kBootDetailSynchronizingTime = "Synchronizing time";
constexpr const char *kBootDetailPageDataQueued = "Page data queued";
constexpr const char *kBootDetailBackgroundRefresh = "Refreshing after startup";
constexpr const char *kBootSetupDetailFallback = "Setup AP: --";
constexpr const char *kBootSetupDetailFormat = "Setup AP: %s";
constexpr const char *kBootRtcInvalidNtpPriorityLog =
    "system time invalid after Wi-Fi connect, prioritizing boot NTP";
constexpr const char *kBootPageDataDeferredLog =
    "boot page HTTPS deferred to staggered background sync";

class BootSyncDeadlineGuard {
public:
    BootSyncDeadlineGuard()
    {
        s_boot_sync_deadline_us = esp_timer_get_time() +
                                  static_cast<int64_t>(kBootStartupBudgetMs) *
                                      kMicrosecondsPerMillisecond;
    }

    ~BootSyncDeadlineGuard()
    {
        s_boot_sync_deadline_us = 0;
    }

    BootSyncDeadlineGuard(const BootSyncDeadlineGuard &) = delete;
    BootSyncDeadlineGuard &operator=(const BootSyncDeadlineGuard &) = delete;
};

void copy_boot_detail_fallback_on_format_error(int written, char *out, size_t out_len)
{
    if (!app_text::output_buffer_available(out, out_len)) {
        return;
    }
    if (app_text::format_failed(written, out_len)) {
        strlcpy(out, kBootSetupDetailFallback, out_len);
    }
}

void format_boot_setup_detail(char *out, size_t out_len)
{
    if (!app_text::output_buffer_available(out, out_len)) {
        return;
    }
    int written = snprintf(out, out_len, kBootSetupDetailFormat, g_ap_ssid);
    copy_boot_detail_fallback_on_format_error(written, out, out_len);
}

} // namespace

int boot_sync_remaining_ms()
{
    return network_boot_budget_remaining_ms(s_boot_sync_deadline_us,
                                            esp_timer_get_time());
}

void run_boot_connectivity_sync()
{
    if (g_offline_mode_ui_enabled) {
        update_boot_screen(kBootScreenCompletePercent, "Offline mode", "Using RTC time");
        vTaskDelay(pdMS_TO_TICKS(kBootScreenOfflineDelayMs));
        return;
    }
    NetworkCredentialsSnapshot credentials = {};
    network_credentials_snapshot(&credentials);
    if (!credentials.wifi_configured) {
        char detail[kBootSetupDetailTextSize] = {};
        format_boot_setup_detail(detail, sizeof(detail));
        update_boot_screen(kBootScreenCompletePercent, "Setup mode", detail);
        vTaskDelay(pdMS_TO_TICKS(kBootScreenSetupDelayMs));
        return;
    }

    update_boot_screen(18, "Connecting Wi-Fi", credentials.wifi_ssid);
    NetworkAwakeLockGuard awake_lock;
    BootSyncDeadlineGuard deadline_guard;
    if (!awake_lock.locked()) {
        update_boot_screen(kBootScreenCompletePercent,
                           kBootDetailPowerLockUnavailable,
                           kBootDetailStartingClock);
        vTaskDelay(pdMS_TO_TICKS(kBootScreenShortDelayMs));
        service_wifi_radio_stop_when_idle();
        return;
    }
    if (!start_wifi_radio(false)) {
        update_boot_screen(kBootScreenCompletePercent, "Wi-Fi start failed", kBootDetailStartingClock);
        vTaskDelay(pdMS_TO_TICKS(kBootScreenShortDelayMs));
        awake_lock.release();
        service_wifi_radio_stop_when_idle();
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
        awake_lock.release();
        service_wifi_radio_stop_when_idle();
        return;
    }

    update_boot_screen(42, "Wi-Fi connected", "Checking time");
    ESP_LOGI(TAG, "%s", kBootPageDataDeferredLog);
    bool ntp_attempted = false;
    bool ntp_ok = false;
    if (!is_time_valid()) {
        ESP_LOGI(TAG, "%s", kBootRtcInvalidNtpPriorityLog);
        remaining_ms = boot_sync_remaining_ms();
        if (remaining_ms > kBootNtpMinRemainingMs) {
            ntp_attempted = true;
            update_boot_screen(46, kBootDetailSynchronizingTime, "Restoring lost RTC time");
            ntp_ok = perform_ntp_sync(kBootNtpRetries);
            update_boot_screen(72,
                               ntp_ok ? "Time synchronized" : "NTP retry later",
                               ntp_ok ? kBootDetailPageDataQueued : "Will retry in background");
        }
    }
    remaining_ms = boot_sync_remaining_ms();
    if (!ntp_attempted && remaining_ms > kBootNtpMinRemainingMs) {
        update_boot_screen(82, kBootDetailSynchronizingTime, "Short NTP check");
        ntp_ok = perform_ntp_sync(kBootNtpRetries);
    }
    update_boot_screen(kBootScreenCompletePercent,
                       ntp_ok ? "Time synchronized" : "NTP retry later",
                       kBootDetailBackgroundRefresh);

    vTaskDelay(pdMS_TO_TICKS(kBootScreenShortDelayMs));
    stop_wifi_radio();
    awake_lock.release();
    service_wifi_radio_stop_when_idle();
}

void boot_connectivity_task(void *)
{
    run_boot_connectivity_sync();
    xEventGroupSetBits(g_app_events, kBootSyncDoneBit);
    vTaskDelete(nullptr);
}
