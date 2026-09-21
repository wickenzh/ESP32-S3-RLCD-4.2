// 提供无需和风凭据的Open-Meteo城市、天气和配网验证入口。
#pragma once
#include "weather_update.h"
#include "weather_types.h"

enum class OpenMeteoCityStatus { kOk, kNotFound, kFailed };
OpenMeteoCityStatus open_meteo_lookup_city(const char *city, WeatherData *location);
bool open_meteo_validate_configuration();
WeatherUpdateResult perform_open_meteo_update(WeatherUpdateScope scope);
