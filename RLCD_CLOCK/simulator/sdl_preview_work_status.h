// 管理 SDL 工作页预览共用的日期、电池和状态图标栏。
#pragma once

#include <time.h>

#include <vector>

#include "lvgl.h"

namespace sdl_preview_work_status {

class Bar {
public:
    Bar();

    void build(lv_obj_t *screen,
               const struct tm &local,
               bool show_time = true,
               bool show_summary = true,
               int battery_percent = 76);
    void update_date(const struct tm &local);
    void update_battery(int percent);
    void set_status_icons_visible(bool visible);
    void set_simulated_status(bool wifi, bool chime, bool alarm);

private:
    void build_battery(lv_obj_t *parent);
    void build_status_icon(lv_obj_t *screen,
                           lv_obj_t **canvas,
                           lv_color_t *pixels,
                           int x,
                           int y,
                           int width,
                           int height,
                           int bytes_per_row,
                           const uint8_t *bits);

    lv_obj_t *date_label_ = nullptr;
    lv_obj_t *battery_segments_[5] = {};
    lv_obj_t *chime_status_icon_canvas_ = nullptr;
    lv_obj_t *wifi_status_icon_canvas_ = nullptr;
    lv_obj_t *alarm_status_icon_canvas_ = nullptr;
    std::vector<lv_color_t> chime_status_icon_pixels_;
    std::vector<lv_color_t> wifi_status_icon_pixels_;
    std::vector<lv_color_t> alarm_status_icon_pixels_;
};

} // namespace sdl_preview_work_status
