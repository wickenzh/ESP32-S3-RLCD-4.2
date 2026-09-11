// 固件与SDL共用聚合时钟布局和差分显示，不持有网络或传感器资源。
#pragma once
#include "lvgl.h"

struct AggregateClockView {
    lv_obj_t *weather_panel = nullptr;
    int weather_kind = -1;
    int texture_read_width = -1;
    lv_obj_t *digits[3] = {};
    int values[3] = {-1, -1, -1};
    lv_obj_t *city = nullptr;
    lv_obj_t *weather = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *temperature = nullptr;
    lv_obj_t *range = nullptr;
    lv_obj_t *day = nullptr;
    lv_obj_t *month = nullptr;
    lv_obj_t *lunar = nullptr;
    lv_obj_t *local_temp = nullptr;
    lv_obj_t *humidity = nullptr;
};

inline constexpr int kAggregateDigitWidth = 104;
inline constexpr int kAggregateDigitHeight = 80;
void aggregate_clock_view_build(lv_obj_t *root, AggregateClockView &view,
                                lv_color_t *const buffers[3]);
bool aggregate_clock_view_time(AggregateClockView &view, int hour, int minute, int second);
bool aggregate_clock_set_text(lv_obj_t *label, const char *text);

bool aggregate_clock_weather_theme(AggregateClockView &view,int weather_kind);
