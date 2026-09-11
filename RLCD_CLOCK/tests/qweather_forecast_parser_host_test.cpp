// 验证 QWeather 每日预报字段、裁剪和本地建议规则。
#include "qweather_forecast_parser.h"

#include "cJSON.h"

#include <assert.h>
#include <string.h>

namespace {

WeatherForecastDay weather_day(const char *text,
                                const char *temp_max,
                                const char *temp_min)
{
    WeatherForecastDay day = {};
    if (text) {
        strlcpy(day.text, text, sizeof(day.text));
    }
    if (temp_max) {
        strlcpy(day.temp_max, temp_max, sizeof(day.temp_max));
    }
    if (temp_min) {
        strlcpy(day.temp_min, temp_min, sizeof(day.temp_min));
    }
    return day;
}

void expect_advice(const WeatherForecastDay &day, const char *expected)
{
    assert(strcmp(weather_advice_for_day(day), expected) == 0);
}

} // namespace

int main()
{
    expect_advice(weather_day("小雨", "20", "12"), "今日预报有雨雪，建议带伞。");
    expect_advice(weather_day("晴", "30", "20"), "天气较热，注意防晒补水。");
    expect_advice(weather_day("晴", "18", "8"), "气温偏低，注意保暖。");
    expect_advice(weather_day("晴", "25", "15"), "早晚温差大，建议备外套。");
    expect_advice(weather_day("晴", "24", "18"), "天气平稳，适合轻装出行。");

    const char *json =
        "[{\"fxDate\":\"2026-07-13\",\"textDay\":\"小雨\",\"iconDay\":\"305\","
        "\"tempMax\":\"26\",\"tempMin\":\"20\",\"humidity\":\"70\","
        "\"windDirDay\":\"东风\",\"windScaleDay\":\"3\","
        "\"sunrise\":\"05:10\",\"sunset\":\"19:02\"},"
        "{\"fxDate\":\"2026-07-14\",\"tempMax\":\"28\"},"
        "{\"textDay\":\"晴\"}]";
    cJSON *daily = cJSON_Parse(json);
    assert(daily != nullptr);
    WeatherForecastData forecast = {};
    assert(parse_qweather_forecast_days(daily, &forecast));
    assert(forecast.ready);
    assert(forecast.count == 2);
    assert(strcmp(forecast.days[0].date, "2026-07-13") == 0);
    assert(strcmp(forecast.days[0].text, "小雨") == 0);
    assert(strcmp(forecast.days[0].icon, "305") == 0);
    assert(strcmp(forecast.days[0].temp_max, "26") == 0);
    assert(strcmp(forecast.days[0].temp_min, "20") == 0);
    assert(strcmp(forecast.days[0].humidity, "70") == 0);
    assert(strcmp(forecast.days[0].wind_dir, "东风") == 0);
    assert(strcmp(forecast.days[0].wind_scale, "3") == 0);
    assert(strcmp(forecast.days[0].sunrise, "05:10") == 0);
    assert(strcmp(forecast.days[0].sunset, "19:02") == 0);
    assert(strcmp(forecast.advice, "今日预报有雨雪，建议带伞。") == 0);
    assert(forecast.updated_at > 0);
    cJSON_Delete(daily);

    cJSON *seven_days = cJSON_CreateArray();
    assert(seven_days != nullptr);
    for (int index = 0; index < 7; ++index) {
        cJSON *item = cJSON_CreateObject();
        assert(item != nullptr);
        cJSON_AddStringToObject(item, "fxDate", "2026-07-13");
        cJSON_AddStringToObject(item, "textDay", "晴");
        cJSON_AddItemToArray(seven_days, item);
    }
    WeatherForecastData capped = {};
    assert(parse_qweather_forecast_days(seven_days, &capped));
    assert(capped.count == kWeatherForecastDays);
    cJSON_Delete(seven_days);

    cJSON *replacement_days = cJSON_Parse(
        "[{\"fxDate\":\"2026-07-20\",\"textDay\":\"晴\","
        "\"tempMax\":\"24\",\"tempMin\":\"18\"}]");
    assert(replacement_days != nullptr);
    WeatherForecastData replacement = {};
    replacement.ready = true;
    replacement.count = 1;
    strcpy(replacement.days[0].date, "stale-date");
    strcpy(replacement.advice, "stale-advice");
    replacement.updated_at = 123;
    assert(parse_qweather_forecast_days(replacement_days, &replacement));
    assert(replacement.ready);
    assert(replacement.count == 1);
    assert(strcmp(replacement.days[0].date, "2026-07-20") == 0);
    assert(strcmp(replacement.advice, "天气平稳，适合轻装出行。") == 0);
    cJSON_Delete(replacement_days);

    WeatherForecastData empty = replacement;
    assert(!parse_qweather_forecast_days(nullptr, &empty));
    assert(!empty.ready);
    assert(empty.count == 0);
    assert(empty.days[0].date[0] == '\0');
    assert(empty.advice[0] == '\0');
    assert(empty.updated_at == 0);

    cJSON *object_root = cJSON_Parse(
        "{\"day\":{\"fxDate\":\"2026-07-21\",\"textDay\":\"晴\"}}");
    assert(object_root != nullptr);
    WeatherForecastData malformed = replacement;
    assert(!parse_qweather_forecast_days(object_root, &malformed));
    assert(!malformed.ready);
    assert(malformed.count == 0);
    assert(malformed.days[0].date[0] == '\0');
    assert(malformed.advice[0] == '\0');
    assert(malformed.updated_at == 0);
    cJSON_Delete(object_root);

    cJSON *empty_array = cJSON_Parse("[]");
    assert(empty_array != nullptr);
    assert(!parse_qweather_forecast_days(empty_array, nullptr));
    cJSON_Delete(empty_array);
    return 0;
}
