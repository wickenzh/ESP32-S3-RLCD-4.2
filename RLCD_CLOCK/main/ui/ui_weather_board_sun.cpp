// 实现天气看板日出、日落倒计时，不包含 LVGL 对象或页面布局。
#include "ui_weather_board_sun.h"

#include "ui_text_format.h"

#include <string.h>

namespace {
constexpr const char *kWeatherBoardTimeParseFormat = "%d:%d";
constexpr const char *kWeatherBoardSunCountdownFormat = "距%s %02d:%02d";
constexpr const char *kWeatherBoardSunTargetSunrise = "日出";
constexpr const char *kWeatherBoardSunTargetSunset = "日落";
constexpr int kHoursPerDay = 24;
constexpr int kMinutesPerHour = 60;
constexpr int kSecondsPerMinute = 60;

static_assert(kHoursPerDay > 0, "weather board hours per day must be positive");
static_assert(kMinutesPerHour > 0, "weather board minutes per hour must be positive");
static_assert(kSecondsPerMinute > 0, "weather board seconds per minute must be positive");

bool parse_weather_board_time(const char *text, int *hour, int *minute)
{
    if (!text || !hour || !minute) {
        return false;
    }
    int parsed_hour = 0;
    int parsed_minute = 0;
    if (sscanf(text, kWeatherBoardTimeParseFormat, &parsed_hour, &parsed_minute) != 2) {
        return false;
    }
    if (parsed_hour < 0 || parsed_hour >= kHoursPerDay ||
        parsed_minute < 0 || parsed_minute >= kMinutesPerHour) {
        return false;
    }
    *hour = parsed_hour;
    *minute = parsed_minute;
    return true;
}

time_t weather_board_time_on_day(const struct tm &local, const char *hhmm, int day_offset)
{
    int hour = 0;
    int minute = 0;
    if (!parse_weather_board_time(hhmm, &hour, &minute)) {
        return (time_t)-1;
    }
    struct tm target = local;
    target.tm_sec = 0;
    target.tm_min = minute;
    target.tm_hour = hour;
    target.tm_mday += day_offset;
    target.tm_isdst = -1;
    return mktime(&target);
}

void set_sun_countdown_placeholder(char *out, size_t out_len)
{
    ui_text::copy(out, out_len, kWeatherBoardSunCountdownPlaceholder);
}
} // namespace

const WeatherForecastDay *weather_board_forecast_day_or_null(const WeatherForecastData &forecast,
                                                              int index)
{
    if (!forecast.ready || index < 0 || index >= forecast.count || !forecast.days[index].valid) {
        return nullptr;
    }
    return &forecast.days[index];
}

int64_t weather_board_minute_key(const struct tm &local)
{
    return (((static_cast<int64_t>(local.tm_year) * 366LL + local.tm_yday) *
             kHoursPerDay + local.tm_hour) *
            kMinutesPerHour) + local.tm_min;
}

WeatherBoardSunSchedule weather_board_sun_schedule(const WeatherForecastData &forecast)
{
    WeatherBoardSunSchedule schedule;
    const WeatherForecastDay *today = weather_board_forecast_day_or_null(forecast, 0);
    if (!today || !today->sunrise[0] || !today->sunset[0]) {
        return schedule;
    }
    schedule.ready = true;
    schedule.sunrise_day_offset = today->sunrise_day_offset;
    schedule.sunset_day_offset = today->sunset_day_offset;
    strlcpy(schedule.today_sunrise,
            today->sunrise,
            sizeof(schedule.today_sunrise));
    strlcpy(schedule.today_sunset,
            today->sunset,
            sizeof(schedule.today_sunset));
    const WeatherForecastDay *tomorrow = weather_board_forecast_day_or_null(forecast, 1);
    schedule.tomorrow_sunrise_day_offset = tomorrow ? tomorrow->sunrise_day_offset : today->sunrise_day_offset;
    strlcpy(schedule.tomorrow_sunrise,
            tomorrow && tomorrow->sunrise[0] ? tomorrow->sunrise : today->sunrise,
            sizeof(schedule.tomorrow_sunrise));
    return schedule;
}

void format_weather_board_sun_countdown(const struct tm &local,
                                        const WeatherBoardSunSchedule &schedule,
                                        char *out,
                                        size_t out_len)
{
    if (!ui_text::output_buffer_available(out, out_len)) {
        return;
    }
    if (!schedule.ready ||
        !schedule.today_sunrise[0] ||
        !schedule.today_sunset[0]) {
        set_sun_countdown_placeholder(out, out_len);
        return;
    }
    struct tm now_tm = local;
    time_t now = mktime(&now_tm);
    time_t sunrise = weather_board_time_on_day(local, schedule.today_sunrise, schedule.sunrise_day_offset);
    time_t sunset = weather_board_time_on_day(local, schedule.today_sunset, schedule.sunset_day_offset);
    if (now <= 0 || sunrise <= 0 || sunset <= 0) {
        set_sun_countdown_placeholder(out, out_len);
        return;
    }
    // A fixed device timezone may put yesterday's sunset after midnight.
    // Without yesterday's solar event, do not invent its exact countdown.
    if (schedule.sunset_day_offset > 0 && now < sunrise &&
        now < weather_board_time_on_day(local, schedule.today_sunset, 0)) {
        set_sun_countdown_placeholder(out, out_len);
        return;
    }
    const char *target_name = kWeatherBoardSunTargetSunset;
    time_t target = sunset;
    if (now < sunrise) {
        target_name = kWeatherBoardSunTargetSunrise;
        target = sunrise;
    } else if (now >= sunset) {
        target_name = kWeatherBoardSunTargetSunrise;
        target = weather_board_time_on_day(local,
                                           schedule.tomorrow_sunrise[0]
                                               ? schedule.tomorrow_sunrise
                                               : schedule.today_sunrise,
                                           1 + schedule.tomorrow_sunrise_day_offset);
    }
    if (target <= now) {
        set_sun_countdown_placeholder(out, out_len);
        return;
    }
    int total_minutes = (int)((target - now + (kSecondsPerMinute - 1)) / kSecondsPerMinute);
    int hours = total_minutes / kMinutesPerHour;
    int minutes = total_minutes % kMinutesPerHour;
    ui_text::format_or_fallback(out,
                                out_len,
                                kWeatherBoardSunCountdownPlaceholder,
                                kWeatherBoardSunCountdownFormat,
                                target_name,
                                hours,
                                minutes);
}

void format_weather_board_sun_countdown(const struct tm &local,
                                        const WeatherForecastData &forecast,
                                        char *out,
                                        size_t out_len)
{
    format_weather_board_sun_countdown(local,
                                       weather_board_sun_schedule(forecast),
                                       out,
                                       out_len);
}
