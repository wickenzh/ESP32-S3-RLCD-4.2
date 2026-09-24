// 管理网络和音频期间的电源管理锁，避免关键流程被睡眠打断。
#include "power_services_internal.h"

#include "app_metadata.h"
#include "scoped_semaphore_lock.h"
#include "runtime_health.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_pm.h>

#define POWER_PM_LOCK_MUTEX_UNAVAILABLE_LOG_FORMAT "%s pm lock mutex unavailable"
#define POWER_PM_LOCK_MUTEX_TIMEOUT_LOG_FORMAT "%s pm lock mutex timeout"
#define POWER_PM_LOCK_ACQUIRE_FAILED_LOG_FORMAT "%s pm lock acquire failed: %s"
#define POWER_PM_LOCK_RELEASE_ZERO_LOG_FORMAT "%s pm lock release skipped: depth is zero"
#define POWER_PM_LOCK_RELEASE_FAILED_LOG_FORMAT "%s pm lock release failed: %s"
#define POWER_PM_LOCK_RELEASE_RECONCILED_LOG_FORMAT "%s pm lock driver already released; local depth reset"
#define POWER_SETUP_FAILED_LOG_FORMAT "power management setup failed: %s"
#define POWER_SETUP_OK_LOG_FORMAT "power management: max=%dMHz min=%dMHz light sleep enabled"
#define POWER_MUTEX_CREATE_FAILED_LOG_FORMAT "pm lock mutex create failed"
#define POWER_PM_LOCK_CREATE_FAILED_LOG_FORMAT "%s pm lock create failed: %s"
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
constexpr const char *kAudioWakePmCreateLogName = "audio wake";
constexpr const char *kAudioCpuPmCreateLogName = "audio cpu";
} // namespace

#if CONFIG_PM_ENABLE
namespace {
struct PmLockRuntime {
    esp_pm_lock_handle_t handle = nullptr;
    int depth = 0;
};

struct PmLockDescriptor {
    esp_pm_lock_type_t type;
    const char *lock_name;
    const char *log_name;
    const char *create_log_name;
    PmLockRuntime *runtime;
};

StaticTaskMutex s_pm_lock_mutex;
bool s_pm_configured = false;
PmLockRuntime s_network_pm_lock_runtime;
PmLockRuntime s_audio_pm_lock_runtime;
PmLockRuntime s_audio_wake_pm_lock_runtime;
PmLockRuntime s_audio_cpu_pm_lock_runtime;
const PmLockDescriptor kNetworkPmLock = {
    ESP_PM_NO_LIGHT_SLEEP,
    kNetworkPmLockName,
    kNetworkPmLogName,
    kNetworkPmLogName,
    &s_network_pm_lock_runtime,
};
const PmLockDescriptor kAudioPmLock = {
    ESP_PM_NO_LIGHT_SLEEP,
    kAudioPmLockName,
    kAudioPmLogName,
    kAudioPmLogName,
    &s_audio_pm_lock_runtime,
};
const PmLockDescriptor kAudioWakePmLock = {
    ESP_PM_APB_FREQ_MAX,
    kAudioWakePmLockName,
    kAudioWakePmLogName,
    kAudioWakePmCreateLogName,
    &s_audio_wake_pm_lock_runtime,
};
const PmLockDescriptor kAudioCpuPmLock = {
    ESP_PM_CPU_FREQ_MAX,
    kAudioCpuPmLockName,
    kAudioCpuPmLogName,
    kAudioCpuPmCreateLogName,
    &s_audio_cpu_pm_lock_runtime,
};
const PmLockDescriptor *const kPmLockCatalog[] = {
    &kNetworkPmLock,
    &kAudioPmLock,
    &kAudioWakePmLock,
    &kAudioCpuPmLock,
};
static_assert(sizeof(kPmLockCatalog) / sizeof(kPmLockCatalog[0]) == 4,
              "All PM locks must be registered in the initialization catalog");

constexpr uint32_t kPmLockMutexTimeoutMs = 1000;
constexpr TickType_t kPmLockMutexTimeout = pdMS_TO_TICKS(kPmLockMutexTimeoutMs);
constexpr TickType_t kPmLockReleaseMutexTimeout = portMAX_DELAY;
static_assert(kPmLockMutexTimeout > 0, "PM lock mutex tick timeout must be positive");
static_assert(kPmLockReleaseMutexTimeout > kPmLockMutexTimeout,
              "PM lock release must not abandon owned resources on timeout");

void log_pm_lock_mutex_failure(const char *name)
{
    runtime_health_note_event(RuntimeHealthEvent::kPmLockFailure);
    if (!s_pm_lock_mutex.handle()) {
        ESP_LOGW(TAG, POWER_PM_LOCK_MUTEX_UNAVAILABLE_LOG_FORMAT, name);
    } else {
        ESP_LOGW(TAG, POWER_PM_LOCK_MUTEX_TIMEOUT_LOG_FORMAT, name);
    }
}

bool create_pm_lock(const PmLockDescriptor &lock)
{
    PmLockRuntime &runtime = *lock.runtime;
    if (runtime.handle) {
        return true;
    }
    esp_err_t err = esp_pm_lock_create(lock.type, 0, lock.lock_name, &runtime.handle);
    if (err != ESP_OK) {
        runtime.handle = nullptr;
        ESP_LOGW(TAG,
                 POWER_PM_LOCK_CREATE_FAILED_LOG_FORMAT,
                 lock.create_log_name,
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

bool pm_lock_catalog_ready()
{
    for (const PmLockDescriptor *lock : kPmLockCatalog) {
        if (!lock->runtime->handle) {
            return false;
        }
    }
    return true;
}

bool acquire_pm_lock(const PmLockDescriptor &lock)
{
    PmLockRuntime &runtime = *lock.runtime;
    if (!runtime.handle) {
        return false;
    }
    ScopedSemaphoreLock state_lock(s_pm_lock_mutex.handle(),
                                   kPmLockMutexTimeout);
    if (!state_lock) {
        log_pm_lock_mutex_failure(lock.log_name);
        return false;
    }
    if (runtime.depth == 0) {
        esp_err_t err = esp_pm_lock_acquire(runtime.handle);
        if (err != ESP_OK) {
            runtime_health_note_event(RuntimeHealthEvent::kPmLockFailure);
            ESP_LOGW(TAG,
                     POWER_PM_LOCK_ACQUIRE_FAILED_LOG_FORMAT,
                     lock.log_name,
                     esp_err_to_name(err));
            return false;
        }
    }
    ++runtime.depth;
    return true;
}

bool release_pm_lock_handle(const PmLockDescriptor &lock)
{
    PmLockRuntime &runtime = *lock.runtime;
    esp_err_t err = esp_pm_lock_release(runtime.handle);
    if (err == ESP_OK) {
        return true;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        runtime.depth = 0;
        ESP_LOGW(TAG,
                 POWER_PM_LOCK_RELEASE_RECONCILED_LOG_FORMAT,
                 lock.log_name);
        return true;
    }
    ESP_LOGW(TAG,
             POWER_PM_LOCK_RELEASE_FAILED_LOG_FORMAT,
             lock.log_name,
             esp_err_to_name(err));
    runtime_health_note_event(RuntimeHealthEvent::kPmLockFailure);
    return false;
}

void release_pm_lock(const PmLockDescriptor &lock)
{
    PmLockRuntime &runtime = *lock.runtime;
    if (!runtime.handle) {
        return;
    }
    ScopedSemaphoreLock state_lock(s_pm_lock_mutex.handle(),
                                   kPmLockReleaseMutexTimeout);
    if (!state_lock) {
        log_pm_lock_mutex_failure(lock.log_name);
        return;
    }
    if (runtime.depth <= 0) {
        ESP_LOGW(TAG, POWER_PM_LOCK_RELEASE_ZERO_LOG_FORMAT, lock.log_name);
        return;
    }
    --runtime.depth;
    if (runtime.depth == 0 && !release_pm_lock_handle(lock)) {
        runtime.depth = 1;
    }
}

bool set_pm_lock_active(const PmLockDescriptor &lock, bool enabled)
{
    PmLockRuntime &runtime = *lock.runtime;
    if (!runtime.handle) {
        return false;
    }
    const TickType_t mutex_timeout =
        enabled ? kPmLockMutexTimeout : kPmLockReleaseMutexTimeout;
    ScopedSemaphoreLock state_lock(s_pm_lock_mutex.handle(), mutex_timeout);
    if (!state_lock) {
        log_pm_lock_mutex_failure(lock.log_name);
        return false;
    }
    bool active = runtime.depth > 0;
    if (enabled && !active) {
        esp_err_t err = esp_pm_lock_acquire(runtime.handle);
        if (err == ESP_OK) {
            runtime.depth = 1;
        } else {
            ESP_LOGW(TAG,
                     POWER_PM_LOCK_ACQUIRE_FAILED_LOG_FORMAT,
                     lock.log_name,
                     esp_err_to_name(err));
            return false;
        }
    } else if (!enabled && active) {
        if (release_pm_lock_handle(lock)) {
            runtime.depth = 0;
        } else {
            return false;
        }
    }
    return true;
}
} // namespace
#endif

void init_power_management()
{
#if CONFIG_PM_ENABLE
    if (s_pm_configured && pm_lock_catalog_ready()) {
        return;
    }
    if (!s_pm_lock_mutex.handle() && !s_pm_lock_mutex.init()) {
        ESP_LOGW(TAG, POWER_MUTEX_CREATE_FAILED_LOG_FORMAT);
        return;
    }
    if (!s_pm_configured) {
        esp_pm_config_t pm_config = {};
        pm_config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        pm_config.min_freq_mhz = CONFIG_XTAL_FREQ;
        pm_config.light_sleep_enable = true;

        esp_err_t err = esp_pm_configure(&pm_config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, POWER_SETUP_FAILED_LOG_FORMAT, esp_err_to_name(err));
        } else {
            s_pm_configured = true;
            ESP_LOGI(TAG, POWER_SETUP_OK_LOG_FORMAT,
                     pm_config.max_freq_mhz, pm_config.min_freq_mhz);
        }
    }
    for (const PmLockDescriptor *lock : kPmLockCatalog) {
        (void)create_pm_lock(*lock);
    }
#else
    ESP_LOGW(TAG, POWER_DISABLED_LOG_FORMAT);
#endif
}

bool acquire_network_awake_lock()
{
#if CONFIG_PM_ENABLE
    return acquire_pm_lock(kNetworkPmLock);
#else
    return true;
#endif
}

void release_network_awake_lock()
{
#if CONFIG_PM_ENABLE
    release_pm_lock(kNetworkPmLock);
#endif
}

bool network_awake_lock_active()
{
#if CONFIG_PM_ENABLE
    ScopedSemaphoreLock state_lock(s_pm_lock_mutex.handle(), kPmLockMutexTimeout);
    if (!state_lock) {
        log_pm_lock_mutex_failure(kNetworkPmLogName);
        return true;
    }
    return s_network_pm_lock_runtime.depth > 0;
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
    ScopedSemaphoreLock state_lock(s_pm_lock_mutex.handle(), kPmLockMutexTimeout);
    if (!state_lock) {
        log_pm_lock_mutex_failure(kNetworkPmLogName);
        return false;
    }
    out->network = s_network_pm_lock_runtime.depth;
    out->audio = s_audio_pm_lock_runtime.depth;
    out->audio_wake = s_audio_wake_pm_lock_runtime.depth;
    out->audio_cpu = s_audio_cpu_pm_lock_runtime.depth;
#endif
    return true;
}

bool acquire_audio_awake_lock()
{
#if CONFIG_PM_ENABLE
    if (!acquire_pm_lock(kAudioPmLock)) {
        return false;
    }
    if (!acquire_pm_lock(kAudioWakePmLock)) {
        release_pm_lock(kAudioPmLock);
        return false;
    }
    if (!set_pm_lock_active(kAudioCpuPmLock, true)) {
        release_pm_lock(kAudioWakePmLock);
        release_pm_lock(kAudioPmLock);
        return false;
    }
#endif
    return true;
}

void release_audio_awake_lock()
{
#if CONFIG_PM_ENABLE
    (void)set_audio_performance_mode(false);
    release_pm_lock(kAudioWakePmLock);
    release_pm_lock(kAudioPmLock);
#endif
}

bool set_audio_performance_mode(bool enabled)
{
#if CONFIG_PM_ENABLE
    return set_pm_lock_active(kAudioCpuPmLock, enabled);
#else
    (void)enabled;
    return true;
#endif
}
