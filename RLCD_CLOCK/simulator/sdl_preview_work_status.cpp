// 实现 SDL 工作页预览共用的日期、电池和状态图标栏。
#include "sdl_preview_work_status.h"

#include <stdio.h>
#include <utility>

#include "sdl_preview_widgets.h"
#include "ui_icons.h"

namespace sdl_preview_work_status {
void Bar::set_simulated_status(bool wifi, bool chime, bool alarm)
{
    for (auto entry : {std::pair<lv_obj_t *, bool>{wifi_status_icon_canvas_, wifi},
                       {chime_status_icon_canvas_, chime}, {alarm_status_icon_canvas_, alarm}}) {
        if (!entry.first) continue;
        if (entry.second) lv_obj_clear_flag(entry.first, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(entry.first, LV_OBJ_FLAG_HIDDEN);
    }
}
namespace {

constexpr int kTmYearOffset = 1900;
constexpr int kTmMonthOffset = 1;
const char *const kWeekDaysFull[] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
};

const char *weekday_full(int weekday)
{
    if (weekday < 0 || weekday >= static_cast<int>(sizeof(kWeekDaysFull) / sizeof(kWeekDaysFull[0]))) {
        return "--";
    }
    return kWeekDaysFull[weekday];
}

void set_obj_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void style_sensor_summary(lv_obj_t *label)
{
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
}

void style_battery_part(lv_obj_t *obj, bool filled)
{
    lv_obj_set_style_bg_color(obj, filled ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

void style_battery_frame(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

} // namespace

Bar::Bar()
    : chime_status_icon_pixels_(CHIME_STATUS_ICON_WIDTH * CHIME_STATUS_ICON_HEIGHT),
      wifi_status_icon_pixels_(WIFI_STATUS_ICON_WIDTH * WIFI_STATUS_ICON_HEIGHT),
      alarm_status_icon_pixels_(ALARM_STATUS_ICON_WIDTH * ALARM_STATUS_ICON_HEIGHT)
{
}

void Bar::build(lv_obj_t *screen,
                const struct tm &local,
                bool show_time,
                bool show_summary,
                int battery_percent)
{
    date_label_ = sdl_preview_widgets::make_label(screen,
                                                   198,
                                                   15,
                                                   182,
                                                   26,
                                                   "----/--/-- / 星期-");
    lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    build_battery(screen);
    update_battery(battery_percent);
    if (show_summary) {
        lv_obj_t *summary = sdl_preview_widgets::make_label_with_font(
            screen, 210, 36, 98, 18, "25C 46%", &lv_font_montserrat_16);
        style_sensor_summary(summary);
    }
    if (show_time) {
        char time_text[8];
        snprintf(time_text, sizeof(time_text), "%02d:%02d", local.tm_hour, local.tm_min);
        lv_obj_t *time = sdl_preview_widgets::make_label_with_font(
            screen, 318, 36, 60, 18, time_text, &lv_font_montserrat_16);
        lv_obj_set_style_text_align(time, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_set_style_pad_all(time, 0, LV_PART_MAIN);
    }
    build_status_icon(screen,
                      &chime_status_icon_canvas_,
                      chime_status_icon_pixels_.data(),
                      64,
                      15,
                      CHIME_STATUS_ICON_WIDTH,
                      CHIME_STATUS_ICON_HEIGHT,
                      CHIME_STATUS_ICON_BYTES_PER_ROW,
                      chime_status_icon_bits);
    build_status_icon(screen,
                      &wifi_status_icon_canvas_,
                      wifi_status_icon_pixels_.data(),
                      90,
                      15,
                      WIFI_STATUS_ICON_WIDTH,
                      WIFI_STATUS_ICON_HEIGHT,
                      WIFI_STATUS_ICON_BYTES_PER_ROW,
                      wifi_status_icon_bits);
    build_status_icon(screen,
                      &alarm_status_icon_canvas_,
                      alarm_status_icon_pixels_.data(),
                      116,
                      15,
                      ALARM_STATUS_ICON_WIDTH,
                      ALARM_STATUS_ICON_HEIGHT,
                      ALARM_STATUS_ICON_BYTES_PER_ROW,
                      alarm_status_icon_bits);
}

void Bar::update_date(const struct tm &local)
{
    char date[48];
    snprintf(date,
             sizeof(date),
             "%04d/%02d/%02d / %s",
             local.tm_year + kTmYearOffset,
             local.tm_mon + kTmMonthOffset,
             local.tm_mday,
             weekday_full(local.tm_wday));
    sdl_preview_widgets::set_label_text_if_changed(date_label_, date);
}

void Bar::update_battery(int percent)
{
    int filled = 0;
    if (percent >= 0) {
        if (percent > 100) {
            percent = 100;
        }
        filled = (percent + 19) / 20;
    }
    for (int i = 0; i < 5; ++i) {
        if (battery_segments_[i]) {
            style_battery_part(battery_segments_[i], i < filled);
            lv_obj_set_style_border_width(battery_segments_[i], 0, LV_PART_MAIN);
            lv_obj_set_style_radius(battery_segments_[i], 1, LV_PART_MAIN);
        }
    }
}

void Bar::set_status_icons_visible(bool visible)
{
    set_obj_visible(chime_status_icon_canvas_, visible);
    set_obj_visible(wifi_status_icon_canvas_, visible);
    set_obj_visible(alarm_status_icon_canvas_, visible);
}

void Bar::build_battery(lv_obj_t *parent)
{
    lv_obj_t *frame = lv_obj_create(parent);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(frame, 20, 17);
    lv_obj_set_size(frame, 34, 16);
    style_battery_frame(frame);

    lv_obj_t *inner = lv_obj_create(frame);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(inner, 2, 2);
    lv_obj_set_size(inner, 30, 12);
    style_battery_part(inner, false);
    lv_obj_set_style_border_width(inner, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(inner, 2, LV_PART_MAIN);

    lv_obj_t *tip = lv_obj_create(parent);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tip, 55, 22);
    lv_obj_set_size(tip, 3, 6);
    style_battery_part(tip, true);
    lv_obj_set_style_border_width(tip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 1, LV_PART_MAIN);

    for (int i = 0; i < 5; ++i) {
        battery_segments_[i] = lv_obj_create(frame);
        lv_obj_clear_flag(battery_segments_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(battery_segments_[i], 3 + i * 6, 4);
        lv_obj_set_size(battery_segments_[i], 4, 8);
        style_battery_part(battery_segments_[i], false);
        lv_obj_set_style_border_width(battery_segments_[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(battery_segments_[i], 1, LV_PART_MAIN);
    }
}

void Bar::build_status_icon(lv_obj_t *screen,
                            lv_obj_t **canvas,
                            lv_color_t *pixels,
                            int x,
                            int y,
                            int width,
                            int height,
                            int bytes_per_row,
                            const uint8_t *bits)
{
    *canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(*canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(*canvas, x, y);
    lv_obj_set_size(*canvas, width, height);
    lv_obj_set_style_border_width(*canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(*canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(*canvas, pixels, width, height, LV_IMG_CF_TRUE_COLOR);
    sdl_preview_widgets::draw_1bit_icon(*canvas,
                                        width,
                                        height,
                                        bytes_per_row,
                                        bits,
                                        lv_color_black(),
                                        lv_color_white());
}

} // namespace sdl_preview_work_status
