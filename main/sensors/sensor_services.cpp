// 调度本地温湿度、电池采样和传感器相关低频后台任务。
#include "sensor_services.h"

#include "ota_services.h"
#include "ui_views.h"

namespace {
constexpr TickType_t kHousekeepingOtaPauseDelay = pdMS_TO_TICKS(5000);
constexpr TickType_t kHousekeepingFallbackDelay = pdMS_TO_TICKS(1000);
} // namespace

void housekeeping_task(void *)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t next_sensor = next_sensor_sample_tick(start_tick);
    TickType_t next_battery = next_battery_sample_tick(start_tick);
    bool last_time_valid = is_system_time_plausible();
    if (!g_low_battery_mode) {
        sample_sensor();
    }
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (ota_flow_active()) {
            vTaskDelay(kHousekeepingOtaPauseDelay);
            next_sensor = next_sensor_sample_tick(xTaskGetTickCount());
            next_battery = next_battery_sample_tick(xTaskGetTickCount());
            continue;
        }
        bool time_valid = is_system_time_plausible();
        if (time_valid && !last_time_valid) {
            next_sensor = next_sensor_sample_tick(now);
            next_battery = next_battery_sample_tick(now);
        }
        last_time_valid = time_valid;
        if (now >= next_sensor) {
            if (!g_low_battery_mode) {
                sample_sensor();
            }
            next_sensor = next_sensor_sample_tick(xTaskGetTickCount());
        }
        if (now >= next_battery) {
            bool was_low_battery = g_low_battery_mode;
            sample_battery();
            TickType_t after_battery = xTaskGetTickCount();
            if (was_low_battery && !g_low_battery_mode) {
                next_sensor = next_sensor_sample_tick(after_battery);
            }
            next_battery = g_battery_charging
                               ? after_battery + pdMS_TO_TICKS(kBatteryChargingSampleMs)
                               : next_battery_sample_tick(after_battery);
        }
        TickType_t next_wake = g_low_battery_mode
                                   ? next_battery
                                   : (next_sensor < next_battery ? next_sensor : next_battery);
        TickType_t delay_now = xTaskGetTickCount();
        TickType_t delay_ticks = next_wake > delay_now ? next_wake - delay_now : kHousekeepingFallbackDelay;
        vTaskDelay(delay_ticks);
    }
}
