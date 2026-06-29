// 管理网络和音频期间的电源管理锁，避免关键流程被睡眠打断。
#include "sensor_services.h"

#include "ui_views.h"

namespace {
constexpr uint16_t kRtcMinMonth = 1;
constexpr uint16_t kRtcMaxMonth = 12;
constexpr uint16_t kRtcMinDay = 1;
constexpr uint16_t kRtcMaxDay = 31;
constexpr uint16_t kRtcMaxHour = 23;
constexpr uint16_t kRtcMaxMinute = 59;
constexpr uint16_t kRtcMaxSecond = 59;

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
} // namespace

#if CONFIG_PM_ENABLE
#include "freertos/semphr.h"

namespace {
SemaphoreHandle_t s_pm_lock_mutex = nullptr;
constexpr TickType_t kPmLockMutexTimeout = pdMS_TO_TICKS(1000);

bool take_pm_lock_mutex(const char *name)
{
    if (!s_pm_lock_mutex) {
        ESP_LOGW(TAG, "%s pm lock mutex unavailable", name);
        return false;
    }
    if (xSemaphoreTake(s_pm_lock_mutex, kPmLockMutexTimeout) != pdTRUE) {
        ESP_LOGW(TAG, "%s pm lock mutex timeout", name);
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
            ESP_LOGW(TAG, "%s pm lock acquire failed: %s", name, esp_err_to_name(err));
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
        ESP_LOGW(TAG, "%s pm lock release skipped: depth is zero", name);
        give_pm_lock_mutex();
        return;
    }
    --(*depth);
    if (*depth == 0) {
        esp_err_t err = esp_pm_lock_release(lock);
        if (err != ESP_OK) {
            *depth = 1;
            ESP_LOGW(TAG, "%s pm lock release failed: %s", name, esp_err_to_name(err));
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
        ESP_LOGW(TAG, "power management setup failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "power management: max=%dMHz min=%dMHz light sleep enabled",
                 pm_config.max_freq_mhz, pm_config.min_freq_mhz);
    }
    s_pm_lock_mutex = xSemaphoreCreateMutex();
    if (!s_pm_lock_mutex) {
        ESP_LOGW(TAG, "pm lock mutex create failed");
    }
    err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "network_sync", &g_network_pm_lock);
    if (err != ESP_OK) {
        g_network_pm_lock = nullptr;
        ESP_LOGW(TAG, "network pm lock create failed: %s", esp_err_to_name(err));
    }
    err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "audio_play", &g_audio_pm_lock);
    if (err != ESP_OK) {
        g_audio_pm_lock = nullptr;
        ESP_LOGW(TAG, "audio pm lock create failed: %s", esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "power management disabled in sdkconfig");
#endif
}

void acquire_network_awake_lock()
{
#if CONFIG_PM_ENABLE
    acquire_pm_lock(g_network_pm_lock, &g_network_pm_lock_depth, "network");
#endif
}

void release_network_awake_lock()
{
#if CONFIG_PM_ENABLE
    release_pm_lock(g_network_pm_lock, &g_network_pm_lock_depth, "network");
#endif
}

void acquire_audio_awake_lock()
{
#if CONFIG_PM_ENABLE
    acquire_pm_lock(g_audio_pm_lock, &g_audio_pm_lock_depth, "audio");
#endif
}

void release_audio_awake_lock()
{
#if CONFIG_PM_ENABLE
    release_pm_lock(g_audio_pm_lock, &g_audio_pm_lock_depth, "audio");
#endif
}

void restore_system_time_from_rtc()
{
    rtcTimeStruct_t rtc_time = {};
    Rtc_GetTime(&rtc_time);
    if (!rtc_time_fields_in_range(rtc_time)) {
        ESP_LOGW(TAG, "ignore invalid RTC time: %04u-%02u-%02u %02u:%02u:%02u",
                 rtc_time.year, rtc_time.month, rtc_time.day,
                 rtc_time.hour, rtc_time.minute, rtc_time.second);
        return;
    }
    struct tm tm_time = {};
    tm_time.tm_year = rtc_time.year - 1900;
    tm_time.tm_mon = rtc_time.month - 1;
    tm_time.tm_mday = rtc_time.day;
    tm_time.tm_hour = rtc_time.hour;
    tm_time.tm_min = rtc_time.minute;
    tm_time.tm_sec = rtc_time.second;
    time_t epoch = mktime(&tm_time);
    struct tm normalized = {};
    localtime_r(&epoch, &normalized);
    if (normalized.tm_year + 1900 != rtc_time.year ||
        normalized.tm_mon + 1 != rtc_time.month ||
        normalized.tm_mday != rtc_time.day) {
        ESP_LOGW(TAG, "ignore normalized RTC time mismatch");
        return;
    }
    struct timeval now = {};
    now.tv_sec = epoch;
    if (settimeofday(&now, nullptr) != 0) {
        ESP_LOGW(TAG, "set system time from RTC failed");
        return;
    }
    ESP_LOGI(TAG, "system time restored from RTC: %04u-%02u-%02u %02u:%02u:%02u",
             rtc_time.year, rtc_time.month, rtc_time.day,
             rtc_time.hour, rtc_time.minute, rtc_time.second);
}

void sync_rtc_from_system_time()
{
    time_t now;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    if (!is_tm_plausible(local)) {
        ESP_LOGW(TAG, "skip RTC sync: system time is not plausible");
        return;
    }
    Rtc_SetTime(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}
