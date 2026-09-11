// 预报解析与页面共用的本地天气建议规则，不依赖JSON或LVGL。
#pragma once
#include "weather_types.h"
#include <stdlib.h>
#include <string.h>
namespace weather_advice_detail {
constexpr int kWeatherAdviceHotTempC = 30;
constexpr int kWeatherAdviceColdTempC = 8;
constexpr int kWeatherAdviceLargeTempGapC = 10;
constexpr const char *kWeatherAdviceRainOrSnow = "今日预报有雨雪，建议带伞。";
constexpr const char *kWeatherAdviceHot = "天气较热，注意防晒补水。";
constexpr const char *kWeatherAdviceCold = "气温偏低，注意保暖。";
constexpr const char *kWeatherAdviceLargeTempGap = "早晚温差大，建议备外套。";
constexpr const char *kWeatherAdviceCalm = "天气平稳，适合轻装出行。";
inline int weather_text_to_int(const char *text, int fallback = 0)
{
    return text && text[0] ? atoi(text) : fallback;
}

}
inline const char *weather_advice_for_day(const WeatherForecastDay &today)
{
    using namespace weather_advice_detail;
    int temp_max = weather_text_to_int(today.temp_max);
    int temp_min = weather_text_to_int(today.temp_min, temp_max);
    const char *text = today.text;
    if (text && (strstr(text, "雨") || strstr(text, "雪"))) {
        return kWeatherAdviceRainOrSnow;
    }
    if (temp_max >= kWeatherAdviceHotTempC) {
        return kWeatherAdviceHot;
    }
    if (temp_min <= kWeatherAdviceColdTempC) {
        return kWeatherAdviceCold;
    }
    if (temp_max - temp_min >= kWeatherAdviceLargeTempGapC) {
        return kWeatherAdviceLargeTempGap;
    }
    return kWeatherAdviceCalm;
}
