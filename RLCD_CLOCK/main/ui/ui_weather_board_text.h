// 声明天气看板日期、温度范围和预警行的纯文本格式化接口。
#pragma once

#include "weather_types.h"

#include <stddef.h>

inline constexpr const char *kWeatherBoardDash = "--";
inline constexpr const char *kWeatherBoardShortDatePlaceholder = "--/--";
inline constexpr const char *kWeatherBoardTodayRangePlaceholder = "今日 --/--C";
inline constexpr const char *kWeatherBoardAlertPlaceholder = "预警 --";
inline constexpr const char *kWeatherBoardAirPlaceholder = "AQI --";
inline constexpr const char *kWeatherBoardHumidityPlaceholder = "湿度 --%";
inline constexpr const char *kWeatherBoardWindPlaceholder = "-- --级";
inline constexpr const char *kWeatherBoardSunrisePlaceholder = "日出 --:--";
inline constexpr const char *kWeatherBoardSunsetPlaceholder = "日落 --:--";
inline constexpr const char *kWeatherBoardAdvicePlaceholder = "等待更多天气数据";

const char *text_or_dash(const char *text);
void format_today_range(const WeatherForecastDay &day, char *out, size_t out_len);
void format_forecast_date_line(const WeatherForecastDay &day, char *out, size_t out_len);
void format_forecast_temp_range(const WeatherForecastDay &day, char *out, size_t out_len);
void format_weather_board_alert_line(const WeatherAlertData &alert, char *out, size_t out_len);
void format_weather_board_air_line(const WeatherAirData &air, char *out, size_t out_len);
void format_weather_board_humidity_line(const WeatherData &weather,
                                        const WeatherForecastDay *today,
                                        char *out,
                                        size_t out_len);
void format_weather_board_wind_line(const WeatherForecastDay *today,
                                    char *out,
                                    size_t out_len);
void format_weather_board_sunrise_line(const WeatherForecastDay *today,
                                       char *out,
                                       size_t out_len);
void format_weather_board_sunset_line(const WeatherForecastDay *today,
                                      char *out,
                                      size_t out_len);
const char *weather_board_advice_text(const WeatherForecastData &forecast, const struct tm &local);
