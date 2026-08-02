// 构建并刷新所有工作页复用的五格电池图标。
#include "ui_battery.h"

#include "app_constexpr.h"
#include "app_state.h"

namespace {
#define BATTERY_ICON_INVALID_ARG_LOG "battery icon invalid arg"
#define BATTERY_FRAME_CREATE_FAILED_LOG "battery frame create failed"
#define BATTERY_INNER_CREATE_FAILED_LOG "battery inner create failed"
#define BATTERY_TIP_CREATE_FAILED_LOG "battery tip create failed"
#define BATTERY_SEGMENT_CREATE_FAILED_FORMAT "battery segment %d create failed"

constexpr int kBatteryFrameX = 20;
constexpr int kBatteryFrameY = 17;
constexpr int kBatteryFrameW = 34;
constexpr int kBatteryFrameH = 16;
constexpr int kBatteryInnerX = 2;
constexpr int kBatteryInnerY = 2;
constexpr int kBatteryInnerW = 30;
constexpr int kBatteryInnerH = 12;
constexpr int kBatteryTipX = 55;
constexpr int kBatteryTipY = 22;
constexpr int kBatteryTipW = 3;
constexpr int kBatteryTipH = 6;
constexpr int kBatterySegmentCount = 5;
constexpr int kBatteryPercentPerSegment = 20;
constexpr int kBatterySegmentX = 3;
constexpr int kBatterySegmentY = 4;
constexpr int kBatterySegmentW = 4;
constexpr int kBatterySegmentH = 8;
constexpr int kBatterySegmentGap = 6;
constexpr const char *kBatteryLogTexts[] = {
    BATTERY_ICON_INVALID_ARG_LOG,
    BATTERY_FRAME_CREATE_FAILED_LOG,
    BATTERY_INNER_CREATE_FAILED_LOG,
    BATTERY_TIP_CREATE_FAILED_LOG,
    BATTERY_SEGMENT_CREATE_FAILED_FORMAT,
};
lv_obj_t **const kBatterySegmentsByWorkPage[kWorkPageCount] = {
    g_battery_segments,
    g_gallery_battery_segments,
    g_weather_board_battery_segments,
    g_radio_battery_segments,
    g_calendar_battery_segments,
    g_history_battery_segments,
    g_xiaozhi_battery_segments,
};

static_assert(cstr_array_nonempty(kBatteryLogTexts), "battery log texts must be non-empty");
static_assert(array_count(kBatterySegmentsByWorkPage) == kWorkPageCount,
              "battery segment page map must cover every work page");
static_assert(kBatteryFrameW > 0 && kBatteryFrameH > 0,
              "battery frame size must be positive");
static_assert(kBatteryInnerW > 0 && kBatteryInnerH > 0,
              "battery inner size must be positive");
static_assert(kBatteryTipW > 0 && kBatteryTipH > 0,
              "battery tip size must be positive");
static_assert(kBatterySegmentCount > 0, "battery segment count must be positive");
static_assert(kBatterySegmentW > 0 && kBatterySegmentH > 0,
              "battery segment size must be positive");
static_assert(kBatterySegmentCount * kBatteryPercentPerSegment == 100,
              "battery segments must cover exactly 100 percent");
} // namespace

void style_battery_part(lv_obj_t *obj, bool filled)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_bg_color(obj, filled ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

void style_battery_frame(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

void build_battery_icon(lv_obj_t *parent, lv_obj_t **segments)
{
    if (!parent || !segments) {
        ESP_LOGW(TAG, BATTERY_ICON_INVALID_ARG_LOG);
        return;
    }
    lv_obj_t *frame = lv_obj_create(parent);
    if (!frame) {
        ESP_LOGW(TAG, BATTERY_FRAME_CREATE_FAILED_LOG);
        return;
    }
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(frame, kBatteryFrameX, kBatteryFrameY);
    lv_obj_set_size(frame, kBatteryFrameW, kBatteryFrameH);
    style_battery_frame(frame);

    lv_obj_t *inner = lv_obj_create(frame);
    if (!inner) {
        ESP_LOGW(TAG, BATTERY_INNER_CREATE_FAILED_LOG);
        return;
    }
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(inner, kBatteryInnerX, kBatteryInnerY);
    lv_obj_set_size(inner, kBatteryInnerW, kBatteryInnerH);
    style_battery_part(inner, false);
    lv_obj_set_style_border_width(inner, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(inner, 2, LV_PART_MAIN);

    lv_obj_t *tip = lv_obj_create(parent);
    if (!tip) {
        ESP_LOGW(TAG, BATTERY_TIP_CREATE_FAILED_LOG);
        return;
    }
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tip, kBatteryTipX, kBatteryTipY);
    lv_obj_set_size(tip, kBatteryTipW, kBatteryTipH);
    style_battery_part(tip, true);
    lv_obj_set_style_border_width(tip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 1, LV_PART_MAIN);

    for (int i = 0; i < kBatterySegmentCount; ++i) {
        segments[i] = lv_obj_create(frame);
        if (!segments[i]) {
            ESP_LOGW(TAG, BATTERY_SEGMENT_CREATE_FAILED_FORMAT, i);
            continue;
        }
        lv_obj_clear_flag(segments[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(segments[i], kBatterySegmentX + i * kBatterySegmentGap, kBatterySegmentY);
        lv_obj_set_size(segments[i], kBatterySegmentW, kBatterySegmentH);
        style_battery_part(segments[i], false);
        lv_obj_set_style_border_width(segments[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(segments[i], 1, LV_PART_MAIN);
    }
}

void update_battery_segments(lv_obj_t **segments, int percent, bool charging, bool blink_on)
{
    int filled = 0;
    int blink_index = -1;
    if (percent >= 0) {
        if (percent > 100) {
            percent = 100;
        }
        filled = (percent + kBatteryPercentPerSegment - 1) / kBatteryPercentPerSegment;
        if (charging && percent < kBatteryChargingAnimationStopPercent) {
            blink_index = percent / kBatteryPercentPerSegment;
            if (blink_index >= kBatterySegmentCount) {
                blink_index = kBatterySegmentCount - 1;
            }
        }
    }
    for (int i = 0; i < kBatterySegmentCount; ++i) {
        if (segments[i]) {
            bool active = i < filled;
            if (i == blink_index) {
                active = blink_on;
            }
            style_battery_part(segments[i], active);
            lv_obj_set_style_border_width(segments[i], 0, LV_PART_MAIN);
            lv_obj_set_style_radius(segments[i], 1, LV_PART_MAIN);
        }
    }
}

void update_battery_icon(int percent, bool charging, bool blink_on)
{
    update_battery_segments(g_battery_segments, percent, charging, blink_on);
    update_battery_segments(g_history_battery_segments, percent, charging, blink_on);
    update_battery_segments(g_gallery_battery_segments, percent, charging, blink_on);
    update_battery_segments(g_calendar_battery_segments, percent, charging, blink_on);
}

void update_work_page_battery_icon(int page, int percent, bool charging, bool blink_on)
{
    if (page < 0 || page >= kWorkPageCount) {
        return;
    }
    update_battery_segments(kBatterySegmentsByWorkPage[page], percent, charging, blink_on);
}
