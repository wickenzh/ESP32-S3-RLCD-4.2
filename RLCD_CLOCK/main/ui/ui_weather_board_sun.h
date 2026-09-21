// 提供天气看板日出、日落目标选择和分钟倒计时纯计算。
#pragma once

#include "weather_types.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

inline constexpr const char *kWeatherBoardSunCountdownPlaceholder = "距日落 --:--";

struct WeatherBoardSunSchedule {
    bool ready = false;
    char today_sunrise[8] = {};
    char today_sunset[8] = {};
    char tomorrow_sunrise[8] = {};
    int8_t sunrise_day_offset = 0;
    int8_t sunset_day_offset = 0;
    int8_t tomorrow_sunrise_day_offset = 0;
};

const WeatherForecastDay *weather_board_forecast_day_or_null(const WeatherForecastData &forecast,
                                                              int index);
int64_t weather_board_minute_key(const struct tm &local);
WeatherBoardSunSchedule weather_board_sun_schedule(const WeatherForecastData &forecast);
void format_weather_board_sun_countdown(const struct tm &local,
                                        const WeatherBoardSunSchedule &schedule,
                                        char *out,
                                        size_t out_len);
void format_weather_board_sun_countdown(const struct tm &local,
                                        const WeatherForecastData &forecast,
                                        char *out,
                                        size_t out_len);
