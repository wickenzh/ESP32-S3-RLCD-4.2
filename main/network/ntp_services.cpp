// 执行 NTP 时间同步并维护系统可信时间状态。
#include "network_services.h"

#include "sensor_services.h"

#include "sdkconfig.h"

namespace {
constexpr const char *const kNtpServers[] = {
    "pool.ntp.org",
    "ntp.aliyun.com",
    "time.windows.com",
};
constexpr size_t kNtpServerCount = sizeof(kNtpServers) / sizeof(kNtpServers[0]);
#ifdef CONFIG_LWIP_SNTP_MAX_SERVERS
constexpr size_t kConfiguredNtpServerSlots = CONFIG_LWIP_SNTP_MAX_SERVERS;
#else
constexpr size_t kConfiguredNtpServerSlots = 1;
#endif
constexpr size_t kActiveNtpServerCount =
    kNtpServerCount < kConfiguredNtpServerSlots ? kNtpServerCount : kConfiguredNtpServerSlots;
constexpr uint32_t kNtpPollDelayMs = 1000;
static_assert(kNtpServerCount > 0, "at least one NTP server is required");
static_assert(kConfiguredNtpServerSlots > 0, "SNTP must support at least one configured server");

void set_time_synced_event_bit()
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "skip time synced event bit: app events unavailable");
        return;
    }
    xEventGroupSetBits(g_app_events, kTimeSyncedBit);
}

void configure_ntp_servers()
{
    for (size_t i = 0; i < kActiveNtpServerCount; ++i) {
        esp_sntp_setservername(i, kNtpServers[i]);
    }
}
} // namespace

bool perform_ntp_sync(int max_retries)
{
    if (max_retries <= 0) {
        ESP_LOGW(TAG, "ntp sync invalid retry count: %d", max_retries);
        return false;
    }
    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    if (!g_ntp_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        configure_ntp_servers();
        esp_sntp_init();
        g_ntp_started = true;
    } else {
        esp_sntp_restart();
    }

    for (int retry = 0; retry < max_retries; ++retry) {
        struct tm local = {};
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
            is_system_time_plausible(&local)) {
            sync_rtc_from_system_time();
            time(&g_last_ntp_sync_time);
            set_time_synced_event_bit();
            ESP_LOGI(TAG, "ntp synced: %04d-%02d-%02d %02d:%02d:%02d",
                     local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                     local.tm_hour, local.tm_min, local.tm_sec);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kNtpPollDelayMs));
    }
    ESP_LOGW(TAG, "ntp sync timeout retries=%d poll_ms=%lu",
             max_retries,
             (unsigned long)kNtpPollDelayMs);
    return false;
}
