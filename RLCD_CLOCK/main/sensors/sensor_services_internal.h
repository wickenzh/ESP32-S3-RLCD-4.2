// 供应用启动和 housekeeping 内部管理传感器硬件、采样与任务生命周期。
#pragma once

#include "freertos/FreeRTOS.h"

class I2cMasterBus;

inline constexpr int kSensorSampleDayMinutes = 1;
inline constexpr int kSensorSampleNightMinutes = 2;
inline constexpr int kLocalSensorReadFailureGraceSamples = 1;

constexpr bool local_sensor_read_failure_within_grace(int consecutive_failures)
{
    return consecutive_failures > 0 &&
           consecutive_failures <= kLocalSensorReadFailureGraceSamples;
}

bool init_sensor_services_state();
void load_hourly_sensor_history();
void load_battery_charge_history();
void init_shtc3_sensor(I2cMasterBus &i2c);
bool sample_sensor();
bool sample_battery();
bool battery_charge_confirmation_pending();
void housekeeping_task(void *);
