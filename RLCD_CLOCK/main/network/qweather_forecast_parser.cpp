// 解析 QWeather 每日预报字段并生成本地天气建议。
#include "qweather_forecast_parser.h"

#include "network_json.h"

#include <stdlib.h>
#include <string.h>

namespace {
constexpr const char *kQweatherDailyJsonDateField = "fxDate";
constexpr const char *kQweatherDailyJsonTextDayField = "textDay";
constexpr const char *kQweatherDailyJsonIconDayField = "iconDay";
constexpr const char *kQweatherDailyJsonTempMaxField = "tempMax";
constexpr const char *kQweatherDailyJsonTempMinField = "tempMin";
constexpr const char *kQweatherDailyJsonHumidityField = "humidity";
constexpr const char *kQweatherDailyJsonWindDirDayField = "windDirDay";
constexpr const char *kQweatherDailyJsonWindScaleDayField = "windScaleDay";
constexpr const char *kQweatherDailyJsonSunriseField = "sunrise";
constexpr const char *kQweatherDailyJsonSunsetField = "sunset";

void build_weather_advice(WeatherForecastData *forecast)
{
    if (!forecast || forecast->count <= 0 || !forecast->days[0].valid) {
        return;
    }
    strlcpy(forecast->advice, weather_advice_for_day(forecast->days[0]), sizeof(forecast->advice));
}

void copy_weather_forecast_day_fields(const cJSON *item, WeatherForecastDay *day)
{
    if (!item || !day) {
        return;
    }
    json_copy_string(item, kQweatherDailyJsonDateField, day->date, sizeof(day->date));
    json_copy_string(item, kQweatherDailyJsonTextDayField, day->text, sizeof(day->text));
    json_copy_string(item, kQweatherDailyJsonIconDayField, day->icon, sizeof(day->icon));
    json_copy_string(item, kQweatherDailyJsonTempMaxField, day->temp_max, sizeof(day->temp_max));
    json_copy_string(item, kQweatherDailyJsonTempMinField, day->temp_min, sizeof(day->temp_min));
    json_copy_string(item, kQweatherDailyJsonHumidityField, day->humidity, sizeof(day->humidity));
    json_copy_string(item, kQweatherDailyJsonWindDirDayField, day->wind_dir, sizeof(day->wind_dir));
    json_copy_string(item, kQweatherDailyJsonWindScaleDayField, day->wind_scale, sizeof(day->wind_scale));
    json_copy_string(item, kQweatherDailyJsonSunriseField, day->sunrise, sizeof(day->sunrise));
    json_copy_string(item, kQweatherDailyJsonSunsetField, day->sunset, sizeof(day->sunset));
}

bool parse_weather_forecast_day(const cJSON *item, WeatherForecastDay *day)
{
    if (!cJSON_IsObject(item) || !day) {
        return false;
    }
    copy_weather_forecast_day_fields(item, day);
    day->valid = day->date[0] != '\0' &&
                 (day->text[0] != '\0' || day->temp_max[0] != '\0' || day->temp_min[0] != '\0');
    return day->valid;
}

int weather_forecast_parse_count(const cJSON *daily)
{
    int count = cJSON_GetArraySize(daily);
    return count > kWeatherForecastDays ? kWeatherForecastDays : count;
}

} // namespace

bool parse_qweather_forecast_days(const cJSON *daily, WeatherForecastData *forecast)
{
    if (!forecast) {
        return false;
    }
    *forecast = WeatherForecastData{};
    if (!cJSON_IsArray(daily)) {
        return false;
    }
    int count = weather_forecast_parse_count(daily);
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(daily, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }
        WeatherForecastDay &day = forecast->days[forecast->count];
        if (parse_weather_forecast_day(item, &day)) {
            ++forecast->count;
        }
    }
    forecast->ready = forecast->count > 0;
    if (forecast->ready) {
        time(&forecast->updated_at);
        build_weather_advice(forecast);
    }
    return forecast->ready;
}
