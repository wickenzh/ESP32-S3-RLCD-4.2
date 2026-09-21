// 声明Open-Meteo响应到共享天气数据的有界转换接口。
#pragma once
#include "weather_types.h"
#include <stddef.h>

bool parse_open_meteo_weather(const char *json, WeatherData *weather, WeatherForecastData *forecast);
bool parse_open_meteo_air(const char *json, WeatherAirData *air);
bool open_meteo_weather_code(int code, bool daylight, char *text, size_t text_size,
                            char *icon, size_t icon_size);
