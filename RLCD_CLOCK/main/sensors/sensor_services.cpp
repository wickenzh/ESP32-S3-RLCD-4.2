// 调度本地温湿度、电池采样和传感器相关低频后台任务。
#include "sensor_services_internal.h"

#include "app_metadata.h"
#include "app_task_readiness.h"
#include "app_tick_time.h"
#include "battery_policy.h"
#include "battery_runtime_state.h"
#include "hourly_sensor_history_state_internal.h"
#include "housekeeping_schedule_notify.h"
#include "housekeeping_wait_policy.h"
#include "local_sensor_state.h"
#include "local_sensor_state_internal.h"
#include "ota_runtime_state.h"
#include "runtime_health.h"
#include "sensor_time.h"
#include "task_notification_target.h"
#include "ui_task_notify.h"

#include "esp_log.h"

#include <atomic>

namespace {
#define SENSOR_INTERVAL_INVALID_LOG_FORMAT "sensor interval invalid: %d"
constexpr const char *kHourlySensorHistoryStateInitFailedLog =
    "hourly sensor history state initialization failed";
constexpr const char *kLocalSensorStateInitFailedLog =
    "local sensor state initialization failed";
constexpr int kMillisecondsPerSecond = 1000;
constexpr int kSecondsPerMinute = 60;
constexpr int kUnknownTimeSensorSampleMs = kSecondsPerMinute * kMillisecondsPerSecond;
constexpr uint32_t kHousekeepingFallbackDelayMs = 1000;
constexpr TickType_t kHousekeepingFallbackDelay = pdMS_TO_TICKS(kHousekeepingFallbackDelayMs);
constexpr TickType_t kBatteryChargingSampleDelay = pdMS_TO_TICKS(kBatteryChargingSampleMs);
TaskNotificationTarget s_housekeeping_task_target;
std::atomic<uint32_t> s_housekeeping_schedule_generation{0};
static_assert(kUnknownTimeSensorSampleMs > 0, "unknown-time sensor sample interval must be positive");
static_assert(kSensorSampleNightMinutes >= kSensorSampleDayMinutes,
              "night sensor sample interval must not be faster than day interval");
static_assert(kBatteryChargingSampleMs <
                  kSensorSampleDayMinutes * kSecondsPerMinute * kMillisecondsPerSecond,
              "confirmed charging samples should be faster than idle samples");
static_assert(kHousekeepingFallbackDelay > 0, "housekeeping fallback tick delay must be positive");
static_assert(kBatteryChargingSampleDelay > 0, "battery charging sample tick delay must be positive");

TickType_t next_periodic_sample_tick(TickType_t now,
                                     int day_minutes,
                                     int night_minutes,
                                     int unknown_time_ms)
{
    struct tm local = {};
    if (!is_system_time_plausible(&local)) {
        return now + pdMS_TO_TICKS(unknown_time_ms);
    }
    int interval_seconds = periodic_sample_minutes(local, day_minutes, night_minutes) *
                           kSecondsPerMinute;
    if (interval_seconds <= 0) {
        ESP_LOGW(TAG, SENSOR_INTERVAL_INVALID_LOG_FORMAT, interval_seconds);
    }
    int seconds_to_next = seconds_until_next_periodic_sample(local, interval_seconds);
    return now + pdMS_TO_TICKS(seconds_to_next * kMillisecondsPerSecond);
}

static TickType_t next_sensor_sample_tick(TickType_t now)
{
    return next_periodic_sample_tick(now,
                                     kSensorSampleDayMinutes,
                                     kSensorSampleNightMinutes,
                                     kUnknownTimeSensorSampleMs);
}

TickType_t next_housekeeping_wake_tick(bool low_battery,
                                       TickType_t now,
                                       TickType_t next_sensor,
                                       TickType_t next_battery)
{
    if (low_battery) {
        return next_battery;
    }
    return app_tick_earlier_deadline(now, next_sensor, next_battery);
}

TickType_t next_battery_wake_after_sample(TickType_t sampled_tick,
                                          bool charging)
{
    if (battery_charging_requires_fast_sampling(charging) ||
        battery_charge_confirmation_pending()) {
        return sampled_tick + kBatteryChargingSampleDelay;
    }
    return next_sensor_sample_tick(sampled_tick);
}

TickType_t delay_until_housekeeping_wake(TickType_t next_wake)
{
    TickType_t now = xTaskGetTickCount();
    return housekeeping_wait_ticks<TickType_t>(false,
                                               false,
                                               now,
                                               0,
                                               next_wake,
                                               kHousekeepingFallbackDelay,
                                               portMAX_DELAY);
}

bool system_time_became_valid(bool current_valid, bool previous_valid)
{
    return current_valid && !previous_valid;
}

bool local_sensor_sample_available()
{
    return get_local_sensor_snapshot(nullptr, nullptr, nullptr, nullptr);
}

void schedule_housekeeping_samples(TickType_t now,
                                   bool charging,
                                   TickType_t *next_sensor,
                                   TickType_t *next_battery)
{
    const TickType_t next_sample = next_sensor_sample_tick(now);
    *next_sensor = next_sample;
    *next_battery = (battery_charging_requires_fast_sampling(charging) ||
                     battery_charge_confirmation_pending())
                        ? next_battery_wake_after_sample(now, true)
                        : next_sample;
}
} // namespace

void notify_housekeeping_schedule_changed()
{
    s_housekeeping_schedule_generation.fetch_add(1, std::memory_order_release);
    (void)s_housekeeping_task_target.notify();
}

bool init_sensor_services_state()
{
    if (!init_hourly_sensor_history_state()) {
        ESP_LOGE(TAG, "%s", kHourlySensorHistoryStateInitFailedLog);
        return false;
    }
    if (!init_local_sensor_state()) {
        ESP_LOGE(TAG, "%s", kLocalSensorStateInitFailedLog);
        return false;
    }
    return true;
}

void housekeeping_task(void *)
{
    s_housekeeping_task_target.publish(xTaskGetCurrentTaskHandle());
    uint32_t observed_schedule_generation =
        s_housekeeping_schedule_generation.load(std::memory_order_acquire);
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t next_sensor = 0;
    TickType_t next_battery = 0;
    const BatteryRuntimeStatusSnapshot initial_battery_status =
        battery_runtime_status_load();
    schedule_housekeeping_samples(start_tick,
                                  initial_battery_status.charging,
                                  &next_sensor,
                                  &next_battery);
    bool last_time_valid = is_system_time_plausible();
    regular_app_task_mark_ready(RegularAppTaskId::kHousekeeping);
    if (!initial_battery_status.low_battery_mode &&
        !local_sensor_sample_available()) {
        if (sample_sensor()) {
            notify_ui_task();
        }
    }
    for (;;) {
        runtime_health_record_current_task_stack(
            RegularAppTaskId::kHousekeeping);
        runtime_health_service_periodic();
        TickType_t now = xTaskGetTickCount();
        const uint32_t schedule_generation =
            s_housekeeping_schedule_generation.load(std::memory_order_acquire);
        bool time_valid = is_system_time_plausible();
        if (schedule_generation != observed_schedule_generation ||
            system_time_became_valid(time_valid, last_time_valid)) {
            const BatteryRuntimeStatusSnapshot reschedule_battery_status =
                battery_runtime_status_load();
            schedule_housekeeping_samples(now,
                                          reschedule_battery_status.charging,
                                          &next_sensor,
                                          &next_battery);
            observed_schedule_generation = schedule_generation;
        }
        last_time_valid = time_valid;

        OtaRuntimeTimingSnapshot ota_timing = {};
        ota_runtime_timing_snapshot_load(&ota_timing);
        const bool ota_active = ota_flow_active_for_tick(
            ota_timing.state,
            ota_timing.status_hold_set,
            now,
            ota_timing.status_until_tick);
        if (ota_active) {
            TickType_t wait_ticks = housekeeping_wait_ticks<TickType_t>(
                true,
                ota_timing.status_hold_set,
                now,
                ota_timing.status_until_tick,
                next_sensor,
                kHousekeepingFallbackDelay,
                portMAX_DELAY);
            ulTaskNotifyTake(pdTRUE, wait_ticks);
            const BatteryRuntimeStatusSnapshot paused_battery_status =
                battery_runtime_status_load();
            schedule_housekeeping_samples(xTaskGetTickCount(),
                                          paused_battery_status.charging,
                                          &next_sensor,
                                          &next_battery);
            observed_schedule_generation =
                s_housekeeping_schedule_generation.load(std::memory_order_acquire);
            continue;
        }
        BatteryRuntimeStatusSnapshot battery_status =
            battery_runtime_status_load();
        bool ui_refresh_requested = false;
        if (app_tick_deadline_reached(now, next_sensor)) {
            if (!battery_status.low_battery_mode) {
                ui_refresh_requested |= sample_sensor();
            }
            next_sensor = next_sensor_sample_tick(xTaskGetTickCount());
        }
        if (app_tick_deadline_reached(now, next_battery)) {
            const bool was_low_battery = battery_status.low_battery_mode;
            ui_refresh_requested |= sample_battery();
            TickType_t after_battery = xTaskGetTickCount();
            battery_status = battery_runtime_status_load();
            if (was_low_battery && !battery_status.low_battery_mode) {
                next_sensor = next_sensor_sample_tick(after_battery);
            }
            next_battery = next_battery_wake_after_sample(
                after_battery,
                battery_status.charging);
        }
        if (ui_refresh_requested) {
            notify_ui_task();
        }
        TickType_t next_wake = next_housekeeping_wake_tick(
            battery_status.low_battery_mode,
            xTaskGetTickCount(),
            next_sensor,
            next_battery);
        TickType_t wait_ticks = delay_until_housekeeping_wake(next_wake);
        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}
