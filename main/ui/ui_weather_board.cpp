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

const char *weekday_name_from_date(const char *date)
{
    static const char *names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    int year = 0;
    int month = 0;
    int day = 0;
    if (!date || sscanf(date, "%d-%d-%d", &year, &month, &day) != 3) {
        return "--";
    }
    struct tm tm_value = {};
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month - 1;
    tm_value.tm_mday = day;
    tm_value.tm_isdst = -1;
    time_t epoch = mktime(&tm_value);
    if (epoch <= 0) {
        return "--";
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
        strlcpy(out, "--/--", out_len);
        return;
    }
    snprintf(out, out_len, "%d日", day);
}

bool update_forecast_card(ForecastCardUi &card, const WeatherForecastDay *day)
{
    bool changed = false;
    if (!day || !day->valid) {
        changed |= set_label_text_if_changed(card.date, "--");
        changed |= set_label_text_if_changed(card.icon, weather_icon_text("999"));
        changed |= set_label_text_if_changed(card.text, "--");
        changed |= set_label_text_if_changed(card.range, "--/--");
        return changed;
    }
    char date_line[24];
    char date_short[8];
    char temp_range[20];
    format_short_date(day->date, date_short, sizeof(date_short));
    snprintf(date_line, sizeof(date_line), "%s\n%s", weekday_name_from_date(day->date), date_short);
    snprintf(temp_range, sizeof(temp_range), "%s/%s", day->temp_min[0] ? day->temp_min : "--", day->temp_max[0] ? day->temp_max : "--");
    changed |= set_label_text_if_changed(card.date, date_line);
    changed |= set_label_text_if_changed(card.icon, weather_icon_text(day->icon[0] ? day->icon : "999"));
    changed |= set_label_text_if_changed(card.text, day->text[0] ? day->text : "--");
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

    s_city_label = make_label(screen, 20, 66, 135, 28, "等待数据");
    lv_obj_set_style_text_align(s_city_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

    s_current_temp_label = make_label_with_font(screen, 20, 86, 88, 54, "--", &lv_font_montserrat_48);
    lv_obj_set_style_text_align(s_current_temp_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    s_current_unit_label = make_label_with_font(screen, 88, 96, 24, 32, "C", &lv_font_montserrat_24);
    lv_obj_set_style_text_align(s_current_unit_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

    s_current_icon_label = make_label(screen, 20, 143, 42, 40, weather_icon_text("999"));
    lv_obj_set_style_text_font(s_current_icon_label, &qweather_icons_36, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_current_icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    s_current_text_label = make_label(screen, 62, 151, 92, 24, "--");
    s_today_range_label = make_label(screen, 20, 179, 134, 22, "今日 --/--C");

    static const int kCardX[kWeatherForecastDays] = {138, 180, 222, 264, 306, 348};
    static const int kCardY = 66;
    for (int i = 0; i < kWeatherForecastDays; ++i) {
        int x = kCardX[i];
        int y = kCardY;
        s_cards[i].box = lv_obj_create(screen);
        if (!s_cards[i].box) {
            ESP_LOGW(TAG, "weather forecast card %d create failed", i);
        } else {
            lv_obj_set_pos(s_cards[i].box, x, y);
            lv_obj_set_size(s_cards[i].box, 34, 126);
            style_weather_card(s_cards[i].box);
        }

        s_cards[i].date = make_label(screen, x, y, 34, 30, "--");
        lv_label_set_long_mode(s_cards[i].date, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_cards[i].date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        s_cards[i].icon = make_label(screen, x, y + 35, 34, 36, weather_icon_text("999"));
        lv_obj_set_style_text_font(s_cards[i].icon, &qweather_icons_36, LV_PART_MAIN);
        lv_obj_set_style_text_align(s_cards[i].icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        s_cards[i].text = make_label(screen, x, y + 72, 34, 34, "--");
        lv_label_set_long_mode(s_cards[i].text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_cards[i].text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        s_cards[i].range = make_label(screen, x, y + 108, 34, 16, "--/--");
        lv_obj_set_style_text_align(s_cards[i].range, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_cards[i].range, &lv_font_montserrat_12, LV_PART_MAIN);
    }

    lv_obj_t *detail_line = make_bar(screen, 20, 196, 360, 2);
    set_obj_black(detail_line, true);
    s_air_label = make_label(screen, 20, 202, 110, 22, "AQI --");
    s_humidity_label = make_label(screen, 132, 202, 86, 22, "湿度 --%");
    s_wind_label = make_label(screen, 228, 202, 152, 22, "-- --级");
    s_sunrise_label = make_label(screen, 20, 224, 110, 20, "日出 --:--");
    s_sunset_label = make_label(screen, 132, 224, 120, 20, "日落 --:--");
    s_alert_label = make_label(screen, 20, 246, 360, 22, "预警 --");
    s_advice_label = make_label(screen, 20, 272, 360, 20, "等待更多天气数据");
    lv_label_set_long_mode(s_advice_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_advice_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
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
        char temp_line[12];
        char today_range[32];
        snprintf(temp_line, sizeof(temp_line), "%s", weather.temp[0] ? weather.temp : "--");
        changed |= set_label_text_if_changed(s_city_label, weather.city[0] ? weather.city : "--");
        changed |= set_label_text_if_changed(s_current_temp_label, temp_line);
        changed |= set_label_text_if_changed(s_current_icon_label, weather_icon_text(weather.icon[0] ? weather.icon : "999"));
        changed |= set_label_text_if_changed(s_current_text_label, weather.text[0] ? weather.text : "--");
        if (forecast.ready && forecast.count > 0 && forecast.days[0].valid) {
            snprintf(today_range, sizeof(today_range), "今日 %s/%sC",
                     forecast.days[0].temp_min[0] ? forecast.days[0].temp_min : "--",
                     forecast.days[0].temp_max[0] ? forecast.days[0].temp_max : "--");
        } else {
            snprintf(today_range, sizeof(today_range), "今日 --/--C");
        }
        changed |= set_label_text_if_changed(s_today_range_label, today_range);
    } else {
        changed |= set_label_text_if_changed(s_city_label, "--");
        changed |= set_label_text_if_changed(s_current_temp_label, "--");
        changed |= set_label_text_if_changed(s_current_icon_label, weather_icon_text("999"));
        changed |= set_label_text_if_changed(s_current_text_label, (bits & kWifiConnectedBit) ? "同步中" : "等待数据");
        changed |= set_label_text_if_changed(s_today_range_label, "今日 --/--C");
    }

    for (int i = 0; i < kWeatherForecastDays; ++i) {
        const WeatherForecastDay *day = (forecast.ready && i < forecast.count) ? &forecast.days[i] : nullptr;
        set_card_visible(s_cards[i], true);
        changed |= update_forecast_card(s_cards[i], day);
    }

    char humi_line[24];
    char air_line[40];
    char wind_line[48];
    char sunrise_line[24];
    char sunset_line[24];
    char alert_line[160];
    const WeatherForecastDay *today = (forecast.ready && forecast.count > 0 && forecast.days[0].valid) ? &forecast.days[0] : nullptr;
    if (air.ready) {
        snprintf(air_line, sizeof(air_line), "AQI %s %s",
                 air.aqi[0] ? air.aqi : "--",
                 air.category[0] ? air.category : "--");
    } else {
        snprintf(air_line, sizeof(air_line), "AQI --");
    }
    snprintf(humi_line, sizeof(humi_line), "湿度 %s%%",
             today && today->humidity[0] ? today->humidity : (weather.humidity[0] ? weather.humidity : "--"));
    snprintf(wind_line, sizeof(wind_line), "%s %s级",
             today && today->wind_dir[0] ? today->wind_dir : "--",
             today && today->wind_scale[0] ? today->wind_scale : "--");
    snprintf(sunrise_line, sizeof(sunrise_line), "日出 %s",
             today && today->sunrise[0] ? today->sunrise : "--:--");
    snprintf(sunset_line, sizeof(sunset_line), "日落 %s",
             today && today->sunset[0] ? today->sunset : "--:--");
    if (alert.active && alert.count > 0 && alert.titles[0][0]) {
        strlcpy(alert_line, "预警 ", sizeof(alert_line));
        for (int i = 0; i < alert.count && i < 3; ++i) {
            if (i > 0) {
                strlcat(alert_line, " / ", sizeof(alert_line));
            }
            strlcat(alert_line, alert.titles[i], sizeof(alert_line));
        }
    } else {
        snprintf(alert_line, sizeof(alert_line), "预警 --");
    }
    changed |= set_label_text_if_changed(s_air_label, air_line);
    changed |= set_label_text_if_changed(s_humidity_label, humi_line);
    changed |= set_label_text_if_changed(s_wind_label, wind_line);
    changed |= set_label_text_if_changed(s_sunrise_label, sunrise_line);
    changed |= set_label_text_if_changed(s_sunset_label, sunset_line);
    changed |= set_label_text_if_changed(s_alert_label, alert_line);
    changed |= set_label_text_if_changed(s_advice_label,
                                         forecast.ready && forecast.advice[0] ? forecast.advice : "等待更多天气数据");
    return changed;
}
