// 声明天气状态初始化和网络生产者使用的内部提交接口。
#pragma once

#include "weather_state.h"

bool init_weather_state();
void clear_weather_ready_event();
void invalidate_weather_configuration();
void commit_weather_update_snapshot(const WeatherData &next,
                                    const WeatherAlertData &next_alert,
                                    const WeatherForecastData &next_forecast,
                                    const WeatherAirData &next_air,
                                    bool alert_updated,
                                    bool forecast_ok,
                                    bool air_ok);
void commit_weather_basic_snapshot(const WeatherData &next,
                                   const WeatherAlertData &next_alert,
                                   bool alert_updated);
void commit_weather_resource_deferred_snapshot(
    const WeatherData &next,
    const WeatherAlertData &next_alert,
    const WeatherForecastData &next_forecast,
    bool alert_updated,
    bool forecast_ok);
