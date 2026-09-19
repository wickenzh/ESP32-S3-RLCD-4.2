// 构建温湿时钟的静态控件、数字牌、传感器面板和日期面板。
#include "ui_flip_clock.h"
#include "ui_flip_clock_objects.h"

#include "app_constexpr.h"
#include "app_metadata.h"
#include "battery_runtime_state.h"
#include "flip_sensor_icons.h"
#include "ui_battery.h"
#include "ui_canvas_primitives.h"
#include "ui_draw_cache.h"
#include "ui_fonts.h"
#include "ui_celsius_marker.h"
#include "ui_inverted_clock_card.h"
#include "ui_page_state.h"
#include "ui_progress.h"
#include "ui_widgets.h"
#include "ui_work_page_layout.h"
#include "ui_work_status.h"
#include "work_page_ids.h"

#include <esp_attr.h>
#include <esp_log.h>

#define FLIP_CLOCK_TEMP_LABEL_CREATE_FAILED_LOG "flip clock temp label create failed"
#define FLIP_CLOCK_HUMIDITY_LABEL_CREATE_FAILED_LOG "flip clock humidity label create failed"
#define FLIP_CLOCK_MOOD_CANVAS_CREATE_FAILED_FORMAT "flip clock %s mood canvas create failed"
#define FLIP_CLOCK_TREND_CANVAS_CREATE_FAILED_FORMAT "flip clock %s trend canvas create failed"

namespace {

static constexpr int kCardCount = inverted_clock_card::kCount;
static constexpr int kCardW = inverted_clock_card::kWidth;
static constexpr int kCardH = inverted_clock_card::kHeight;
static constexpr int kCardY = inverted_clock_card::kY;
static constexpr const int (&kCardX)[kCardCount] = inverted_clock_card::kX;
static constexpr int kHourCardIndex = 0;
static constexpr int kMinuteCardIndex = 1;
static constexpr int kFlipSensorPanelX = kCardX[kHourCardIndex];
static constexpr int kFlipSensorPanelY = 198;
static constexpr int kFlipSensorPanelW = kCardX[kMinuteCardIndex] + kCardW - kFlipSensorPanelX;
static constexpr int kFlipSensorPanelH = 88;
static constexpr int kFlipSensorPanelRadius = 18;
static constexpr int kFlipSensorTempTextY = 204;
static constexpr int kFlipSensorHumiTextY = 243;
static constexpr int kFlipSensorTextH = 36;
static constexpr int kFlipSensorTextPadX = 16;
static constexpr int kFlipSensorTextW = 148;
static constexpr int kFlipSensorBoldOffset = 1;
static constexpr int kFlipSensorBoldYOffset = 1;
static constexpr int kFlipMoodCanvasX = kFlipSensorPanelX + kFlipSensorPanelW - FLIP_SENSOR_ICON_WIDTH - 16;
static constexpr int kFlipTempMoodCanvasY = 204;
static constexpr int kFlipHumiMoodCanvasY = 244;
static constexpr int kFlipTrendCanvasW = 20;
static constexpr int kFlipTrendCanvasH = 20;
static constexpr int kFlipTrendCanvasX = kFlipMoodCanvasX - kFlipTrendCanvasW - 8;
static constexpr int kFlipTempTrendCanvasY = kFlipTempMoodCanvasY + (FLIP_SENSOR_ICON_HEIGHT - kFlipTrendCanvasH) / 2;
static constexpr int kFlipHumiTrendCanvasY = kFlipHumiMoodCanvasY + (FLIP_SENSOR_ICON_HEIGHT - kFlipTrendCanvasH) / 2;
static constexpr int kFlipDatePanelX = 270;
static constexpr int kFlipDatePanelY = kFlipSensorPanelY;
static constexpr int kFlipDatePanelW = kCardW;
static constexpr int kFlipDatePanelH = kFlipSensorPanelH;
static constexpr int kFlipDatePanelRadius = kFlipSensorPanelRadius;
static constexpr int kFlipDayTextY = 196;
static constexpr int kFlipDayTextH = 52;
static constexpr int kFlipLunarTextY = 249;
static constexpr int kFlipLunarTextH = 42;
static constexpr int kFlipDateBoldOffset = 1;
static constexpr int kFlipDateBoldYOffset = 1;
static constexpr const char *kFlipTempPlaceholder = "--.- C";
static constexpr const char *kFlipHumiPlaceholder = "--%";
static constexpr const char *kFlipDayPlaceholder = "--";
EXT_RAM_BSS_ATTR lv_color_t *s_flip_clock_card_canvas_buffer[kCardCount];
lv_color_t *s_flip_clock_temp_mood_canvas_buffer;
lv_color_t *s_flip_clock_humi_mood_canvas_buffer;
lv_color_t *s_flip_clock_temp_trend_canvas_buffer;
lv_color_t *s_flip_clock_humi_trend_canvas_buffer;
EXT_RAM_BSS_ATTR FlipClockObjectRefs s_flip_clock_objects;

static_assert(array_count(kCardX) == kCardCount,
              "flip clock card X table must match card count");
static_assert(kFlipClockObjectCardCount == kCardCount,
              "flip clock object registry must match inverted card count");
static_assert(kFlipSensorPanelX == kCardX[kHourCardIndex] &&
                  kFlipSensorPanelX + kFlipSensorPanelW == kCardX[kMinuteCardIndex] + kCardW &&
                  kFlipDatePanelW == kCardW,
              "flip clock sensor panel must span hour and minute cards");
void style_flip_white_label(lv_obj_t *label, lv_text_align_t align)
{
    if (!label) {
        return;
    }
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
}

lv_obj_t *make_flip_sensor_value_label(lv_obj_t *parent,
                                        int x,
                                        int y,
                                        const char *text,
                                        lv_text_align_t align)
{
    lv_obj_t *label = make_label_with_font(parent,
                                           x,
                                           y,
                                           kFlipSensorTextW,
                                           kFlipSensorTextH,
                                           text,
                                           &lv_font_montserrat_24);
    if (!label) {
        return nullptr;
    }
    style_flip_white_label(label, align);
    ui_enable_celsius_marker(label);
    return label;
}

void build_flip_sensor_value_labels(lv_obj_t *parent,
                                    lv_obj_t **normal_label,
                                    lv_obj_t **bold_x_label,
                                    lv_obj_t **bold_y_label,
                                    int y,
                                    const char *placeholder,
                                    const char *failure_log)
{
    if (!normal_label || !bold_x_label || !bold_y_label) {
        return;
    }
    const int x = kFlipSensorPanelX + kFlipSensorTextPadX;
    *normal_label = make_flip_sensor_value_label(parent,
                                                  x,
                                                  y,
                                                  placeholder,
                                                  LV_TEXT_ALIGN_LEFT);
    *bold_x_label = make_flip_sensor_value_label(parent,
                                                  x + kFlipSensorBoldOffset,
                                                  y,
                                                  placeholder,
                                                  LV_TEXT_ALIGN_LEFT);
    *bold_y_label = make_flip_sensor_value_label(parent,
                                                  x,
                                                  y + kFlipSensorBoldYOffset,
                                                  placeholder,
                                                  LV_TEXT_ALIGN_LEFT);
    if (!*normal_label || !*bold_x_label || !*bold_y_label) {
        ESP_LOGW(TAG, "%s", failure_log ? failure_log : FLIP_CLOCK_TEMP_LABEL_CREATE_FAILED_LOG);
    }
}

lv_obj_t *make_flip_date_label(lv_obj_t *parent,
                               int x,
                               int y,
                               int w,
                               int h,
                               const char *text,
                               const lv_font_t *font)
{
    lv_obj_t *label = make_label_with_font(parent, x, y, w, h, text, font);
    if (!label) {
        return nullptr;
    }
    style_flip_white_label(label, LV_TEXT_ALIGN_CENTER);
    return label;
}

void build_flip_date_label_group(lv_obj_t *parent,
                                 lv_obj_t **normal_label,
                                 lv_obj_t **bold_x_label,
                                 lv_obj_t **bold_y_label,
                                 lv_obj_t **bold_xy_label,
                                 int y,
                                 int height,
                                 const char *text,
                                 const lv_font_t *font)
{
    if (!normal_label || !bold_x_label || !bold_y_label) {
        return;
    }
    *normal_label = make_flip_date_label(parent,
                                         kFlipDatePanelX,
                                         y,
                                         kFlipDatePanelW,
                                         height,
                                         text,
                                         font);
    *bold_x_label = make_flip_date_label(parent,
                                         kFlipDatePanelX + kFlipDateBoldOffset,
                                         y,
                                         kFlipDatePanelW,
                                         height,
                                         text,
                                         font);
    *bold_y_label = make_flip_date_label(parent,
                                         kFlipDatePanelX,
                                         y + kFlipDateBoldYOffset,
                                         kFlipDatePanelW,
                                         height,
                                         text,
                                         font);
    if (bold_xy_label) {
        *bold_xy_label = make_flip_date_label(parent,
                                              kFlipDatePanelX + kFlipDateBoldOffset,
                                              y + kFlipDateBoldYOffset,
                                              kFlipDatePanelW,
                                              height,
                                              text,
                                              font);
    }
}

void build_flip_mood_canvas(lv_obj_t *screen,
                            lv_obj_t **canvas,
                            lv_color_t **buffer,
                            int y,
                            const char *name)
{
    if (!canvas || !buffer) {
        return;
    }
    if (!ensure_canvas_buffer(buffer,
                              FLIP_SENSOR_ICON_WIDTH,
                              FLIP_SENSOR_ICON_HEIGHT)) {
        return;
    }
    *canvas = lv_canvas_create(screen);
    if (!*canvas) {
        ESP_LOGW(TAG, FLIP_CLOCK_MOOD_CANVAS_CREATE_FAILED_FORMAT, name);
        return;
    }
    configure_canvas_base(*canvas,
                          *buffer,
                          kFlipMoodCanvasX,
                          y,
                          FLIP_SENSOR_ICON_WIDTH,
                          FLIP_SENSOR_ICON_HEIGHT);
    lv_canvas_fill_bg(*canvas, lv_color_black(), LV_OPA_COVER);
}

void build_flip_trend_canvas(lv_obj_t *screen,
                             lv_obj_t **canvas,
                             lv_color_t **buffer,
                             int y,
                             const char *name)
{
    if (!canvas || !buffer) {
        return;
    }
    if (!ensure_canvas_buffer(buffer,
                              kFlipTrendCanvasW,
                              kFlipTrendCanvasH)) {
        return;
    }
    *canvas = lv_canvas_create(screen);
    if (!*canvas) {
        ESP_LOGW(TAG, FLIP_CLOCK_TREND_CANVAS_CREATE_FAILED_FORMAT, name);
        return;
    }
    configure_canvas_base(*canvas,
                          *buffer,
                          kFlipTrendCanvasX,
                          y,
                          kFlipTrendCanvasW,
                          kFlipTrendCanvasH);
    lv_canvas_fill_bg(*canvas, lv_color_black(), LV_OPA_COVER);
}

void build_flip_sensor_panel(lv_obj_t *screen, FlipClockObjectRefs &objects)
{
    lv_obj_t *sensor_panel = make_black_bar(screen,
                                            kFlipSensorPanelX,
                                            kFlipSensorPanelY,
                                            kFlipSensorPanelW,
                                            kFlipSensorPanelH);
    if (sensor_panel) {
        lv_obj_set_style_radius(sensor_panel, kFlipSensorPanelRadius, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(sensor_panel, true, LV_PART_MAIN);
    }

    build_flip_sensor_value_labels(screen,
                                   &objects.sensor_label,
                                   &objects.sensor_bold_label,
                                   &objects.sensor_bold_y_label,
                                   kFlipSensorTempTextY,
                                   kFlipTempPlaceholder,
                                   FLIP_CLOCK_TEMP_LABEL_CREATE_FAILED_LOG);
    build_flip_sensor_value_labels(screen,
                                   &objects.humidity_label,
                                   &objects.humidity_bold_label,
                                   &objects.humidity_bold_y_label,
                                   kFlipSensorHumiTextY,
                                   kFlipHumiPlaceholder,
                                   FLIP_CLOCK_HUMIDITY_LABEL_CREATE_FAILED_LOG);
    build_flip_mood_canvas(screen,
                           &objects.temp_mood_canvas,
                           &s_flip_clock_temp_mood_canvas_buffer,
                           kFlipTempMoodCanvasY,
                           "temperature");
    build_flip_mood_canvas(screen,
                           &objects.humi_mood_canvas,
                           &s_flip_clock_humi_mood_canvas_buffer,
                           kFlipHumiMoodCanvasY,
                           "humidity");

    build_flip_trend_canvas(screen,
                            &objects.temp_trend_canvas,
                            &s_flip_clock_temp_trend_canvas_buffer,
                            kFlipTempTrendCanvasY,
                            "temperature");
    build_flip_trend_canvas(screen,
                            &objects.humi_trend_canvas,
                            &s_flip_clock_humi_trend_canvas_buffer,
                            kFlipHumiTrendCanvasY,
                            "humidity");
}

void build_flip_date_panel(lv_obj_t *screen, FlipClockObjectRefs &objects)
{
    lv_obj_t *date_panel = make_black_bar(screen,
                                          kFlipDatePanelX,
                                          kFlipDatePanelY,
                                          kFlipDatePanelW,
                                          kFlipDatePanelH);
    if (date_panel) {
        lv_obj_set_style_radius(date_panel, kFlipDatePanelRadius, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(date_panel, true, LV_PART_MAIN);
    }
    build_flip_date_label_group(screen,
                                &objects.day_label,
                                &objects.day_bold_label,
                                &objects.day_bold_y_label,
                                nullptr,
                                kFlipDayTextY,
                                kFlipDayTextH,
                                kFlipDayPlaceholder,
                                &lv_font_montserrat_48);
    build_flip_date_label_group(screen,
                                &objects.lunar_label,
                                &objects.lunar_bold_x_label,
                                &objects.lunar_bold_y_label,
                                &objects.lunar_bold_xy_label,
                                kFlipLunarTextY,
                                kFlipLunarTextH,
                                kFlipDayPlaceholder,
                                &zh_flip_lunar_22);
}

} // namespace

FlipClockObjectRefs &mutable_flip_clock_object_refs()
{
    return s_flip_clock_objects;
}

const FlipClockObjectRefs &flip_clock_object_refs()
{
    return s_flip_clock_objects;
}

void clear_flip_clock_object_refs()
{
    s_flip_clock_objects = {};
}

void build_flip_clock_page()
{
    if (work_page_root(kWorkPageFlipClock)) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    set_work_page_root(kWorkPageFlipClock, screen);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    FlipClockObjectRefs &objects = mutable_flip_clock_object_refs();

    build_work_page_battery_icon(screen, kWorkPageFlipClock);
    build_work_page_status_bar(screen,
                               kWorkPageFlipClock,
                               false,
                               false);

    make_black_bar(screen,
                   ui_work_page_layout::kTopSeparatorX,
                   ui_work_page_layout::kTopSeparatorY,
                   ui_work_page_layout::kTopSeparatorWidth,
                   ui_work_page_layout::kTopSeparatorHeight);
    build_work_page_day_progress(screen, kWorkPageFlipClock);

    build_inverted_clock_cards(screen,
                               objects.card_canvas,
                               s_flip_clock_card_canvas_buffer);
    build_flip_sensor_panel(screen, objects);
    build_flip_date_panel(screen, objects);

    update_work_page_battery_icon(kWorkPageFlipClock, battery_percent_load());
    invalidate_flip_clock_draw_cache();
}
