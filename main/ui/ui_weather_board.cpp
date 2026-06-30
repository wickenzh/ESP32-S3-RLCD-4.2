// 绘制第五页天气看板，复用 QWeather 缓存并避免秒级刷新。
#include "ui_views.h"

#include "network_services.h"

namespace {

lv_obj_t *s_city_label;
lv_obj_t *s_current_icon_label;
lv_obj_t *s_current_temp_label;
lv_obj_t *s_current_unit_label;
lv_obj_t *s_current_text_label;
lv_obj_t *s_today_range_label;
lv_obj_t *s_air_label;
lv_obj_t *s_humidity_label;
lv_obj_t *s_wind_label;
lv_obj_t *s_sunrise_label;
lv_obj_t *s_sunset_label;
lv_obj_t *s_alert_label;
lv_obj_t *s_advice_label;

struct ForecastCardUi {
    lv_obj_t *box = nullptr;
    lv_obj_t *date = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *text = nullptr;
    lv_obj_t *range = nullptr;
};

ForecastCardUi s_cards[kWeatherForecastDays];
constexpr int kTmYearOffset = 1900;
constexpr int kTmMonthOffset = 1;
constexpr const char *kWeatherBoardDash = "--";
constexpr const char *kWeatherBoardShortDatePlaceholder = "--/--";
constexpr const char *kWeatherBoardUnknownIcon = "999";
constexpr const char *kWeatherBoardWaitingData = "等待数据";
constexpr const char *kWeatherBoardSyncing = "同步中";
constexpr const char *kWeatherBoardTodayRangePlaceholder = "今日 --/--C";
constexpr const char *kWeatherBoardAirPlaceholder = "AQI --";
constexpr const char *kWeatherBoardHumidityPlaceholder = "湿度 --%";
constexpr const char *kWeatherBoardWindPlaceholder = "-- --级";
constexpr const char *kWeatherBoardTimePlaceholder = "--:--";
constexpr const char *kWeatherBoardSunrisePlaceholder = "日出 --:--";
constexpr const char *kWeatherBoardSunsetPlaceholder = "日落 --:--";
constexpr const char *kWeatherBoardAlertPlaceholder = "预警 --";
constexpr const char *kWeatherBoardAdvicePlaceholder = "等待更多天气数据";
constexpr int kForecastCardX[kWeatherForecastDays] = {138, 180, 222, 264, 306, 348};
constexpr int kForecastCardY = 66;
constexpr int kForecastCardW = 34;
constexpr int kForecastCardH = 126;
constexpr int kForecastCardDateH = 30;
constexpr int kForecastCardIconY = 35;
constexpr int kForecastCardIconH = 36;
constexpr int kForecastCardTextY = 72;
constexpr int kForecastCardTextH = 34;
constexpr int kForecastCardRangeY = 108;
constexpr int kForecastCardRangeH = 16;
constexpr size_t kForecastDateLineSize = 24;
constexpr size_t kForecastShortDateSize = 8;
constexpr size_t kForecastTempRangeSize = 20;
constexpr size_t kCurrentTempLineSize = 12;
constexpr size_t kTodayRangeLineSize = 32;
constexpr size_t kWeatherBoardHumidityLineSize = 24;
constexpr size_t kWeatherBoardAirLineSize = 40;
constexpr size_t kWeatherBoardWindLineSize = 48;
constexpr size_t kWeatherBoardSunTimeLineSize = 24;
constexpr size_t kWeatherBoardAlertLineSize = 160;
static_assert(sizeof(kForecastCardX) / sizeof(kForecastCardX[0]) == kWeatherForecastDays,
              "weather forecast card positions must match forecast day count");

void set_weather_label_align(lv_obj_t *label, lv_text_align_t align)
{
    if (!label) {
        return;
    }
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
}

void set_weather_label_font(lv_obj_t *label, const lv_font_t *font)
{
    if (!label || !font) {
        return;
    }
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
}

void set_weather_label_long_mode(lv_obj_t *label, lv_label_long_mode_t mode)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, mode);
}

void style_weather_card(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

void set_card_visible(ForecastCardUi &card, bool visible)
{
    set_obj_visible(card.box, visible);
    set_obj_visible(card.date, visible);
    set_obj_visible(card.icon, visible);
    set_obj_visible(card.text, visible);
    set_obj_visible(card.range, visible);
}

const char *text_or_dash(const char *text)
{
    return text && text[0] ? text : kWeatherBoardDash;
}

const char *weather_icon_or_default(const char *icon)
{
    return weather_icon_text(icon && icon[0] ? icon : kWeatherBoardUnknownIcon);
}

const char *weekday_name_from_date(const char *date)
{
    static const char *names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    int year = 0;
    int month = 0;
    int day = 0;
    if (!date || sscanf(date, "%d-%d-%d", &year, &month, &day) != 3) {
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
    return names[tm_value.tm_wday];
}

void format_short_date(const char *date, char *out, size_t out_len)
{
    int year = 0;
    int month = 0;
    int day = 0;
    if (!out || out_len == 0) {
        return;
    }
    if (!date || sscanf(date, "%d-%d-%d", &year, &month, &day) != 3) {
        strlcpy(out, kWeatherBoardShortDatePlaceholder, out_len);
        return;
    }
    snprintf(out, out_len, "%d日", day);
}

bool update_forecast_card(ForecastCardUi &card, const WeatherForecastDay *day)
{
    bool changed = false;
    if (!day || !day->valid) {
        changed |= set_label_text_if_changed(card.date, kWeatherBoardDash);
        changed |= set_label_text_if_changed(card.icon, weather_icon_text(kWeatherBoardUnknownIcon));
        changed |= set_label_text_if_changed(card.text, kWeatherBoardDash);
        changed |= set_label_text_if_changed(card.range, kWeatherBoardShortDatePlaceholder);
        return changed;
    }
    char date_line[kForecastDateLineSize];
    char date_short[kForecastShortDateSize];
    char temp_range[kForecastTempRangeSize];
    format_short_date(day->date, date_short, sizeof(date_short));
    snprintf(date_line, sizeof(date_line), "%s\n%s", weekday_name_from_date(day->date), date_short);
    snprintf(temp_range, sizeof(temp_range), "%s/%s", text_or_dash(day->temp_min), text_or_dash(day->temp_max));
    changed |= set_label_text_if_changed(card.date, date_line);
    changed |= set_label_text_if_changed(card.icon, weather_icon_or_default(day->icon));
    changed |= set_label_text_if_changed(card.text, text_or_dash(day->text));
    changed |= set_label_text_if_changed(card.range, temp_range);
    return changed;
}

} // namespace

void build_weather_board_page()
{
    if (g_weather_board_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    g_weather_board_root = screen;
    lv_obj_add_flag(g_weather_board_root, LV_OBJ_FLAG_HIDDEN);

    build_battery_icon(screen, g_weather_board_battery_segments);
    build_work_page_status_bar(screen,
                               4,
                               &g_weather_board_date_label,
                               &g_weather_board_summary_label,
                               &g_weather_board_status_time_label,
                               true);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);

    s_city_label = make_label(screen, 20, 66, 135, 28, kWeatherBoardWaitingData);
    set_weather_label_align(s_city_label, LV_TEXT_ALIGN_LEFT);

    s_current_temp_label = make_label_with_font(screen, 20, 86, 88, 54, kWeatherBoardDash, &lv_font_montserrat_48);
    set_weather_label_align(s_current_temp_label, LV_TEXT_ALIGN_LEFT);
    s_current_unit_label = make_label_with_font(screen, 88, 96, 24, 32, "C", &lv_font_montserrat_24);
    set_weather_label_align(s_current_unit_label, LV_TEXT_ALIGN_LEFT);

    s_current_icon_label = make_label(screen, 20, 143, 42, 40, weather_icon_text(kWeatherBoardUnknownIcon));
    set_weather_label_font(s_current_icon_label, &qweather_icons_36);
    set_weather_label_align(s_current_icon_label, LV_TEXT_ALIGN_CENTER);
    s_current_text_label = make_label(screen, 62, 151, 92, 24, kWeatherBoardDash);
    s_today_range_label = make_label(screen, 20, 179, 134, 22, kWeatherBoardTodayRangePlaceholder);

    for (int i = 0; i < kWeatherForecastDays; ++i) {
        int x = kForecastCardX[i];
        int y = kForecastCardY;
        s_cards[i].box = lv_obj_create(screen);
        if (!s_cards[i].box) {
            ESP_LOGW(TAG, "weather forecast card %d create failed", i);
        } else {
            lv_obj_set_pos(s_cards[i].box, x, y);
            lv_obj_set_size(s_cards[i].box, kForecastCardW, kForecastCardH);
            style_weather_card(s_cards[i].box);
        }

        s_cards[i].date = make_label(screen, x, y, kForecastCardW, kForecastCardDateH, kWeatherBoardDash);
        set_weather_label_long_mode(s_cards[i].date, LV_LABEL_LONG_WRAP);
        set_weather_label_align(s_cards[i].date, LV_TEXT_ALIGN_CENTER);
        s_cards[i].icon = make_label(screen,
                                     x,
                                     y + kForecastCardIconY,
                                     kForecastCardW,
                                     kForecastCardIconH,
                                     weather_icon_text(kWeatherBoardUnknownIcon));
        set_weather_label_font(s_cards[i].icon, &qweather_icons_36);
        set_weather_label_align(s_cards[i].icon, LV_TEXT_ALIGN_CENTER);
        s_cards[i].text = make_label(screen,
                                     x,
                                     y + kForecastCardTextY,
                                     kForecastCardW,
                                     kForecastCardTextH,
                                     kWeatherBoardDash);
        set_weather_label_long_mode(s_cards[i].text, LV_LABEL_LONG_WRAP);
        set_weather_label_align(s_cards[i].text, LV_TEXT_ALIGN_CENTER);
        s_cards[i].range = make_label(screen,
                                      x,
                                      y + kForecastCardRangeY,
                                      kForecastCardW,
                                      kForecastCardRangeH,
                                      kWeatherBoardShortDatePlaceholder);
        set_weather_label_align(s_cards[i].range, LV_TEXT_ALIGN_CENTER);
        set_weather_label_font(s_cards[i].range, &lv_font_montserrat_12);
    }

    lv_obj_t *detail_line = make_bar(screen, 20, 196, 360, 2);
    set_obj_black(detail_line, true);
    s_air_label = make_label(screen, 20, 202, 110, 22, kWeatherBoardAirPlaceholder);
    s_humidity_label = make_label(screen, 132, 202, 86, 22, kWeatherBoardHumidityPlaceholder);
    s_wind_label = make_label(screen, 228, 202, 152, 22, kWeatherBoardWindPlaceholder);
    s_sunrise_label = make_label(screen, 20, 224, 110, 20, kWeatherBoardSunrisePlaceholder);
    s_sunset_label = make_label(screen, 132, 224, 120, 20, kWeatherBoardSunsetPlaceholder);
    s_alert_label = make_label(screen, 20, 246, 360, 22, kWeatherBoardAlertPlaceholder);
    s_advice_label = make_label(screen, 20, 272, 360, 20, kWeatherBoardAdvicePlaceholder);
    set_weather_label_long_mode(s_advice_label, LV_LABEL_LONG_WRAP);
    set_weather_label_align(s_advice_label, LV_TEXT_ALIGN_LEFT);
}

bool update_weather_board_page(const struct tm &local)
{
    build_weather_board_page();
    if (!g_weather_board_root) {
        return false;
    }

    WeatherData weather = {};
    WeatherAlertData alert = {};
    WeatherForecastData forecast = {};
    WeatherAirData air = {};
    get_weather_snapshot(&weather, &alert);
    get_weather_forecast_snapshot(&forecast);
    get_weather_air_snapshot(&air);
    EventBits_t bits = xEventGroupGetBits(g_app_events);
    bool weather_ready = (bits & kWeatherReadyBit) != 0;
    bool changed = update_work_page_status_time(g_weather_board_status_time_label, local);
    changed |= update_work_page_sensor_summary(g_weather_board_summary_label);

    if (weather_ready) {
        char temp_line[kCurrentTempLineSize];
        char today_range[kTodayRangeLineSize];
        snprintf(temp_line, sizeof(temp_line), "%s", text_or_dash(weather.temp));
        changed |= set_label_text_if_changed(s_city_label, text_or_dash(weather.city));
        changed |= set_label_text_if_changed(s_current_temp_label, temp_line);
        changed |= set_label_text_if_changed(s_current_icon_label, weather_icon_or_default(weather.icon));
        changed |= set_label_text_if_changed(s_current_text_label, text_or_dash(weather.text));
        if (forecast.ready && forecast.count > 0 && forecast.days[0].valid) {
            snprintf(today_range, sizeof(today_range), "今日 %s/%sC",
                     text_or_dash(forecast.days[0].temp_min),
                     text_or_dash(forecast.days[0].temp_max));
        } else {
            strlcpy(today_range, kWeatherBoardTodayRangePlaceholder, sizeof(today_range));
        }
        changed |= set_label_text_if_changed(s_today_range_label, today_range);
    } else {
        changed |= set_label_text_if_changed(s_city_label, kWeatherBoardDash);
        changed |= set_label_text_if_changed(s_current_temp_label, kWeatherBoardDash);
        changed |= set_label_text_if_changed(s_current_icon_label, weather_icon_text(kWeatherBoardUnknownIcon));
        changed |= set_label_text_if_changed(s_current_text_label,
                                             (bits & kWifiConnectedBit) ? kWeatherBoardSyncing : kWeatherBoardWaitingData);
        changed |= set_label_text_if_changed(s_today_range_label, kWeatherBoardTodayRangePlaceholder);
    }

    for (int i = 0; i < kWeatherForecastDays; ++i) {
        const WeatherForecastDay *day = (forecast.ready && i < forecast.count) ? &forecast.days[i] : nullptr;
        set_card_visible(s_cards[i], true);
        changed |= update_forecast_card(s_cards[i], day);
    }

    char humi_line[kWeatherBoardHumidityLineSize];
    char air_line[kWeatherBoardAirLineSize];
    char wind_line[kWeatherBoardWindLineSize];
    char sunrise_line[kWeatherBoardSunTimeLineSize];
    char sunset_line[kWeatherBoardSunTimeLineSize];
    char alert_line[kWeatherBoardAlertLineSize];
    const WeatherForecastDay *today = (forecast.ready && forecast.count > 0 && forecast.days[0].valid) ? &forecast.days[0] : nullptr;
    if (air.ready) {
        snprintf(air_line, sizeof(air_line), "AQI %s %s",
                 text_or_dash(air.aqi),
                 text_or_dash(air.category));
    } else {
        snprintf(air_line, sizeof(air_line), "%s", kWeatherBoardAirPlaceholder);
    }
    snprintf(humi_line, sizeof(humi_line), "湿度 %s%%",
             today && today->humidity[0] ? today->humidity : text_or_dash(weather.humidity));
    snprintf(wind_line, sizeof(wind_line), "%s %s级",
             today ? text_or_dash(today->wind_dir) : "--",
             today ? text_or_dash(today->wind_scale) : "--");
    snprintf(sunrise_line, sizeof(sunrise_line), "日出 %s",
             today && today->sunrise[0] ? today->sunrise : kWeatherBoardTimePlaceholder);
    snprintf(sunset_line, sizeof(sunset_line), "日落 %s",
             today && today->sunset[0] ? today->sunset : kWeatherBoardTimePlaceholder);
    if (alert.active && alert.count > 0 && alert.titles[0][0]) {
        strlcpy(alert_line, "预警 ", sizeof(alert_line));
        for (int i = 0; i < alert.count && i < 3; ++i) {
            if (i > 0) {
                strlcat(alert_line, " / ", sizeof(alert_line));
            }
            strlcat(alert_line, alert.titles[i], sizeof(alert_line));
        }
    } else {
        snprintf(alert_line, sizeof(alert_line), "%s", kWeatherBoardAlertPlaceholder);
    }
    changed |= set_label_text_if_changed(s_air_label, air_line);
    changed |= set_label_text_if_changed(s_humidity_label, humi_line);
    changed |= set_label_text_if_changed(s_wind_label, wind_line);
    changed |= set_label_text_if_changed(s_sunrise_label, sunrise_line);
    changed |= set_label_text_if_changed(s_sunset_label, sunset_line);
    changed |= set_label_text_if_changed(s_alert_label, alert_line);
    changed |= set_label_text_if_changed(s_advice_label,
                                         forecast.ready && forecast.advice[0] ? forecast.advice : kWeatherBoardAdvicePlaceholder);
    return changed;
}
