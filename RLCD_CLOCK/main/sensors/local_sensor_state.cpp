// 使用静态任务互斥发布本地温湿度快照，避免普通任务占用中断临界区。
#include "local_sensor_state_internal.h"

#include "scoped_semaphore_lock.h"

#include <atomic>

namespace {
StaticTaskMutex s_local_sensor_mutex;
LocalSensorStateSnapshot s_local_sensor;
std::atomic<uint32_t> s_local_sensor_version{0};
} // namespace

bool init_local_sensor_state()
{
    return s_local_sensor_mutex.init();
}

bool local_sensor_state_publish_sample(float temperature,
                                       float humidity,
                                       int temperature_trend,
                                       int humidity_trend,
                                       bool *state_changed)
{
    if (state_changed) {
        *state_changed = false;
    }
    ScopedSemaphoreLock lock(s_local_sensor_mutex.handle());
    if (!lock) {
        return false;
    }
    const bool changed =
        !s_local_sensor.available ||
        s_local_sensor.temperature != temperature ||
        s_local_sensor.humidity != humidity ||
        s_local_sensor.temperature_trend != temperature_trend ||
        s_local_sensor.humidity_trend != humidity_trend;
    s_local_sensor.temperature = temperature;
    s_local_sensor.humidity = humidity;
    s_local_sensor.temperature_trend = temperature_trend;
    s_local_sensor.humidity_trend = humidity_trend;
    s_local_sensor.available = true;
    if (changed) {
        ++s_local_sensor.version;
        s_local_sensor_version.store(s_local_sensor.version, std::memory_order_release);
        if (state_changed) {
            *state_changed = true;
        }
    }
    return true;
}

bool local_sensor_state_publish_unavailable(bool *state_changed)
{
    if (state_changed) {
        *state_changed = false;
    }
    ScopedSemaphoreLock lock(s_local_sensor_mutex.handle());
    if (!lock) {
        return false;
    }
    if (!s_local_sensor.available) {
        return true;
    }
    s_local_sensor.available = false;
    ++s_local_sensor.version;
    s_local_sensor_version.store(s_local_sensor.version, std::memory_order_release);
    if (state_changed) {
        *state_changed = true;
    }
    return true;
}

bool local_sensor_state_snapshot_load(LocalSensorStateSnapshot *snapshot)
{
    if (!snapshot) {
        return false;
    }
    ScopedSemaphoreLock lock(s_local_sensor_mutex.handle(),pdMS_TO_TICKS(50));
    if (!lock) {
        *snapshot = {};
        return false;
    }
    *snapshot = s_local_sensor;
    return true;
}

bool get_local_sensor_snapshot(float *temperature,
                               float *humidity,
                               int *temperature_trend,
                               int *humidity_trend)
{
    LocalSensorStateSnapshot snapshot = {};
    const bool snapshot_loaded =
        local_sensor_state_snapshot_load(&snapshot);
    if (temperature) {
        *temperature = snapshot.temperature;
    }
    if (humidity) {
        *humidity = snapshot.humidity;
    }
    if (temperature_trend) {
        *temperature_trend = snapshot.temperature_trend;
    }
    if (humidity_trend) {
        *humidity_trend = snapshot.humidity_trend;
    }
    return snapshot_loaded && snapshot.available;
}

uint32_t local_sensor_state_version()
{
    return s_local_sensor_version.load(std::memory_order_acquire);
}
