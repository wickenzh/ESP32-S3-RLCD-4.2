// 实现天气看板不依赖 LVGL 的日期、温度范围和预警文本格式化。
#include "ui_weather_board_text.h"
#include "weather_advice.h"

#include "app_constexpr.h"
#include "app_time_constants.h"
#include "ui_text_format.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {
constexpr const char *kForecastDateFormat = "%d-%d-%d";
constexpr int kForecastDateFieldCount = 3;
constexpr int kWeatherBoardWeekdayCount = 7;
constexpr const char *kWeatherBoardWeekdayNames[kWeatherBoardWeekdayCount] = {
    "周日", "周一", "周二", "周三", "周四", "周五", "周六",
};
constexpr const char *kForecastShortDateFormat = "%d日";
constexpr const char *kForecastDateLineFormat = "%s\n%s";
constexpr const char *kForecastTempRangeFormat = "%s/%s";
constexpr const char *kWeatherBoardTodayRangeFormat = "今日 %s/%sC";
constexpr const char *kWeatherBoardAlertPrefix = "预警 ";
constexpr const char *kWeatherBoardAlertSeparator = " / ";
constexpr const char *kWeatherBoardTimePlaceholder = "--:--";
constexpr const char *kWeatherBoardAirFormat = "AQI %s %s";
constexpr const char *kWeatherBoardHumidityFormat = "湿度 %s%%";
constexpr const char *kWeatherBoardWindFormat = "%s %s级";
constexpr const char *kWeatherBoardSunriseFormat = "日出 %s";
constexpr const char *kWeatherBoardSunsetFormat = "日落 %s";
constexpr int kWeatherBoardMaxAlertTitles = 3;
constexpr size_t kForecastShortDateSize = 8;

static_assert(array_count(kWeatherBoardWeekdayNames) == kWeatherBoardWeekdayCount,
              "weather board weekday names must match weekday count");
static_assert(kWeatherBoardMaxAlertTitles > 0 &&
                  kWeatherBoardMaxAlertTitles <= kMaxWeatherAlerts,
              "weather board alert display limit must fit alert storage");

bool parse_forecast_date(const char *date, int &year, int &month, int &day)
{
    if (!date) {
        return false;
    }
    return sscanf(date, kForecastDateFormat, &year, &month, &day) ==
           kForecastDateFieldCount;
}

const char *weekday_name_from_date(const char *date)
{
    int year = 0;
    int month = 0;
    int day = 0;
    if (!parse_forecast_date(date, year, month, day)) {
        return kWeatherBoardDash;
    }
    struct tm tm_value = {};
    tm_value.tm_year = year - kTmYearOffset;
    tm_value.tm_mon = month - kTmMonthOffset;
    tm_value.tm_mday = day;
    tm_value.tm_isdst = -1;
    time_t epoch = mktime(&tm_value);
    if (epoch <= 0) {
        return kWeatherBoardDash;
    }
    localtime_r(&epoch, &tm_value);
    if (tm_value.tm_wday < 0 || tm_value.tm_wday >= kWeatherBoardWeekdayCount) {
        return kWeatherBoardDash;
    }
    return kWeatherBoardWeekdayNames[tm_value.tm_wday];
}

void format_short_date(const char *date, char *out, size_t out_len)
{
    int year = 0;
    int month = 0;
    int day = 0;
    if (!ui_text::output_buffer_available(out, out_len)) {
        return;
    }
    if (!parse_forecast_date(date, year, month, day)) {
        strlcpy(out, kWeatherBoardShortDatePlaceholder, out_len);
        return;
    }
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardShortDatePlaceholder,
                                kForecastShortDateFormat,
                                day);
}
} // namespace

const char *text_or_dash(const char *text)
{
    return text && text[0] ? text : kWeatherBoardDash;
}

void format_today_range(const WeatherForecastDay &day, char *out, size_t out_len)
{
    if (!ui_text::output_buffer_available(out, out_len)) {
        return;
    }
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardTodayRangePlaceholder,
                                kWeatherBoardTodayRangeFormat,
                                text_or_dash(day.temp_min),
                                text_or_dash(day.temp_max));
}

void format_forecast_date_line(const WeatherForecastDay &day, char *out, size_t out_len)
{
    if (!ui_text::output_buffer_available(out, out_len)) {
        return;
    }
    char date_short[kForecastShortDateSize] = {};
    format_short_date(day.date, date_short, sizeof(date_short));
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardShortDatePlaceholder,
                                kForecastDateLineFormat,
                                weekday_name_from_date(day.date),
                                date_short);
}

void format_forecast_temp_range(const WeatherForecastDay &day, char *out, size_t out_len)
{
    if (!ui_text::output_buffer_available(out, out_len)) {
        return;
    }
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardShortDatePlaceholder,
                                kForecastTempRangeFormat,
                                text_or_dash(day.temp_min),
                                text_or_dash(day.temp_max));
}

void format_weather_board_alert_line(const WeatherAlertData &alert,
                                     char *out,
                                     size_t out_len)
{
    if (!ui_text::output_buffer_available(out, out_len)) {
        return;
    }
    if (!alert.active || alert.count <= 0 || !alert.titles[0][0]) {
        strlcpy(out, kWeatherBoardAlertPlaceholder, out_len);
        return;
    }
    strlcpy(out, kWeatherBoardAlertPrefix, out_len);
    for (int i = 0; i < alert.count && i < kWeatherBoardMaxAlertTitles; ++i) {
        if (i > 0) {
            strlcat(out, kWeatherBoardAlertSeparator, out_len);
        }
        strlcat(out, alert.titles[i], out_len);
    }
}

void format_weather_board_air_line(const WeatherAirData &air, char *out, size_t out_len)
{
    if (!air.ready) {
        ui_text::copy(out, out_len, kWeatherBoardAirPlaceholder);
        return;
    }
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardAirPlaceholder,
                                kWeatherBoardAirFormat,
                                text_or_dash(air.aqi),
                                text_or_dash(air.category));
}

void format_weather_board_humidity_line(const WeatherData &weather,
                                        const WeatherForecastDay *today,
                                        char *out,
                                        size_t out_len)
{
    const char *humidity = today && today->humidity[0]
                               ? today->humidity
                               : text_or_dash(weather.humidity);
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardHumidityPlaceholder,
                                kWeatherBoardHumidityFormat,
                                humidity);
}

void format_weather_board_wind_line(const WeatherForecastDay *today,
                                    char *out,
                                    size_t out_len)
{
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardWindPlaceholder,
                                kWeatherBoardWindFormat,
                                today ? text_or_dash(today->wind_dir) : kWeatherBoardDash,
                                today ? text_or_dash(today->wind_scale) : kWeatherBoardDash);
}

void format_weather_board_sunrise_line(const WeatherForecastDay *today,
                                       char *out,
                                       size_t out_len)
{
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardSunrisePlaceholder,
                                kWeatherBoardSunriseFormat,
                                today && today->sunrise[0]
                                    ? today->sunrise
                                    : kWeatherBoardTimePlaceholder);
}

void format_weather_board_sunset_line(const WeatherForecastDay *today,
                                      char *out,
                                      size_t out_len)
{
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardSunsetPlaceholder,
                                kWeatherBoardSunsetFormat,
                                today && today->sunset[0]
                                    ? today->sunset
                                    : kWeatherBoardTimePlaceholder);
}

const char *weather_board_advice_text(const WeatherForecastData &forecast, const struct tm &local)
{
    if(!forecast.ready || local.tm_year<100 || local.tm_mon<0 || local.tm_mon>11 ||
       local.tm_mday<1 || local.tm_mday>31) return kWeatherBoardAdvicePlaceholder;
    char date[12]={};
    if(!strftime(date,sizeof(date),"%Y-%m-%d",&local))return kWeatherBoardAdvicePlaceholder;
    for(int i=0;i<forecast.count && i<kWeatherForecastDays;++i) {
        const auto &day=forecast.days[i];
        if(day.valid && strcmp(day.date,date)==0) return weather_advice_for_day(day);
    }
    return kWeatherBoardAdvicePlaceholder;
}
