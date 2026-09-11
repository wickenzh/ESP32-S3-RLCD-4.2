// 绘制第五页天气看板，复用 QWeather 缓存并避免秒级刷新。
#include "ui_work_pages.h"

#include "app_constexpr.h"
#include "app_display_config.h"
#include "app_event_group.h"
#include "app_metadata.h"
#include "qweather_icons.h"
#include "work_page_ids.h"
#include "ui_battery.h"
#include "ui_fonts.h"
#include "ui_page_state.h"
#include "ui_progress.h"
#include "ui_weather_board_sun.h"
#include "ui_weather_board_layout.h"
#include "ui_weather_board_text.h"
#include "ui_widgets.h"
#include "ui_work_page_layout.h"
#include "ui_work_status.h"
#include "weather_state.h"

#include <esp_attr.h>
#include <esp_log.h>

#include <stdint.h>
#include <string.h>

namespace {

using namespace ui_weather_board_layout;

struct WeatherBoardSnapshot {
    WeatherData weather;
    WeatherAlertData alert;
    WeatherForecastData forecast;
    WeatherAirData air;
};

EXT_RAM_BSS_ATTR WeatherBoardSnapshot s_weather_board_snapshot;

static_assert(sizeof(WeatherBoardSnapshot) > 1024,
              "weather-board working snapshot should remain off the UI stack");

struct WeatherBoardRefreshCache {
    uint32_t last_weather_state_version;
    int64_t last_minute_key;
    EventBits_t last_content_state;
    WeatherBoardSunSchedule sun_schedule;
};

EXT_RAM_BSS_ATTR WeatherBoardRefreshCache s_weather_board_refresh_cache;
static_assert(sizeof(WeatherBoardRefreshCache) == 48,
              "weather-board refresh cache must remain compact");

struct WeatherBoardObjectRefs {
    lv_obj_t *city_label = nullptr;
    lv_obj_t *current_icon_label = nullptr;
    lv_obj_t *current_temp_label = nullptr;
    lv_obj_t *current_unit_label = nullptr;
    lv_obj_t *current_text_label = nullptr;
    lv_obj_t *today_range_label = nullptr;
    lv_obj_t *air_label = nullptr;
    lv_obj_t *humidity_label = nullptr;
    lv_obj_t *wind_label = nullptr;
    lv_obj_t *sunrise_label = nullptr;
    lv_obj_t *sunset_label = nullptr;
    lv_obj_t *sun_countdown_label = nullptr;
    lv_obj_t *alert_label = nullptr;
    lv_obj_t *advice_label = nullptr;
};

EXT_RAM_BSS_ATTR WeatherBoardObjectRefs s_weather_board_objects;
static_assert(sizeof(WeatherBoardObjectRefs) == 14 * sizeof(lv_obj_t *),
              "weather-board object references must remain pointer-only");

struct ForecastCardUi {
    lv_obj_t *box = nullptr;
    lv_obj_t *date = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *text = nullptr;
    lv_obj_t *range = nullptr;
};

EXT_RAM_BSS_ATTR ForecastCardUi s_cards[kWeatherForecastDays];
constexpr const char *kWeatherBoardUnknownIcon = "999";
constexpr const char *kWeatherBoardWaitingData = "等待数据";
constexpr const char *kWeatherBoardSyncing = "同步中";
constexpr const char *kWeatherBoardCurrentUnitText = "C";
constexpr size_t kForecastDateLineSize = 24;
constexpr size_t kForecastTempRangeSize = 20;
constexpr size_t kCurrentTempLineSize = 12;
constexpr size_t kTodayRangeLineSize = 32;
constexpr size_t kWeatherBoardHumidityLineSize = 24;
constexpr size_t kWeatherBoardAirLineSize = 40;
constexpr size_t kWeatherBoardWindLineSize = 48;
constexpr size_t kWeatherBoardSunTimeLineSize = 24;
constexpr size_t kWeatherBoardSunCountdownLineSize = 24;
constexpr size_t kWeatherBoardAlertLineSize = 160;

struct WeatherBoardTextWorkspace {
    char forecast_date_line[kForecastDateLineSize];
    char forecast_temp_range[kForecastTempRangeSize];
    char current_temp_line[kCurrentTempLineSize];
    char today_range_line[kTodayRangeLineSize];
    char humidity_line[kWeatherBoardHumidityLineSize];
    char air_line[kWeatherBoardAirLineSize];
    char wind_line[kWeatherBoardWindLineSize];
    char sunrise_line[kWeatherBoardSunTimeLineSize];
    char sunset_line[kWeatherBoardSunTimeLineSize];
    char sun_countdown_line[kWeatherBoardSunCountdownLineSize];
    char alert_line[kWeatherBoardAlertLineSize];
};

EXT_RAM_BSS_ATTR WeatherBoardTextWorkspace s_weather_board_text_workspace;
constexpr size_t kWeatherBoardTextWorkspaceSize =
    kForecastDateLineSize +
    kForecastTempRangeSize +
    kCurrentTempLineSize +
    kTodayRangeLineSize +
    kWeatherBoardHumidityLineSize +
    kWeatherBoardAirLineSize +
    kWeatherBoardWindLineSize +
    (2 * kWeatherBoardSunTimeLineSize) +
    kWeatherBoardSunCountdownLineSize +
    kWeatherBoardAlertLineSize;
static_assert(sizeof(WeatherBoardTextWorkspace) == kWeatherBoardTextWorkspaceSize,
              "weather-board text workspace must remain compact");

constexpr EventBits_t kWeatherBoardContentStateMask =
    kWeatherReadyBit | kWifiConnectedBit;
#define WEATHER_BOARD_FORECAST_CARD_CREATE_FAILED_FORMAT "weather forecast card %d create failed"
static_assert(array_count(kForecastCardX) == kWeatherForecastDays,
              "weather forecast card positions must match forecast day count");
static_assert(array_count(s_cards) == kWeatherForecastDays,
              "weather forecast card storage must match forecast day count");
static_assert(kForecastCardX[kWeatherForecastDays - 1] + kForecastCardW <= kDisplayWidth,
              "weather forecast cards must fit display width");
static_assert(kWeatherBoardCurrentUnitW > 0 && kWeatherBoardCurrentUnitH > 0,
              "weather board current unit label size must be positive");
static_assert(kWeatherBoardCurrentTextW > 0 && kWeatherBoardCurrentTextH > 0,
              "weather board current text label size must be positive");
static_assert(kWeatherBoardTodayRangeW > 0 && kWeatherBoardTodayRangeH > 0,
              "weather board today range label size must be positive");
static_assert((kWeatherBoardContentStateMask & kWeatherReadyBit) != 0,
              "weather board refresh state must include weather readiness");

EventBits_t weather_board_content_state(EventBits_t bits)
{
    if ((bits & kWeatherReadyBit) != 0) {
        return kWeatherReadyBit;
    }
    return bits & kWeatherBoardContentStateMask;
}

void invalidate_weather_board_refresh_cache()
{
    s_weather_board_refresh_cache.last_weather_state_version = UINT32_MAX;
    s_weather_board_refresh_cache.last_minute_key = -1;
    s_weather_board_refresh_cache.last_content_state =
        static_cast<EventBits_t>(~0U);
    s_weather_board_refresh_cache.sun_schedule = {};
}

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

WeatherIconText weather_icon_or_default(const char *icon)
{
    return weather_icon_text(icon && icon[0] ? icon : kWeatherBoardUnknownIcon);
}

bool update_forecast_card(ForecastCardUi &card, const WeatherForecastDay *day)
{
    bool changed = false;
    if (!day || !day->valid) {
        changed |= set_label_text_if_changed(card.date, kWeatherBoardDash);
        changed |= set_label_text_if_changed(
            card.icon,
            weather_icon_text(kWeatherBoardUnknownIcon).c_str());
        changed |= set_label_text_if_changed(card.text, kWeatherBoardDash);
        changed |= set_label_text_if_changed(card.range, kWeatherBoardShortDatePlaceholder);
        return changed;
    }
    auto &date_line = s_weather_board_text_workspace.forecast_date_line;
    auto &temp_range = s_weather_board_text_workspace.forecast_temp_range;
    format_forecast_date_line(*day, date_line, sizeof(date_line));
    format_forecast_temp_range(*day, temp_range, sizeof(temp_range));
    changed |= set_label_text_if_changed(card.date, date_line);
    changed |= set_label_text_if_changed(
        card.icon,
        weather_icon_or_default(day->icon).c_str());
    changed |= set_label_text_if_changed(card.text, text_or_dash(day->text));
    changed |= set_label_text_if_changed(card.range, temp_range);
    return changed;
}

void build_forecast_card(lv_obj_t *screen,
                         ForecastCardUi &card,
                         int index)
{
    if (!screen || index < 0 || index >= kWeatherForecastDays) {
        return;
    }
    int x = kForecastCardX[index];
    int y = kForecastCardY;
    card.box = lv_obj_create(screen);
    if (!card.box) {
        ESP_LOGW(TAG, WEATHER_BOARD_FORECAST_CARD_CREATE_FAILED_FORMAT, index);
    } else {
        lv_obj_set_pos(card.box, x, y);
        lv_obj_set_size(card.box, kForecastCardW, kForecastCardH);
        style_weather_card(card.box);
    }

    card.date = make_label(screen,
                           x,
                           y,
                           kForecastCardW,
                           kForecastCardDateH,
                           kWeatherBoardDash);
    set_weather_label_long_mode(card.date, LV_LABEL_LONG_WRAP);
    set_weather_label_align(card.date, LV_TEXT_ALIGN_CENTER);
    card.icon = make_label(screen,
                           x,
                           y + kForecastCardIconY,
                           kForecastCardW,
                           kForecastCardIconH,
                           weather_icon_text(kWeatherBoardUnknownIcon).c_str());
    set_weather_label_font(card.icon, &qweather_icons_36);
    set_weather_label_align(card.icon, LV_TEXT_ALIGN_CENTER);
    card.text = make_label(screen,
                           x,
                           y + kForecastCardTextY,
                           kForecastCardW,
                           kForecastCardTextH,
                           kWeatherBoardDash);
    set_weather_label_long_mode(card.text, LV_LABEL_LONG_WRAP);
    set_weather_label_align(card.text, LV_TEXT_ALIGN_CENTER);
    card.range = make_label(screen,
                            x,
                            y + kForecastCardRangeY,
                            kForecastCardW,
                            kForecastCardRangeH,
                            kWeatherBoardShortDatePlaceholder);
    set_weather_label_align(card.range, LV_TEXT_ALIGN_CENTER);
    set_weather_label_font(card.range, &lv_font_montserrat_12);
}

void build_current_weather_panel(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }
    WeatherBoardObjectRefs &objects = s_weather_board_objects;
    make_black_bar(screen,
                   ui_work_page_layout::kTopSeparatorX,
                   ui_work_page_layout::kTopSeparatorY,
                   ui_work_page_layout::kTopSeparatorWidth,
                   ui_work_page_layout::kTopSeparatorHeight);

    objects.city_label = make_label(screen,
                                    kWeatherBoardCurrentCityX,
                                    kWeatherBoardCurrentCityY,
                                    kWeatherBoardCurrentCityW,
                                    kWeatherBoardCurrentCityH,
                                    kWeatherBoardWaitingData);
    set_weather_label_align(objects.city_label, LV_TEXT_ALIGN_LEFT);

    objects.current_temp_label = make_label_with_font(screen,
                                                      kWeatherBoardCurrentTempX,
                                                      kWeatherBoardCurrentTempY,
                                                      kWeatherBoardCurrentTempW,
                                                      kWeatherBoardCurrentTempH,
                                                      kWeatherBoardDash,
                                                      &lv_font_montserrat_48);
    set_weather_label_align(objects.current_temp_label, LV_TEXT_ALIGN_LEFT);
    objects.current_unit_label = make_label_with_font(screen,
                                                      kWeatherBoardCurrentUnitX,
                                                      kWeatherBoardCurrentUnitY,
                                                      kWeatherBoardCurrentUnitW,
                                                      kWeatherBoardCurrentUnitH,
                                                      kWeatherBoardCurrentUnitText,
                                                      &lv_font_montserrat_24);
    set_weather_label_align(objects.current_unit_label, LV_TEXT_ALIGN_LEFT);

    objects.current_icon_label = make_label(screen,
                                            kWeatherBoardCurrentIconX,
                                            kWeatherBoardCurrentIconY,
                                            kWeatherBoardCurrentIconW,
                                            kWeatherBoardCurrentIconH,
                                            weather_icon_text(kWeatherBoardUnknownIcon).c_str());
    set_weather_label_font(objects.current_icon_label, &qweather_icons_36);
    set_weather_label_align(objects.current_icon_label, LV_TEXT_ALIGN_CENTER);
    objects.current_text_label = make_label(screen,
                                            kWeatherBoardCurrentTextX,
                                            kWeatherBoardCurrentTextY,
                                            kWeatherBoardCurrentTextW,
                                            kWeatherBoardCurrentTextH,
                                            kWeatherBoardDash);
    objects.today_range_label = make_label(screen,
                                           kWeatherBoardTodayRangeX,
                                           kWeatherBoardTodayRangeY,
                                           kWeatherBoardTodayRangeW,
                                           kWeatherBoardTodayRangeH,
                                           kWeatherBoardTodayRangePlaceholder);
}

void build_weather_detail_panel(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }
    WeatherBoardObjectRefs &objects = s_weather_board_objects;
    make_black_bar(screen,
                   kWeatherBoardDetailLineX,
                   kWeatherBoardDetailLineY,
                   kWeatherBoardDetailLineW,
                   kWeatherBoardDetailLineH);
    objects.air_label = make_label(screen,
                                   kWeatherBoardLeftColumnX,
                                   kWeatherBoardDetailTopY,
                                   kWeatherBoardAirLabelW,
                                   kWeatherBoardDetailLabelH,
                                   kWeatherBoardAirPlaceholder);
    objects.humidity_label = make_label(screen,
                                        kWeatherBoardMiddleColumnX,
                                        kWeatherBoardDetailTopY,
                                        kWeatherBoardHumidityLabelW,
                                        kWeatherBoardDetailLabelH,
                                        kWeatherBoardHumidityPlaceholder);
    objects.wind_label = make_label(screen,
                                    kWeatherBoardRightColumnX,
                                    kWeatherBoardDetailTopY,
                                    kWeatherBoardWindLabelW,
                                    kWeatherBoardDetailLabelH,
                                    kWeatherBoardWindPlaceholder);
    objects.sunrise_label = make_label(screen,
                                       kWeatherBoardLeftColumnX,
                                       kWeatherBoardDetailBottomY,
                                       kWeatherBoardSunriseLabelW,
                                       kWeatherBoardSunLabelH,
                                       kWeatherBoardSunrisePlaceholder);
    objects.sunset_label = make_label(screen,
                                      kWeatherBoardMiddleColumnX,
                                      kWeatherBoardDetailBottomY,
                                      kWeatherBoardSunsetLabelW,
                                      kWeatherBoardSunLabelH,
                                      kWeatherBoardSunsetPlaceholder);
    objects.sun_countdown_label = make_label(screen,
                                             kWeatherBoardRightColumnX,
                                             kWeatherBoardDetailBottomY,
                                             kWeatherBoardSunCountdownLabelW,
                                             kWeatherBoardSunLabelH,
                                             kWeatherBoardSunCountdownPlaceholder);
    objects.alert_label = make_label(screen,
                                     kWeatherBoardAlertX,
                                     kWeatherBoardAlertY,
                                     kWeatherBoardAlertW,
                                     kWeatherBoardAlertH,
                                     kWeatherBoardAlertPlaceholder);
    objects.advice_label = make_label(screen,
                                      kWeatherBoardAdviceX,
                                      kWeatherBoardAdviceY,
                                      kWeatherBoardAdviceW,
                                      kWeatherBoardAdviceH,
                                      kWeatherBoardAdvicePlaceholder);
    set_weather_label_long_mode(objects.advice_label, LV_LABEL_LONG_WRAP);
    set_weather_label_align(objects.advice_label, LV_TEXT_ALIGN_LEFT);
}

bool update_current_weather_panel(const WeatherData &weather,
                                  const WeatherForecastData &forecast,
                                  EventBits_t bits)
{
    WeatherBoardObjectRefs &objects = s_weather_board_objects;
    bool changed = false;
    if ((bits & kWeatherReadyBit) != 0) {
        auto &temp_line = s_weather_board_text_workspace.current_temp_line;
        auto &today_range = s_weather_board_text_workspace.today_range_line;
        strlcpy(temp_line, text_or_dash(weather.temp), sizeof(temp_line));
        changed |= set_label_text_if_changed(objects.city_label,
                                             text_or_dash(weather.city));
        changed |= set_label_text_if_changed(objects.current_temp_label, temp_line);
        changed |= set_label_text_if_changed(objects.current_icon_label,
                                             weather_icon_or_default(weather.icon).c_str());
        changed |= set_label_text_if_changed(objects.current_text_label,
                                             text_or_dash(weather.text));
        if (forecast.ready && forecast.count > 0 && forecast.days[0].valid) {
            format_today_range(forecast.days[0], today_range, sizeof(today_range));
        } else {
            strlcpy(today_range, kWeatherBoardTodayRangePlaceholder, sizeof(today_range));
        }
        changed |= set_label_text_if_changed(objects.today_range_label, today_range);
        return changed;
    }

    changed |= set_label_text_if_changed(objects.city_label, kWeatherBoardDash);
    changed |= set_label_text_if_changed(objects.current_temp_label,
                                         kWeatherBoardDash);
    changed |= set_label_text_if_changed(
        objects.current_icon_label,
        weather_icon_text(kWeatherBoardUnknownIcon).c_str());
    changed |= set_label_text_if_changed(objects.current_text_label,
                                         (bits & kWifiConnectedBit) ? kWeatherBoardSyncing : kWeatherBoardWaitingData);
    changed |= set_label_text_if_changed(objects.today_range_label,
                                         kWeatherBoardTodayRangePlaceholder);
    return changed;
}

bool update_forecast_cards(const WeatherForecastData &forecast)
{
    bool changed = false;
    for (int i = 0; i < kWeatherForecastDays; ++i) {
        const WeatherForecastDay *day = weather_board_forecast_day_or_null(forecast, i);
        changed |= update_forecast_card(s_cards[i], day);
    }
    return changed;
}

bool update_weather_detail_panel(const struct tm &local,
                                 const WeatherData &weather,
                                 const WeatherAlertData &alert,
                                 const WeatherForecastData &forecast,
                                 const WeatherAirData &air,
                                 const WeatherBoardSunSchedule &sun_schedule)
{
    WeatherBoardObjectRefs &objects = s_weather_board_objects;
    auto &humi_line = s_weather_board_text_workspace.humidity_line;
    auto &air_line = s_weather_board_text_workspace.air_line;
    auto &wind_line = s_weather_board_text_workspace.wind_line;
    auto &sunrise_line = s_weather_board_text_workspace.sunrise_line;
    auto &sunset_line = s_weather_board_text_workspace.sunset_line;
    auto &sun_countdown_line = s_weather_board_text_workspace.sun_countdown_line;
    auto &alert_line = s_weather_board_text_workspace.alert_line;
    const WeatherForecastDay *today = weather_board_forecast_day_or_null(forecast, 0);
    format_weather_board_air_line(air, air_line, sizeof(air_line));
    format_weather_board_humidity_line(weather, today, humi_line, sizeof(humi_line));
    format_weather_board_wind_line(today, wind_line, sizeof(wind_line));
    format_weather_board_sunrise_line(today, sunrise_line, sizeof(sunrise_line));
    format_weather_board_sunset_line(today, sunset_line, sizeof(sunset_line));
    format_weather_board_sun_countdown(local,
                                       sun_schedule,
                                       sun_countdown_line,
                                       sizeof(sun_countdown_line));
    format_weather_board_alert_line(alert, alert_line, sizeof(alert_line));

    bool changed = set_label_text_if_changed(objects.air_label, air_line);
    changed |= set_label_text_if_changed(objects.humidity_label, humi_line);
    changed |= set_label_text_if_changed(objects.wind_label, wind_line);
    changed |= set_label_text_if_changed(objects.sunrise_label, sunrise_line);
    changed |= set_label_text_if_changed(objects.sunset_label, sunset_line);
    changed |= set_label_text_if_changed(objects.sun_countdown_label,
                                         sun_countdown_line);
    changed |= set_label_text_if_changed(objects.alert_label, alert_line);
    changed |= set_label_text_if_changed(objects.advice_label,
                                         weather_board_advice_text(forecast,local));
    return changed;
}

bool update_weather_board_sun_countdown(const struct tm &local)
{
    WeatherBoardObjectRefs &objects = s_weather_board_objects;
    auto &sun_countdown_line = s_weather_board_text_workspace.sun_countdown_line;
    sun_countdown_line[0] = '\0';
    format_weather_board_sun_countdown(local,
                                       s_weather_board_refresh_cache.sun_schedule,
                                       sun_countdown_line,
                                       sizeof(sun_countdown_line));
    return set_label_text_if_changed(objects.sun_countdown_label,
                                     sun_countdown_line);
}

bool update_weather_board_full_content(const struct tm &local,
                                       EventBits_t bits,
                                       bool *full_snapshot_loaded)
{
    if (full_snapshot_loaded) {
        *full_snapshot_loaded = false;
    }
    if (!get_weather_full_snapshot(&s_weather_board_snapshot.weather,
                                   &s_weather_board_snapshot.alert,
                                   &s_weather_board_snapshot.forecast,
                                   &s_weather_board_snapshot.air)) {
        return false;
    }
    if (full_snapshot_loaded) {
        *full_snapshot_loaded = true;
    }
    memset(&s_weather_board_text_workspace, 0, sizeof(s_weather_board_text_workspace));
    const WeatherData &weather = s_weather_board_snapshot.weather;
    const WeatherAlertData &alert = s_weather_board_snapshot.alert;
    const WeatherForecastData &forecast = s_weather_board_snapshot.forecast;
    const WeatherAirData &air = s_weather_board_snapshot.air;
    s_weather_board_refresh_cache.sun_schedule =
        weather_board_sun_schedule(forecast);

    bool changed = update_current_weather_panel(weather, forecast, bits);
    changed |= update_forecast_cards(forecast);
    changed |= update_weather_detail_panel(local,
                                           weather,
                                           alert,
                                           forecast,
                                           air,
                                           s_weather_board_refresh_cache
                                               .sun_schedule);
    return changed;
}

} // namespace

void build_weather_board_page()
{
    if (work_page_root(kWorkPageWeatherBoard)) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    set_work_page_root(kWorkPageWeatherBoard, screen);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    build_work_page_battery_icon(screen, kWorkPageWeatherBoard);
    build_work_page_status_bar(screen,
                               kWorkPageWeatherBoard,
                               true,
                               true);

    build_current_weather_panel(screen);
    build_work_page_day_progress(screen, kWorkPageWeatherBoard);

    for (int i = 0; i < kWeatherForecastDays; ++i) {
        build_forecast_card(screen, s_cards[i], i);
    }

    build_weather_detail_panel(screen);
    invalidate_weather_board_refresh_cache();
}

bool update_weather_board_page(const struct tm &local)
{
    build_weather_board_page();
    if (!work_page_root(kWorkPageWeatherBoard)) {
        return false;
    }

    const uint32_t weather_version = weather_state_version_load();
    const EventBits_t bits = app_event_group_get_bits();
    const EventBits_t content_state = weather_board_content_state(bits);
    const int64_t minute_key = weather_board_minute_key(local);
    const bool full_refresh_due =
        weather_version !=
            s_weather_board_refresh_cache.last_weather_state_version ||
        content_state !=
            s_weather_board_refresh_cache.last_content_state;
    const bool minute_refresh_due =
        minute_key != s_weather_board_refresh_cache.last_minute_key;
    if (!full_refresh_due && !minute_refresh_due) {
        return false;
    }

    bool changed = update_work_page_status_time(kWorkPageWeatherBoard, local);
    bool full_snapshot_loaded = !full_refresh_due;
    if (full_refresh_due) {
        changed |= update_weather_board_full_content(local,
                                                     bits,
                                                     &full_snapshot_loaded);
    }
    if (minute_refresh_due && (!full_refresh_due || !full_snapshot_loaded)) {
        changed |= update_weather_board_sun_countdown(local);
        changed |= set_label_text_if_changed(s_weather_board_objects.advice_label,
            weather_board_advice_text(s_weather_board_snapshot.forecast,local));
    }
    if (full_refresh_due && full_snapshot_loaded) {
        s_weather_board_refresh_cache.last_weather_state_version =
            weather_version;
        s_weather_board_refresh_cache.last_content_state = content_state;
    }
    if (minute_refresh_due) {
        s_weather_board_refresh_cache.last_minute_key = minute_key;
    }
    return changed;
}
