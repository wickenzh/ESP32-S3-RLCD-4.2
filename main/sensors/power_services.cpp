// 管理网络和音频期间的电源管理锁，避免关键流程被睡眠打断。
#include "sensor_services.h"

#include "ui_views.h"

#include <errno.h>

#define POWER_PM_LOCK_MUTEX_UNAVAILABLE_LOG_FORMAT "%s pm lock mutex unavailable"
#define POWER_PM_LOCK_MUTEX_TIMEOUT_LOG_FORMAT "%s pm lock mutex timeout"
#define POWER_PM_LOCK_ACQUIRE_FAILED_LOG_FORMAT "%s pm lock acquire failed: %s"
#define POWER_PM_LOCK_RELEASE_ZERO_LOG_FORMAT "%s pm lock release skipped: depth is zero"
#define POWER_PM_LOCK_RELEASE_FAILED_LOG_FORMAT "%s pm lock release failed: %s"
#define POWER_SETUP_FAILED_LOG_FORMAT "power management setup failed: %s"
#define POWER_SETUP_OK_LOG_FORMAT "power management: max=%dMHz min=%dMHz light sleep enabled"
#define POWER_MUTEX_CREATE_FAILED_LOG_FORMAT "pm lock mutex create failed"
#define POWER_NETWORK_LOCK_CREATE_FAILED_LOG_FORMAT "network pm lock create failed: %s"
#define POWER_AUDIO_LOCK_CREATE_FAILED_LOG_FORMAT "audio pm lock create failed: %s"
#define POWER_DISABLED_LOG_FORMAT "power management disabled in sdkconfig"
#define POWER_RTC_INVALID_TIME_LOG_FORMAT "ignore invalid RTC time: %04u-%02u-%02u %02u:%02u:%02u"
#define POWER_RTC_MKTIME_FAILED_LOG_FORMAT "ignore RTC time: mktime failed"
#define POWER_RTC_LOCALTIME_FAILED_LOG_FORMAT "ignore RTC time: localtime normalization failed"
#define POWER_RTC_NORMALIZED_MISMATCH_LOG_FORMAT "ignore normalized RTC time mismatch"
#define POWER_RTC_SETTIME_FAILED_LOG_FORMAT "set system time from RTC failed errno=%d"
#define POWER_RTC_RESTORED_LOG_FORMAT "system time restored from RTC: %04u-%02u-%02u %02u:%02u:%02u"
#define POWER_RTC_SYNC_LOCALTIME_FAILED_LOG_FORMAT "skip RTC sync: localtime failed"
#define POWER_RTC_SYNC_TIME_NOT_PLAUSIBLE_LOG_FORMAT "skip RTC sync: system time is not plausible"

namespace {
constexpr uint16_t kRtcMinMonth = 1;
constexpr uint16_t kRtcMaxMonth = 12;
constexpr uint16_t kRtcMinDay = 1;
constexpr uint16_t kRtcMaxDay = 31;
constexpr uint16_t kRtcMaxHour = 23;
constexpr uint16_t kRtcMaxMinute = 59;
constexpr uint16_t kRtcMaxSecond = 59;
constexpr int kTmYearOffset = 1900;
constexpr int kTmMonthOffset = 1;
constexpr const char *kNetworkPmLockName = "network_sync";
constexpr const char *kAudioPmLockName = "audio_play";
constexpr const char *kNetworkPmLogName = "network";
constexpr const char *kAudioPmLogName = "audio";

bool rtc_time_fields_in_range(const rtcTimeStruct_t &rtc_time)
{
    return rtc_time.year >= kMinValidYear &&
           rtc_time.year <= kMaxValidYear &&
           rtc_time.month >= kRtcMinMonth &&
           rtc_time.month <= kRtcMaxMonth &&
           rtc_time.day >= kRtcMinDay &&
           rtc_time.day <= kRtcMaxDay &&
           rtc_time.hour <= kRtcMaxHour &&
           rtc_time.minute <= kRtcMaxMinute &&
           rtc_time.second <= kRtcMaxSecond;
}

bool rtc_date_matches_tm(const rtcTimeStruct_t &rtc_time, const struct tm &local_time)
{
    return local_time.tm_year + kTmYearOffset == rtc_time.year &&
           local_time.tm_mon + kTmMonthOffset == rtc_time.month &&
           local_time.tm_mday == rtc_time.day;
}
} // namespace

#if CONFIG_PM_ENABLE
#include "freertos/semphr.h"

namespace {
SemaphoreHandle_t s_pm_lock_mutex = nullptr;
constexpr uint32_t kPmLockMutexTimeoutMs = 1000;
constexpr TickType_t kPmLockMutexTimeout = pdMS_TO_TICKS(kPmLockMutexTimeoutMs);

bool take_pm_lock_mutex(const char *name)
{
    if (!s_pm_lock_mutex) {
        ESP_LOGW(TAG, POWER_PM_LOCK_MUTEX_UNAVAILABLE_LOG_FORMAT, name);
        return false;
    }
    if (xSemaphoreTake(s_pm_lock_mutex, kPmLockMutexTimeout) != pdTRUE) {
        ESP_LOGW(TAG, POWER_PM_LOCK_MUTEX_TIMEOUT_LOG_FORMAT, name);
        return false;
    }
    return true;
}

void give_pm_lock_mutex()
{
    xSemaphoreGive(s_pm_lock_mutex);
}

void acquire_pm_lock(esp_pm_lock_handle_t lock, int *depth, const char *name)
{
    if (!lock || !depth || !take_pm_lock_mutex(name)) {
        return;
    }
    if (*depth == 0) {
        esp_err_t err = esp_pm_lock_acquire(lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, POWER_PM_LOCK_ACQUIRE_FAILED_LOG_FORMAT, name, esp_err_to_name(err));
            give_pm_lock_mutex();
            return;
        }
    }
    ++(*depth);
    give_pm_lock_mutex();
}

void release_pm_lock(esp_pm_lock_handle_t lock, int *depth, const char *name)
{
    if (!lock || !depth || !take_pm_lock_mutex(name)) {
        return;
    }
    if (*depth <= 0) {
        ESP_LOGW(TAG, POWER_PM_LOCK_RELEASE_ZERO_LOG_FORMAT, name);
        give_pm_lock_mutex();
        return;
    }
    --(*depth);
    if (*depth == 0) {
        esp_err_t err = esp_pm_lock_release(lock);
        if (err != ESP_OK) {
            *depth = 1;
            ESP_LOGW(TAG, POWER_PM_LOCK_RELEASE_FAILED_LOG_FORMAT, name, esp_err_to_name(err));
        }
    }
    give_pm_lock_mutex();
}
} // namespace
#endif

void init_power_management()
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    pm_config.min_freq_mhz = CONFIG_XTAL_FREQ;
    pm_config.light_sleep_enable = true;

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, POWER_SETUP_FAILED_LOG_FORMAT, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, POWER_SETUP_OK_LOG_FORMAT,
                 pm_config.max_freq_mhz, pm_config.min_freq_mhz);
    }
    s_pm_lock_mutex = xSemaphoreCreateMutex();
    if (!s_pm_lock_mutex) {
        ESP_LOGW(TAG, POWER_MUTEX_CREATE_FAILED_LOG_FORMAT);
    }
    err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, kNetworkPmLockName, &g_network_pm_lock);
    if (err != ESP_OK) {
        g_network_pm_lock = nullptr;
        ESP_LOGW(TAG, POWER_NETWORK_LOCK_CREATE_FAILED_LOG_FORMAT, esp_err_to_name(err));
    }
    err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, kAudioPmLockName, &g_audio_pm_lock);
    if (err != ESP_OK) {
        g_audio_pm_lock = nullptr;
        ESP_LOGW(TAG, POWER_AUDIO_LOCK_CREATE_FAILED_LOG_FORMAT, esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, POWER_DISABLED_LOG_FORMAT);
#endif
}

void acquire_network_awake_lock()
{
#if CONFIG_PM_ENABLE
    acquire_pm_lock(g_network_pm_lock, &g_network_pm_lock_depth, kNetworkPmLogName);
#endif
}

void release_network_awake_lock()
{
#if CONFIG_PM_ENABLE
    release_pm_lock(g_network_pm_lock, &g_network_pm_lock_depth, kNetworkPmLogName);
#endif
}

void acquire_audio_awake_lock()
{
#if CONFIG_PM_ENABLE
    acquire_pm_lock(g_audio_pm_lock, &g_audio_pm_lock_depth, kAudioPmLogName);
#endif
}

void release_audio_awake_lock()
{
#if CONFIG_PM_ENABLE
    release_pm_lock(g_audio_pm_lock, &g_audio_pm_lock_depth, kAudioPmLogName);
#endif
}

void restore_system_time_from_rtc()
{
    rtcTimeStruct_t rtc_time = {};
    Rtc_GetTime(&rtc_time);
    if (!rtc_time_fields_in_range(rtc_time)) {
        ESP_LOGW(TAG, POWER_RTC_INVALID_TIME_LOG_FORMAT,
                 rtc_time.year, rtc_time.month, rtc_time.day,
                 rtc_time.hour, rtc_time.minute, rtc_time.second);
        return;
    }
    struct tm tm_time = {};
    tm_time.tm_year = rtc_time.year - kTmYearOffset;
    tm_time.tm_mon = rtc_time.month - kTmMonthOffset;
    tm_time.tm_mday = rtc_time.day;
    tm_time.tm_hour = rtc_time.hour;
    tm_time.tm_min = rtc_time.minute;
    tm_time.tm_sec = rtc_time.second;
    time_t epoch = mktime(&tm_time);
    if (epoch == (time_t)-1) {
        ESP_LOGW(TAG, POWER_RTC_MKTIME_FAILED_LOG_FORMAT);
        return;
    }
    struct tm normalized = {};
    if (!localtime_r(&epoch, &normalized)) {
        ESP_LOGW(TAG, POWER_RTC_LOCALTIME_FAILED_LOG_FORMAT);
        return;
    }
    if (!rtc_date_matches_tm(rtc_time, normalized)) {
        ESP_LOGW(TAG, POWER_RTC_NORMALIZED_MISMATCH_LOG_FORMAT);
        return;
    }
    struct timeval now = {};
    now.tv_sec = epoch;
    if (settimeofday(&now, nullptr) != 0) {
        ESP_LOGW(TAG, POWER_RTC_SETTIME_FAILED_LOG_FORMAT, errno);
        return;
    }
    ESP_LOGI(TAG, POWER_RTC_RESTORED_LOG_FORMAT,
             rtc_time.year, rtc_time.month, rtc_time.day,
             rtc_time.hour, rtc_time.minute, rtc_time.second);
}

void sync_rtc_from_system_time()
{
    time_t now;
    time(&now);
    struct tm local = {};
    if (!localtime_r(&now, &local)) {
        ESP_LOGW(TAG, POWER_RTC_SYNC_LOCALTIME_FAILED_LOG_FORMAT);
        return;
    }
    if (!is_tm_plausible(local)) {
        ESP_LOGW(TAG, POWER_RTC_SYNC_TIME_NOT_PLAUSIBLE_LOG_FORMAT);
        return;
    }
    Rtc_SetTime(local.tm_year + kTmYearOffset,
                local.tm_mon + kTmMonthOffset,
                local.tm_mday,
                local.tm_hour,
                local.tm_min,
                local.tm_sec);
}
