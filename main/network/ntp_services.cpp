// 执行 NTP 时间同步并维护系统可信时间状态。
#include "network_services.h"

#include "sensor_services.h"

#include "sdkconfig.h"

#define NTP_SYNCED_LOG_FORMAT "ntp synced: %04d-%02d-%02d %02d:%02d:%02d"
#define NTP_TIMEOUT_LOG_FORMAT "ntp sync timeout retries=%d poll_ms=%lu"
#define NTP_INVALID_RETRY_COUNT_LOG_FORMAT "ntp sync invalid retry count: %d"

namespace {
constexpr const char *const kNtpServers[] = {
    "pool.ntp.org",
    "ntp.aliyun.com",
    "time.windows.com",
};
template <typename T, size_t N>
constexpr size_t array_count(const T (&)[N])
{
    return N;
}

constexpr size_t kNtpServerCount = array_count(kNtpServers);
constexpr size_t kDefaultConfiguredNtpServerSlots = 1;
#ifdef CONFIG_LWIP_SNTP_MAX_SERVERS
constexpr size_t kConfiguredNtpServerSlots = CONFIG_LWIP_SNTP_MAX_SERVERS;
#else
constexpr size_t kConfiguredNtpServerSlots = kDefaultConfiguredNtpServerSlots;
#endif
constexpr uint32_t kNtpPollDelayMs = 1000;
constexpr int kTmYearOffset = 1900;
constexpr int kTmMonthOffset = 1;
static_assert(kNtpServerCount > 0, "at least one NTP server is required");
static_assert(kConfiguredNtpServerSlots > 0, "SNTP must support at least one configured server");

constexpr size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

constexpr size_t kActiveNtpServerCount = min_size(kNtpServerCount, kConfiguredNtpServerSlots);
constexpr const char *kNtpTimeSyncedEventUnavailableLog = "skip time synced event bit: app events unavailable";

void set_time_synced_event_bit()
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "%s", kNtpTimeSyncedEventUnavailableLog);
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

void log_ntp_synced_time(const struct tm &local)
{
    ESP_LOGI(TAG, NTP_SYNCED_LOG_FORMAT,
             local.tm_year + kTmYearOffset, local.tm_mon + kTmMonthOffset, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
}

bool ntp_synced_time_available(struct tm *local)
{
    return local &&
           esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
           is_system_time_plausible(local);
}
} // namespace

bool perform_ntp_sync(int max_retries)
{
    if (max_retries <= 0) {
        ESP_LOGW(TAG, NTP_INVALID_RETRY_COUNT_LOG_FORMAT, max_retries);
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
        if (ntp_synced_time_available(&local)) {
            sync_rtc_from_system_time();
            time(&g_last_ntp_sync_time);
            set_time_synced_event_bit();
            log_ntp_synced_time(local);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kNtpPollDelayMs));
    }
    ESP_LOGW(TAG, NTP_TIMEOUT_LOG_FORMAT,
             max_retries,
             (unsigned long)kNtpPollDelayMs);
    return false;
}
