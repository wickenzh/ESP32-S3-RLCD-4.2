// 执行 NTP 时间同步并维护系统可信时间状态。
#include "ntp_services.h"

#include "alarm_services.h"
#include "app_constexpr.h"
#include "app_event_group.h"
#include "app_metadata.h"
#include "app_time_constants.h"
#include "housekeeping_schedule_notify.h"
#include "ntp_runtime_state_internal.h"
#include "ntp_wait_policy.h"
#include "network_sync_wait.h"
#include "rtc_services.h"
#include "sensor_time.h"
#include "ui_task_notify.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NTP_SYNCED_LOG_FORMAT "ntp synced: %04d-%02d-%02d %02d:%02d:%02d"
#define NTP_TIMEOUT_LOG_FORMAT "ntp sync timeout retries=%d poll_ms=%lu"
#define NTP_INVALID_RETRY_COUNT_LOG_FORMAT "ntp sync invalid retry count: %d"
static constexpr const char *kNtpRuntimeChangedLog =
    "ntp sync stopped after network runtime change";

namespace {
bool s_ntp_started = false;
constexpr const char *const kNtpServers[] = {
    "ntp.aliyun.com",
    "ntp.tencent.com",
    "pool.ntp.org",
};
constexpr size_t kNtpServerCount = array_count(kNtpServers);
constexpr size_t kDefaultConfiguredNtpServerSlots = kNtpServerCount;
#ifdef CONFIG_LWIP_SNTP_MAX_SERVERS
constexpr size_t kConfiguredNtpServerSlots = CONFIG_LWIP_SNTP_MAX_SERVERS;
#else
constexpr size_t kConfiguredNtpServerSlots = kDefaultConfiguredNtpServerSlots;
#endif
constexpr uint32_t kNtpPollDelayMs = 1000;
static_assert(kNtpServerCount > 0, "at least one NTP server is required");
static_assert(kConfiguredNtpServerSlots > 0, "SNTP must support at least one configured server");

constexpr size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

constexpr size_t kActiveNtpServerCount = min_size(kNtpServerCount, kConfiguredNtpServerSlots);
constexpr TickType_t kNtpPollDelay = pdMS_TO_TICKS(kNtpPollDelayMs);
constexpr TickType_t kNtpMaxFiniteWait = portMAX_DELAY - 1;
constexpr const char *kNtpTimeSyncedEventUnavailableLog = "skip time synced event bit: app events unavailable";

static_assert(cstr_array_nonempty(kNtpServers), "NTP server names must be non-empty");
static_assert(kActiveNtpServerCount > 0, "active NTP server count must be positive");
static_assert(kActiveNtpServerCount <= kNtpServerCount, "active NTP server count must fit server list");
static_assert(kActiveNtpServerCount <= kConfiguredNtpServerSlots, "active NTP server count must fit SNTP slots");
static_assert(kNtpPollDelayMs > 0, "NTP poll delay must be positive");
static_assert(kNtpPollDelay > 0, "NTP poll tick delay must be positive");
static_assert(portMAX_DELAY > 1, "NTP finite wait requires a distinct maximum tick");
static_assert(kNtpMaxFiniteWait > 0, "NTP finite wait must be positive");

void set_time_synced_event_bit()
{
    if (!app_event_group_ready()) {
        ESP_LOGW(TAG, "%s", kNtpTimeSyncedEventUnavailableLog);
        return;
    }
    app_event_group_set_bits(kTimeSyncedBit);
}

void configure_ntp_servers()
{
    for (size_t i = 0; i < kActiveNtpServerCount; ++i) {
        esp_sntp_setservername(i, kNtpServers[i]);
    }
}

void on_ntp_time_sync(struct timeval *)
{
    app_event_group_set_bits(kNtpSyncCompletedBit);
}

void start_or_restart_ntp()
{
    esp_sntp_set_time_sync_notification_cb(on_ntp_time_sync);
    if (!s_ntp_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        configure_ntp_servers();
        esp_sntp_init();
        s_ntp_started = true;
        return;
    }
    esp_sntp_restart();
}

void stop_ntp()
{
    if (!s_ntp_started) {
        return;
    }
    esp_sntp_stop();
    s_ntp_started = false;
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

NetworkSyncCompletionWaitResult wait_for_ntp_synced_time(
    int max_retries,
    struct tm *synced_time)
{
    if (!synced_time) {
        return NetworkSyncCompletionWaitResult::kTimedOut;
    }
    struct tm local = {};
    if (ntp_synced_time_available(&local)) {
        *synced_time = local;
        return NetworkSyncCompletionWaitResult::kCompleted;
    }
    const TickType_t total_wait = ntp_total_wait_ticks<TickType_t>(
        static_cast<unsigned>(max_retries),
        kNtpPollDelay,
        kNtpMaxFiniteWait);
    const uint32_t total_wait_ms =
        ntp_wait_ticks_to_milliseconds<TickType_t, uint32_t>(
            total_wait,
            configTICK_RATE_HZ,
            UINT32_MAX);
    NetworkSyncCompletionWaitResult wait_result =
        NetworkSyncCompletionWaitResult::kTimedOut;
    if (app_event_group_ready()) {
        wait_result = wait_for_ntp_sync_completion(total_wait_ms);
    } else {
        vTaskDelay(total_wait);
    }
    local = {};
    if (ntp_synced_time_available(&local)) {
        *synced_time = local;
        return NetworkSyncCompletionWaitResult::kCompleted;
    }
    return wait_result;
}
} // namespace

time_t get_last_ntp_sync_time()
{
    return ntp_last_sync_time_load();
}

bool perform_ntp_sync(int max_retries)
{
    if (max_retries <= 0) {
        ESP_LOGW(TAG, NTP_INVALID_RETRY_COUNT_LOG_FORMAT, max_retries);
        return false;
    }
    app_event_group_clear_bits(kNtpSyncCompletedBit);
    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    start_or_restart_ntp();

    struct tm local = {};
    const NetworkSyncCompletionWaitResult wait_result =
        wait_for_ntp_synced_time(max_retries, &local);
    const bool synced =
        wait_result == NetworkSyncCompletionWaitResult::kCompleted;
    stop_ntp();
    app_event_group_clear_bits(kNtpSyncCompletedBit);
    if (synced) {
        sync_rtc_from_system_time();
        const time_t sync_time = time(nullptr);
        ntp_last_sync_time_store(sync_time);
        alarm_notify_time_changed();
        notify_housekeeping_schedule_changed();
        set_time_synced_event_bit();
        notify_ui_task();
        log_ntp_synced_time(local);
        return true;
    }
    if (wait_result == NetworkSyncCompletionWaitResult::kRuntimeChanged) {
        ESP_LOGI(TAG, "%s", kNtpRuntimeChangedLog);
        return false;
    }
    ESP_LOGW(TAG, NTP_TIMEOUT_LOG_FORMAT,
             max_retries,
             (unsigned long)kNtpPollDelayMs);
    return false;
}
