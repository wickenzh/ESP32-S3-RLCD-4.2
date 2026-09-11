// 声明 QWeather 每日预报 JSON 转换和本地天气建议接口。
#pragma once

#include "weather_types.h"
#include "weather_advice.h"

#include "cJSON.h"

bool parse_qweather_forecast_days(const cJSON *daily, WeatherForecastData *forecast);
