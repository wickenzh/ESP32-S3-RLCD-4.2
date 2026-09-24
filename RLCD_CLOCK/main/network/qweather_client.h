// 声明 QWeather 城市、预警、实时天气、预报和空气质量查询接口。
#pragma once

#include "qweather_city_lookup_retry_policy.h"
#include "weather_types.h"

#include <stddef.h>

class HttpTextSession;

QweatherCityLookupStatus qweather_lookup_city_status(const char *location,
                                                      char *city_id,
                                                      size_t city_id_len,
                                                      char *city_name,
                                                      size_t city_name_len,
                                                      char *lat_out = nullptr,
                                                      size_t lat_len = 0,
                                                      char *lon_out = nullptr,
                                                      size_t lon_len = 0,
                                                      HttpTextSession *session = nullptr);
bool qweather_fetch_alert(const char *lat,
                          const char *lon,
                          WeatherAlertData *alert,
                          HttpTextSession *session = nullptr);
bool qweather_fetch_now(const char *city_id,
                        WeatherData *weather,
                        HttpTextSession *session = nullptr);
bool qweather_fetch_daily(const char *city_id,
                          WeatherForecastData *forecast,
                          HttpTextSession *session = nullptr);
bool qweather_fetch_air(const char *lat,
                        const char *lon,
                        WeatherAirData *air,
                        HttpTextSession *session = nullptr);
