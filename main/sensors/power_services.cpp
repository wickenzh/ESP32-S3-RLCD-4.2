// 管理网络和音频期间的电源管理锁，避免关键流程被睡眠打断。
#include "sensor_services.h"

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
#define POWER_AUDIO_WAKE_LOCK_CREATE_FAILED_LOG_FORMAT "audio wake pm lock create failed: %s"
#define POWER_AUDIO_CPU_LOCK_CREATE_FAILED_LOG_FORMAT "audio cpu pm lock create failed: %s"
#define POWER_DISABLED_LOG_FORMAT "power management disabled in sdkconfig"

namespace {
constexpr const char *kNetworkPmLockName = "network_sync";
constexpr const char *kAudioPmLockName = "audio_play";
constexpr const char *kAudioWakePmLockName = "audio_wake_80";
constexpr const char *kAudioCpuPmLockName = "audio_cpu_max";
constexpr const char *kNetworkPmLogName = "network";
constexpr const char *kAudioPmLogName = "audio";
constexpr const char *kAudioWakePmLogName = "audio_wake";
constexpr const char *kAudioCpuPmLogName = "audio_cpu";
} // namespace

#if CONFIG_PM_ENABLE
#include "freertos/semphr.h"

namespace {
StaticSemaphore_t s_pm_lock_mutex_storage = {};
SemaphoreHandle_t s_pm_lock_mutex = nullptr;
esp_pm_lock_handle_t s_network_pm_lock = nullptr;
esp_pm_lock_handle_t s_audio_pm_lock = nullptr;
esp_pm_lock_handle_t s_audio_wake_pm_lock = nullptr;
esp_pm_lock_handle_t s_audio_cpu_pm_lock = nullptr;
int s_network_pm_lock_depth = 0;
int s_audio_pm_lock_depth = 0;
int s_audio_wake_pm_lock_depth = 0;
int s_audio_cpu_pm_lock_depth = 0;
constexpr uint32_t kPmLockMutexTimeoutMs = 1000;
constexpr TickType_t kPmLockMutexTimeout = pdMS_TO_TICKS(kPmLockMutexTimeoutMs);
static_assert(kPmLockMutexTimeout > 0, "PM lock mutex tick timeout must be positive");

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

bool acquire_pm_lock(esp_pm_lock_handle_t lock, int *depth, const char *name)
{
    if (!lock || !depth || !take_pm_lock_mutex(name)) {
        return false;
    }
    if (*depth == 0) {
        esp_err_t err = esp_pm_lock_acquire(lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, POWER_PM_LOCK_ACQUIRE_FAILED_LOG_FORMAT, name, esp_err_to_name(err));
            give_pm_lock_mutex();
            return false;
        }
    }
    ++(*depth);
    give_pm_lock_mutex();
    return true;
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

bool set_pm_lock_active(esp_pm_lock_handle_t lock, int *depth, const char *name, bool enabled)
{
    if (!lock || !depth || !take_pm_lock_mutex(name)) {
        return false;
    }
    bool active = *depth > 0;
    if (enabled && !active) {
        esp_err_t err = esp_pm_lock_acquire(lock);
        if (err == ESP_OK) {
            *depth = 1;
        } else {
            ESP_LOGW(TAG, POWER_PM_LOCK_ACQUIRE_FAILED_LOG_FORMAT, name, esp_err_to_name(err));
            give_pm_lock_mutex();
            return false;
        }
    } else if (!enabled && active) {
        esp_err_t err = esp_pm_lock_release(lock);
        if (err == ESP_OK) {
            *depth = 0;
        } else {
            ESP_LOGW(TAG, POWER_PM_LOCK_RELEASE_FAILED_LOG_FORMAT, name, esp_err_to_name(err));
            give_pm_lock_mutex();
            return false;
        }
    }
    give_pm_lock_mutex();
    return true;
}
} // namespace
#endif

void init_power_management()
{
#if CONFIG_PM_ENABLE
    if (s_pm_lock_mutex) {
        return;
    }
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
    s_pm_lock_mutex = xSemaphoreCreateMutexStatic(&s_pm_lock_mutex_storage);
    if (!s_pm_lock_mutex) {
        ESP_LOGW(TAG, POWER_MUTEX_CREATE_FAILED_LOG_FORMAT);
    }
    err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, kNetworkPmLockName, &s_network_pm_lock);
    if (err != ESP_OK) {
        s_network_pm_lock = nullptr;
        ESP_LOGW(TAG, POWER_NETWORK_LOCK_CREATE_FAILED_LOG_FORMAT, esp_err_to_name(err));
    }
    err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, kAudioPmLockName, &s_audio_pm_lock);
    if (err != ESP_OK) {
        s_audio_pm_lock = nullptr;
        ESP_LOGW(TAG, POWER_AUDIO_LOCK_CREATE_FAILED_LOG_FORMAT, esp_err_to_name(err));
    }
    err = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, kAudioWakePmLockName, &s_audio_wake_pm_lock);
    if (err != ESP_OK) {
        s_audio_wake_pm_lock = nullptr;
        ESP_LOGW(TAG, POWER_AUDIO_WAKE_LOCK_CREATE_FAILED_LOG_FORMAT, esp_err_to_name(err));
    }
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, kAudioCpuPmLockName, &s_audio_cpu_pm_lock);
    if (err != ESP_OK) {
        s_audio_cpu_pm_lock = nullptr;
        ESP_LOGW(TAG, POWER_AUDIO_CPU_LOCK_CREATE_FAILED_LOG_FORMAT, esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, POWER_DISABLED_LOG_FORMAT);
#endif
}

bool acquire_network_awake_lock()
{
#if CONFIG_PM_ENABLE
    return acquire_pm_lock(s_network_pm_lock, &s_network_pm_lock_depth, kNetworkPmLogName);
#else
    return true;
#endif
}

void release_network_awake_lock()
{
#if CONFIG_PM_ENABLE
    release_pm_lock(s_network_pm_lock, &s_network_pm_lock_depth, kNetworkPmLogName);
#endif
}

bool network_awake_lock_active()
{
#if CONFIG_PM_ENABLE
    if (!take_pm_lock_mutex(kNetworkPmLogName)) {
        return true;
    }
    bool active = s_network_pm_lock_depth > 0;
    give_pm_lock_mutex();
    return active;
#else
    return false;
#endif
}

bool get_power_lock_depth_snapshot(PowerLockDepthSnapshot *out)
{
    if (!out) {
        return false;
    }
    *out = {};
#if CONFIG_PM_ENABLE
    if (!take_pm_lock_mutex(kNetworkPmLogName)) {
        return false;
    }
    out->network = s_network_pm_lock_depth;
    out->audio = s_audio_pm_lock_depth;
    out->audio_wake = s_audio_wake_pm_lock_depth;
    out->audio_cpu = s_audio_cpu_pm_lock_depth;
    give_pm_lock_mutex();
#endif
    return true;
}

bool acquire_audio_awake_lock()
{
#if CONFIG_PM_ENABLE
    if (!acquire_pm_lock(s_audio_pm_lock, &s_audio_pm_lock_depth, kAudioPmLogName)) {
        return false;
    }
    if (!acquire_pm_lock(s_audio_wake_pm_lock,
                         &s_audio_wake_pm_lock_depth,
                         kAudioWakePmLogName)) {
        release_pm_lock(s_audio_pm_lock, &s_audio_pm_lock_depth, kAudioPmLogName);
        return false;
    }
    if (!set_pm_lock_active(s_audio_cpu_pm_lock,
                            &s_audio_cpu_pm_lock_depth,
                            kAudioCpuPmLogName,
                            true)) {
        release_pm_lock(s_audio_wake_pm_lock,
                        &s_audio_wake_pm_lock_depth,
                        kAudioWakePmLogName);
        release_pm_lock(s_audio_pm_lock, &s_audio_pm_lock_depth, kAudioPmLogName);
        return false;
    }
#endif
    return true;
}

void release_audio_awake_lock()
{
#if CONFIG_PM_ENABLE
    set_audio_performance_mode(false);
    release_pm_lock(s_audio_wake_pm_lock, &s_audio_wake_pm_lock_depth, kAudioWakePmLogName);
    release_pm_lock(s_audio_pm_lock, &s_audio_pm_lock_depth, kAudioPmLogName);
#endif
}

void set_audio_performance_mode(bool enabled)
{
#if CONFIG_PM_ENABLE
    (void)set_pm_lock_active(s_audio_cpu_pm_lock,
                             &s_audio_cpu_pm_lock_depth,
                             kAudioCpuPmLogName,
                             enabled);
#else
    (void)enabled;
#endif
}
