// 验证天气看板日期、温度范围和多预警文本的既有格式规则。
#include "ui_weather_board_text.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main()
{
    setenv("TZ", "Asia/Shanghai", 1);
    tzset();

    assert(strcmp(text_or_dash(nullptr), kWeatherBoardDash) == 0);
    assert(strcmp(text_or_dash(""), kWeatherBoardDash) == 0);
    assert(strcmp(text_or_dash("晴"), "晴") == 0);

    WeatherForecastDay day = {};
    strcpy(day.date, "2026-07-12");
    strcpy(day.temp_min, "17");
    strcpy(day.temp_max, "26");
    char out[128] = {};
    format_forecast_date_line(day, out, sizeof(out));
    assert(strcmp(out, "周日\n12日") == 0);
    format_forecast_temp_range(day, out, sizeof(out));
    assert(strcmp(out, "17/26") == 0);
    format_today_range(day, out, sizeof(out));
    assert(strcmp(out, "今日 17/26C") == 0);

    strcpy(day.date, "invalid");
    day.temp_min[0] = '\0';
    day.temp_max[0] = '\0';
    format_forecast_date_line(day, out, sizeof(out));
    assert(strcmp(out, "--\n--/--") == 0);
    format_forecast_temp_range(day, out, sizeof(out));
    assert(strcmp(out, "--/--") == 0);
    format_today_range(day, out, sizeof(out));
    assert(strcmp(out, kWeatherBoardTodayRangePlaceholder) == 0);

    WeatherAlertData alert = {};
    format_weather_board_alert_line(alert, out, sizeof(out));
    assert(strcmp(out, kWeatherBoardAlertPlaceholder) == 0);
    alert.active = true;
    alert.count = 4;
    strcpy(alert.titles[0], "大风黄色预警");
    strcpy(alert.titles[1], "高温橙色预警");
    strcpy(alert.titles[2], "暴雨蓝色预警");
    strcpy(alert.titles[3], "雷电黄色预警");
    format_weather_board_alert_line(alert, out, sizeof(out));
    assert(strcmp(out,
                  "预警 大风黄色预警 / 高温橙色预警 / 暴雨蓝色预警") == 0);
    alert.titles[0][0] = '\0';
    format_weather_board_alert_line(alert, out, sizeof(out));
    assert(strcmp(out, kWeatherBoardAlertPlaceholder) == 0);

    WeatherAirData air = {};
    format_weather_board_air_line(air, out, sizeof(out));
    assert(strcmp(out, kWeatherBoardAirPlaceholder) == 0);
    air.ready = true;
    strcpy(air.aqi, "42");
    strcpy(air.category, "优");
    format_weather_board_air_line(air, out, sizeof(out));
    assert(strcmp(out, "AQI 42 优") == 0);

    WeatherData weather = {};
    strcpy(weather.humidity, "58");
    format_weather_board_humidity_line(weather, nullptr, out, sizeof(out));
    assert(strcmp(out, "湿度 58%") == 0);
    strcpy(day.humidity, "61");
    format_weather_board_humidity_line(weather, &day, out, sizeof(out));
    assert(strcmp(out, "湿度 61%") == 0);

    format_weather_board_wind_line(nullptr, out, sizeof(out));
    assert(strcmp(out, kWeatherBoardWindPlaceholder) == 0);
    strcpy(day.wind_dir, "东北风");
    strcpy(day.wind_scale, "3");
    format_weather_board_wind_line(&day, out, sizeof(out));
    assert(strcmp(out, "东北风 3级") == 0);

    format_weather_board_sunrise_line(nullptr, out, sizeof(out));
    assert(strcmp(out, kWeatherBoardSunrisePlaceholder) == 0);
    format_weather_board_sunset_line(nullptr, out, sizeof(out));
    assert(strcmp(out, kWeatherBoardSunsetPlaceholder) == 0);
    strcpy(day.sunrise, "05:12");
    strcpy(day.sunset, "18:47");
    format_weather_board_sunrise_line(&day, out, sizeof(out));
    assert(strcmp(out, "日出 05:12") == 0);
    format_weather_board_sunset_line(&day, out, sizeof(out));
    assert(strcmp(out, "日落 18:47") == 0);

    WeatherForecastData forecast = {};
    struct tm today={}; today.tm_year=126;today.tm_mon=6;today.tm_mday=12;
    assert(strcmp(weather_board_advice_text(forecast,today), kWeatherBoardAdvicePlaceholder) == 0);
    forecast.ready = true;
    forecast.count=2;
    forecast.days[0].valid=true;
    strcpy(forecast.days[0].date,"2026-07-11");
    strcpy(forecast.days[0].text,"小雨");
    strcpy(forecast.advice,"过期的带伞提醒");
    forecast.days[1].valid=true;
    strcpy(forecast.days[1].date,"2026-07-12");
    strcpy(forecast.days[1].text,"晴");
    strcpy(forecast.days[1].temp_max,"26");
    strcpy(forecast.days[1].temp_min,"20");
    assert(strcmp(weather_board_advice_text(forecast,today),"天气平稳，适合轻装出行。")==0);
    strcpy(forecast.days[1].text,"雷阵雨");
    assert(strcmp(weather_board_advice_text(forecast,today),"今日预报有雨雪，建议带伞。")==0);
    today.tm_mday=13;
    assert(strcmp(weather_board_advice_text(forecast,today),kWeatherBoardAdvicePlaceholder)==0);
    today.tm_mday=11;
    assert(strcmp(weather_board_advice_text(forecast,today),"今日预报有雨雪，建议带伞。")==0);
    today.tm_year=70;
    assert(strcmp(weather_board_advice_text(forecast,today),kWeatherBoardAdvicePlaceholder)==0);

    format_today_range(day, nullptr, 0);
    format_forecast_date_line(day, nullptr, 0);
    format_forecast_temp_range(day, nullptr, 0);
    format_weather_board_alert_line(alert, nullptr, 0);
    format_weather_board_air_line(air, nullptr, 0);
    format_weather_board_humidity_line(weather, &day, nullptr, 0);
    format_weather_board_wind_line(&day, nullptr, 0);
    format_weather_board_sunrise_line(&day, nullptr, 0);
    format_weather_board_sunset_line(&day, nullptr, 0);
    return 0;
}
