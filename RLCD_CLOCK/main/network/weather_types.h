// 定义天气实时数据、预警、预报和空气质量的共享值类型。
#pragma once

#include <stdint.h>
#include <time.h>

inline constexpr int kMaxWeatherAlerts = 6;
inline constexpr int kWeatherAlertTitleLen = 64;
inline constexpr int kWeatherForecastDays = 6;
inline constexpr int kWeatherAdviceLen = 96;

struct WeatherData {
    uint32_t configuration_generation = 0;
    bool open_meteo = false;
    char city[32] = {};
    char text[32] = {};
    char icon[8] = {};
    char temp[8] = {};
    char humidity[8] = {};
    char lat[16] = {};
    char lon[16] = {};
};

struct WeatherAlertData {
    bool active = false;
    int count = 0;
    char titles[kMaxWeatherAlerts][kWeatherAlertTitleLen] = {};
    int ranks[kMaxWeatherAlerts] = {};
    time_t updated_at = 0;
};

struct WeatherForecastDay {
    bool valid = false;
    char date[12] = {};
    char text[24] = {};
    char icon[8] = {};
    char temp_max[8] = {};
    char temp_min[8] = {};
    char humidity[8] = {};
    char wind_dir[16] = {};
    char wind_scale[8] = {};
    char sunrise[8] = {};
    char sunset[8] = {};
    int8_t sunrise_day_offset = 0;
    int8_t sunset_day_offset = 0;
};

struct WeatherForecastData {
    bool ready = false;
    int count = 0;
    WeatherForecastDay days[kWeatherForecastDays] = {};
    char advice[kWeatherAdviceLen] = {};
    time_t updated_at = 0;
};

struct WeatherAirData {
    bool us_aqi = false;
    bool ready = false;
    char aqi[8] = {};
    char category[16] = {};
    char primary[16] = {};
    char pm2p5[8] = {};
    time_t updated_at = 0;
};
