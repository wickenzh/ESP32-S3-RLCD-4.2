// 声明传感器、电池、历史数据和电源服务的公共接口。
#pragma once
#include "app_state.h"

void init_power_management();
void acquire_network_awake_lock();
void release_network_awake_lock();
void acquire_audio_awake_lock();
void release_audio_awake_lock();
void restore_system_time_from_rtc();
void sync_rtc_from_system_time();
void release_battery_gauge();
bool init_battery_gauge();
int battery_percent_from_voltage(float voltage);
bool read_battery_percent(int *percent);
void sample_battery();
bool is_system_time_plausible(struct tm *local_out = nullptr);
bool is_tm_plausible(const struct tm &local);
bool is_night_slow_window(const struct tm &local);
int periodic_sample_minutes(const struct tm &local, int day_minutes, int night_minutes);
time_t hour_start_from_time(time_t value);
void reset_hourly_sensor_history();
void load_hourly_sensor_history();
void record_hourly_sensor_sample(float temp, float humi);
time_t next_weather_sync_time(time_t from);
void update_sensor_history(float temp, float humi);
void sample_sensor();
TickType_t next_sensor_sample_tick(TickType_t now);
TickType_t next_battery_sample_tick(TickType_t now);
void housekeeping_task(void *);
