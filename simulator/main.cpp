// 运行天气时钟 LVGL SDL 预览并生成各页面截图。
#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <vector>

#include "lvgl.h"
#include "dseg_digits.h"
#include "boot_anim.h"
#include "status_gif_60.h"
#include "sdl_preview_backend.h"
#include "sdl_preview_calendar.h"
#include "sdl_preview_flip_clock.h"
#include "sdl_preview_gallery.h"
#include "sdl_preview_history.h"
#include "sdl_preview_mode.h"
#include "sdl_preview_settings.h"
#include "sdl_preview_weather.h"
#include "sdl_preview_widgets.h"
#include "sdl_preview_xiaozhi.h"
#include "core/app_constexpr.h"
#include "ui_icons.h"

LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_font_16);

using sdl_preview_widgets::make_bar;
using sdl_preview_widgets::make_label;
using sdl_preview_widgets::make_label_with_font;
using sdl_preview_widgets::draw_1bit_icon;
using sdl_preview_widgets::set_label_text_if_changed;
using sdl_preview_widgets::set_obj_black;

static constexpr int kDisplayWidth = 400;
static constexpr int kDisplayHeight = 300;
static constexpr int kWindowScale = 2;
static const char *APP_VERSION = "v1.5.17";
static const char *const kPreviewWeekDaysFull[] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
};
static constexpr int kPreviewTmYearOffset = 1900;
static constexpr int kPreviewTmMonthOffset = 1;
static constexpr int kTimeCanvasW = 292;
static constexpr int kTimeCanvasH = 92;
static constexpr int kSecondCanvasW = 60;
static constexpr int kSecondCanvasH = 40;
static constexpr int kBootAnimRunFrameMs = 50;

static SdlPreviewBackend g_sdl_preview(kDisplayWidth, kDisplayHeight);

static lv_obj_t *g_date_label;
static lv_obj_t *g_temp_icon_canvas;
static lv_obj_t *g_humi_icon_canvas;
static lv_obj_t *g_temp_label;
static lv_obj_t *g_humi_label;
static lv_obj_t *g_temp_trend_canvas;
static lv_obj_t *g_humi_trend_canvas;
static lv_obj_t *g_weather_city_label;
static lv_obj_t *g_weather_info_label;
static lv_obj_t *g_weather_icon_label;
static lv_obj_t *g_weather_temp_label;
static lv_obj_t *g_weather_humi_label;
static lv_obj_t *g_alert_pill;
static lv_obj_t *g_alert_icon_canvas;
static lv_obj_t *g_alert_label;
static lv_obj_t *g_chime_status_icon_canvas;
static lv_obj_t *g_wifi_status_icon_canvas;
static lv_obj_t *g_alarm_status_icon_canvas;
static lv_obj_t *g_low_battery_icon_canvas;
static lv_obj_t *g_panel_sep_a;
static lv_obj_t *g_panel_sep_b;
static lv_obj_t *g_battery_segments[5];
static lv_obj_t *g_time_canvas;
static lv_obj_t *g_second_canvas;
static lv_obj_t *g_status_gif_canvas;
static lv_obj_t *g_boot_anim_canvas;
static lv_obj_t *g_day_progress_canvas;
static lv_obj_t *g_second_progress_canvas;
static lv_obj_t *g_lower_panel_objects[13];
static lv_obj_t *g_setup_status_labels[6];
static int g_last_day_progress_filled = -1;
static int g_last_second_progress_filled = -1;
static int g_last_status_gif_frame = -1;
static std::vector<lv_color_t> g_time_canvas_pixels(kTimeCanvasW * kTimeCanvasH);
static std::vector<lv_color_t> g_second_canvas_pixels(kSecondCanvasW * kSecondCanvasH);
static std::vector<lv_color_t> g_status_gif_canvas_pixels(STATUS_GIF_WIDTH * STATUS_GIF_HEIGHT);
static std::vector<lv_color_t> g_boot_anim_canvas_pixels(BOOT_ANIM_WIDTH * BOOT_ANIM_HEIGHT);
static std::vector<lv_color_t> g_alert_icon_canvas_pixels(WARNING_ICON_WIDTH * WARNING_ICON_HEIGHT);
static std::vector<lv_color_t> g_chime_status_icon_canvas_pixels(CHIME_STATUS_ICON_WIDTH * CHIME_STATUS_ICON_HEIGHT);
static std::vector<lv_color_t> g_wifi_status_icon_canvas_pixels(WIFI_STATUS_ICON_WIDTH * WIFI_STATUS_ICON_HEIGHT);
static std::vector<lv_color_t> g_alarm_status_icon_canvas_pixels(ALARM_STATUS_ICON_WIDTH * ALARM_STATUS_ICON_HEIGHT);
static std::vector<lv_color_t> g_low_battery_icon_canvas_pixels(LOW_BATTERY_ICON_WIDTH * LOW_BATTERY_ICON_HEIGHT);
static std::vector<lv_color_t> g_temp_trend_canvas_pixels(TREND_ICON_WIDTH * TREND_ICON_HEIGHT);
static std::vector<lv_color_t> g_humi_trend_canvas_pixels(TREND_ICON_WIDTH * TREND_ICON_HEIGHT);
static std::vector<lv_color_t> g_temp_icon_canvas_pixels(TEMP_ICON_WIDTH * TEMP_ICON_HEIGHT);
static std::vector<lv_color_t> g_humi_icon_canvas_pixels(HUMI_ICON_WIDTH * HUMI_ICON_HEIGHT);
static constexpr int kProgressSegmentCount = 60;
static constexpr int kProgressSegmentW = 5;
static constexpr int kProgressSegmentH = 3;
static constexpr int kProgressSegmentGap = 1;
static constexpr int kProgressCanvasW = kProgressSegmentCount * kProgressSegmentW + (kProgressSegmentCount - 1) * kProgressSegmentGap;
static constexpr int kProgressCanvasH = kProgressSegmentH;
static std::vector<lv_color_t> g_day_progress_canvas_pixels(kProgressCanvasW * kProgressCanvasH);
static std::vector<lv_color_t> g_second_progress_canvas_pixels(kProgressCanvasW * kProgressCanvasH);
static std::vector<lv_color_t> g_flip_day_progress_pixels(kProgressCanvasW * kProgressCanvasH);

static void update_time_ui(const struct tm &local);
static time_t preview_time();
static void build_battery_icon(lv_obj_t *parent);
static void update_battery_icon(int percent);

static const char *preview_weekday_full(int weekday)
{
    if (weekday < 0 || weekday >= static_cast<int>(array_count(kPreviewWeekDaysFull))) {
        return "--";
    }
    return kPreviewWeekDaysFull[weekday];
}

static void format_preview_date(char *out, size_t out_len, const struct tm &local)
{
    if (!out || out_len == 0) {
        return;
    }
    snprintf(out,
             out_len,
             "%04d/%02d/%02d / %s",
             local.tm_year + kPreviewTmYearOffset,
             local.tm_mon + kPreviewTmMonthOffset,
             local.tm_mday,
             preview_weekday_full(local.tm_wday));
}

static void draw_progress_segment(lv_obj_t *canvas, int index, bool filled)
{
    if (!canvas || index < 0 || index >= kProgressSegmentCount) return;
    int x0 = index * (kProgressSegmentW + kProgressSegmentGap);
    for (int y = 0; y < kProgressSegmentH; ++y) {
        for (int x = 0; x < kProgressSegmentW; ++x) {
            bool border = x == 0 || x == kProgressSegmentW - 1 || y == 0 || y == kProgressSegmentH - 1;
            lv_canvas_set_px_color(canvas, x0 + x, y, (filled || border) ? lv_color_black() : lv_color_white());
        }
    }
}

static void invalidate_progress_segment(lv_obj_t *canvas, int index)
{
    if (!canvas || index < 0 || index >= kProgressSegmentCount) return;
    int x0 = index * (kProgressSegmentW + kProgressSegmentGap);
    lv_area_t area = {};
    area.x1 = static_cast<lv_coord_t>(x0);
    area.y1 = 0;
    area.x2 = static_cast<lv_coord_t>(x0 + kProgressSegmentW - 1);
    area.y2 = static_cast<lv_coord_t>(kProgressSegmentH - 1);
    lv_obj_invalidate_area(canvas, &area);
}

static void build_progress_canvas(lv_obj_t *parent, lv_obj_t **canvas, std::vector<lv_color_t> &pixels, int y)
{
    *canvas = lv_canvas_create(parent);
    lv_obj_clear_flag(*canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(*canvas, 20, y);
    lv_obj_set_size(*canvas, kProgressCanvasW, kProgressCanvasH);
    lv_obj_set_style_border_width(*canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(*canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(*canvas, pixels.data(), kProgressCanvasW, kProgressCanvasH, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(*canvas, lv_color_white(), LV_OPA_COVER);
    for (int i = 0; i < kProgressSegmentCount; ++i) {
        draw_progress_segment(*canvas, i, false);
    }
    lv_obj_invalidate(*canvas);
}

static void update_progress_canvas(lv_obj_t *canvas, int filled, int *last_filled)
{
    if (!canvas) return;
    if (filled < 0) filled = 0;
    else if (filled > kProgressSegmentCount) filled = kProgressSegmentCount;
    if (*last_filled < 0 || filled < *last_filled) {
        for (int i = 0; i < kProgressSegmentCount; ++i) {
            draw_progress_segment(canvas, i, i < filled);
        }
        lv_obj_invalidate(canvas);
        *last_filled = filled;
        return;
    }
    if (filled == *last_filled) return;
    for (int i = *last_filled; i < filled; ++i) {
        draw_progress_segment(canvas, i, true);
        invalidate_progress_segment(canvas, i);
    }
    *last_filled = filled;
}

static void build_preview_day_progress(lv_obj_t *screen, const struct tm &local)
{
    lv_obj_t *progress = nullptr;
    build_progress_canvas(screen, &progress, g_flip_day_progress_pixels, 59);
    int seconds_of_day = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    int filled = (seconds_of_day * kProgressSegmentCount) / (24 * 3600);
    int last_filled = -1;
    update_progress_canvas(progress, filled, &last_filled);
}

static void update_trend_icon(lv_obj_t *canvas, int trend)
{
    const uint8_t *bits = nullptr;
    if (trend > 0) {
        bits = trend_up_icon_bits;
    } else if (trend < 0) {
        bits = trend_down_icon_bits;
    }
    if (bits) {
        draw_1bit_icon(canvas,
                       TREND_ICON_WIDTH,
                       TREND_ICON_HEIGHT,
                       TREND_ICON_BYTES_PER_ROW,
                       bits,
                       lv_color_black(),
                       lv_color_white());
    } else if (canvas) {
        lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
        lv_obj_invalidate(canvas);
    }
}

static const DsegGlyph *find_dseg_glyph(const DsegFont &font, char ch)
{
    const char *pos = strchr(font.chars, ch);
    if (!pos) return nullptr;
    return &font.glyphs[pos - font.chars];
}

static int draw_dseg_text(lv_obj_t *canvas, const DsegFont &font, const char *text, int cursor_x, int baseline_y)
{
    int x_cursor = cursor_x;
    for (const char *p = text; *p; ++p) {
        const DsegGlyph *glyph = find_dseg_glyph(font, *p);
        if (!glyph) continue;
        uint32_t bit = 0;
        for (int y = 0; y < glyph->height; ++y) {
            for (int x = 0; x < glyph->width; ++x, ++bit) {
                uint8_t byte = font.bitmap[glyph->bitmap_offset + bit / 8];
                if (byte & (0x80 >> (bit & 7))) {
                    lv_canvas_set_px_color(canvas,
                                           x_cursor + glyph->x_offset + x,
                                           baseline_y + glyph->y_offset + y,
                                           lv_color_black());
                }
            }
        }
        x_cursor += glyph->x_advance;
    }
    return x_cursor;
}

static void draw_time_canvas(const struct tm &local)
{
    if (!g_time_canvas) return;
    lv_canvas_fill_bg(g_time_canvas, lv_color_white(), LV_OPA_COVER);
    char hm[6];
    snprintf(hm, sizeof(hm), "%02d:%02d", local.tm_hour, local.tm_min);
    draw_dseg_text(g_time_canvas, kDSEG84Font, hm, 0, 88);
    lv_obj_invalidate(g_time_canvas);
}

static void draw_second_canvas(const struct tm &local)
{
    if (!g_second_canvas) return;
    lv_canvas_fill_bg(g_second_canvas, lv_color_white(), LV_OPA_COVER);
    char ss[3];
    snprintf(ss, sizeof(ss), "%02d", local.tm_sec);
    draw_dseg_text(g_second_canvas, kDSEG36Font, ss, 0, 40);
    lv_obj_invalidate(g_second_canvas);
}

static void draw_status_gif_frame(int frame)
{
    if (!g_status_gif_canvas) return;
    if (frame < 0) {
        frame = 0;
    } else if (frame >= STATUS_GIF_FRAME_COUNT) {
        frame = STATUS_GIF_FRAME_COUNT - 1;
    }
    const uint8_t *pixels = status_gif_frames[frame];
    const uint8_t *prev_pixels = g_last_status_gif_frame >= 0 ? status_gif_frames[g_last_status_gif_frame] : nullptr;
    uint32_t bit = 0;
    bool changed = false;
    for (int y = 0; y < STATUS_GIF_HEIGHT; ++y) {
        for (int x = 0; x < STATUS_GIF_WIDTH; ++x, ++bit) {
            bool black = pixels[bit / 8] & (0x80 >> (bit & 7));
            if (prev_pixels) {
                bool prev_black = prev_pixels[bit / 8] & (0x80 >> (bit & 7));
                if (black == prev_black) {
                    continue;
                }
            }
            lv_canvas_set_px_color(g_status_gif_canvas, x, y, black ? lv_color_black() : lv_color_white());
            changed = true;
        }
    }
    if (changed || g_last_status_gif_frame != frame) {
        lv_obj_invalidate(g_status_gif_canvas);
    }
    g_last_status_gif_frame = frame;
}

static void style_work_page_sensor_summary(lv_obj_t *label)
{
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
}

static void build_preview_status_icon(lv_obj_t *screen,
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
    draw_1bit_icon(*canvas, width, height, bytes_per_row, bits, lv_color_black(), lv_color_white());
}

static void build_preview_work_status_bar(lv_obj_t *screen,
                                          const struct tm &local,
                                          bool show_time = true,
                                          bool show_summary = true)
{
    g_date_label = make_label(screen, 198, 15, 182, 26, "----/--/-- / 星期-");
    lv_obj_set_style_text_align(g_date_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    build_battery_icon(screen);
    update_battery_icon(76);
    if (show_summary) {
        lv_obj_t *summary = make_label_with_font(screen, 210, 36, 98, 18, "25C 46%", &lv_font_montserrat_16);
        style_work_page_sensor_summary(summary);
    }
    if (show_time) {
        char time_text[8];
        snprintf(time_text, sizeof(time_text), "%02d:%02d", local.tm_hour, local.tm_min);
        lv_obj_t *time = make_label_with_font(screen, 318, 36, 60, 18, time_text, &lv_font_montserrat_16);
        lv_obj_set_style_text_align(time, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_set_style_pad_all(time, 0, LV_PART_MAIN);
    }
    build_preview_status_icon(screen,
                              &g_chime_status_icon_canvas,
                              g_chime_status_icon_canvas_pixels.data(),
                              64,
                              15,
                              CHIME_STATUS_ICON_WIDTH,
                              CHIME_STATUS_ICON_HEIGHT,
                              CHIME_STATUS_ICON_BYTES_PER_ROW,
                              chime_status_icon_bits);
    build_preview_status_icon(screen,
                              &g_wifi_status_icon_canvas,
                              g_wifi_status_icon_canvas_pixels.data(),
                              90,
                              15,
                              WIFI_STATUS_ICON_WIDTH,
                              WIFI_STATUS_ICON_HEIGHT,
                              WIFI_STATUS_ICON_BYTES_PER_ROW,
                              wifi_status_icon_bits);
    build_preview_status_icon(screen,
                              &g_alarm_status_icon_canvas,
                              g_alarm_status_icon_canvas_pixels.data(),
                              116,
                              15,
                              ALARM_STATUS_ICON_WIDTH,
                              ALARM_STATUS_ICON_HEIGHT,
                              ALARM_STATUS_ICON_BYTES_PER_ROW,
                              alarm_status_icon_bits);
}

static void remember_lower_panel_object(lv_obj_t *obj)
{
    for (lv_obj_t *&slot : g_lower_panel_objects) {
        if (!slot) {
            slot = obj;
            return;
        }
    }
}

static void set_lower_panel_visible(bool visible)
{
    for (lv_obj_t *obj : g_lower_panel_objects) {
        if (!obj) continue;
        if (visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_setup_panel_visible(bool visible)
{
    for (lv_obj_t *label : g_setup_status_labels) {
        if (!label) continue;
        if (visible) lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_obj_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) return;
    if (visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void draw_boot_anim_frame_index(int frame)
{
    if (!g_boot_anim_canvas) return;
    if (frame < 0) {
        frame = 0;
    } else if (frame >= BOOT_ANIM_FRAME_COUNT) {
        frame = BOOT_ANIM_FRAME_COUNT - 1;
    }
    const uint8_t *pixels = boot_anim_frames[frame];
    uint32_t bit = 0;
    for (int y = 0; y < BOOT_ANIM_HEIGHT; ++y) {
        for (int x = 0; x < BOOT_ANIM_WIDTH; ++x, ++bit) {
            bool black = pixels[bit / 8] & (0x80 >> (bit & 7));
            lv_canvas_set_px_color(g_boot_anim_canvas, x, y, black ? lv_color_black() : lv_color_white());
        }
    }
    lv_obj_invalidate(g_boot_anim_canvas);
}

static void style_battery_part(lv_obj_t *obj, bool filled)
{
    lv_obj_set_style_bg_color(obj, filled ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void style_battery_frame(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void build_battery_icon(lv_obj_t *parent)
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
        g_battery_segments[i] = lv_obj_create(frame);
        lv_obj_clear_flag(g_battery_segments[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(g_battery_segments[i], 3 + i * 6, 4);
        lv_obj_set_size(g_battery_segments[i], 4, 8);
        style_battery_part(g_battery_segments[i], false);
        lv_obj_set_style_border_width(g_battery_segments[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(g_battery_segments[i], 1, LV_PART_MAIN);
    }
}

static void show_boot_screen()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label_with_font(screen, 28, 30, 344, 30, "RLCD Weather Clock", &lv_font_montserrat_16);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *status = make_label_with_font(screen, 28, 64, 344, 24, "Starting...", &lv_font_montserrat_16);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *version = make_label_with_font(screen, 28, 226, 344, 24, APP_VERSION, &lv_font_montserrat_16);
    lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    g_boot_anim_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_boot_anim_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_boot_anim_canvas, 144, 100);
    lv_obj_set_size(g_boot_anim_canvas, BOOT_ANIM_WIDTH, BOOT_ANIM_HEIGHT);
    lv_obj_set_style_border_width(g_boot_anim_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_boot_anim_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_boot_anim_canvas,
                         g_boot_anim_canvas_pixels.data(),
                         BOOT_ANIM_WIDTH,
                         BOOT_ANIM_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(g_boot_anim_canvas, lv_color_white(), LV_OPA_COVER);
    draw_boot_anim_frame_index(0);
}

static void update_battery_icon(int percent)
{
    int filled = 0;
    if (percent >= 0) {
        if (percent > 100) percent = 100;
        filled = (percent + 19) / 20;
    }
    for (int i = 0; i < 5; ++i) {
        if (g_battery_segments[i]) {
            style_battery_part(g_battery_segments[i], i < filled);
            lv_obj_set_style_border_width(g_battery_segments[i], 0, LV_PART_MAIN);
            lv_obj_set_style_radius(g_battery_segments[i], 1, LV_PART_MAIN);
        }
    }
}

static void build_history_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    build_preview_work_status_bar(screen, local);
    lv_obj_t *history_top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(history_top_line, true);
    build_preview_day_progress(screen, local);

    build_history_preview_body(screen, &local);
    update_time_ui(local);
}

static void build_gallery_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    build_preview_work_status_bar(screen, local, false);
    char date_text[48];
    format_preview_date(date_text, sizeof(date_text), local);
    set_label_text_if_changed(g_date_label, date_text);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);
    build_preview_day_progress(screen, local);

    build_gallery_preview_body(screen, &local);

    update_time_ui(local);
}

static void build_calendar_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    build_preview_work_status_bar(screen, local);
    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);
    build_preview_day_progress(screen, local);

    build_calendar_preview_body(screen, &local);
    update_time_ui(local);
}

static void build_weather_board_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    build_preview_work_status_bar(screen, local);
    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);
    build_preview_day_progress(screen, local);

    build_weather_board_preview_body(screen);
    update_time_ui(local);
}

static void build_flip_clock_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    build_preview_work_status_bar(screen, local, false, false);
    char flip_date_text[48];
    format_preview_date(flip_date_text, sizeof(flip_date_text), local);
    set_label_text_if_changed(g_date_label, flip_date_text);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);
    lv_obj_t *day_progress = nullptr;
    build_progress_canvas(screen, &day_progress, g_flip_day_progress_pixels, 59);
    int last_day = -1;
    int seconds_of_day = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    update_progress_canvas(day_progress, (seconds_of_day * 60) / (24 * 3600), &last_day);
    build_flip_clock_preview_body(screen, &local);
}

static void build_xiaozhi_preview_ui(const char *preview_mode)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    XiaozhiPreviewMode mode = classify_xiaozhi_preview_mode(preview_mode);
    build_preview_work_status_bar(screen, local, mode.pomodoro_visible(), true);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);
    lv_obj_t *day_progress = nullptr;
    build_progress_canvas(screen, &day_progress, g_flip_day_progress_pixels, 59);
    int last_day = -1;
    int seconds_of_day = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    update_progress_canvas(day_progress, (seconds_of_day * 60) / (24 * 3600), &last_day);
    build_xiaozhi_preview_body(screen, &local, mode);
}

static void build_info_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label_with_font(screen, 24, 18, 352, 26, "SYSTEM INFO", &lv_font_montserrat_16);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *top_line = make_bar(screen, 24, 50, 352, 3);
    set_obj_black(top_line, true);

    static const char *const info_lines[] = {
        "Last NTP: 2026-07-01 09:30",
        "WiFi: HomeWiFi",
        "Last Weather: 2026-07-01 10:00",
        "Battery: 76%  4.05V",
        "Version: v1.4.40 / 2026-07-01",
        "Source: github.com/wickenzh/ESP32-S3-RLCD-4.2",
    };
    static const int info_y[] = {70, 104, 138, 172, 206, 276};
    for (size_t i = 0; i < sizeof(info_lines) / sizeof(info_lines[0]); ++i) {
        const bool source_line = i == (sizeof(info_lines) / sizeof(info_lines[0])) - 1;
        lv_obj_t *label = make_label_with_font(screen,
                                               source_line ? 0 : 30,
                                               info_y[i],
                                               source_line ? 400 : 340,
                                               source_line ? 18 : 24,
                                               info_lines[i],
                                               source_line ? &lv_font_montserrat_12 : &lv_font_montserrat_14);
        if (source_line) {
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        }
    }

    lv_obj_t *bottom_line = make_bar(screen, 24, 238, 352, 3);
    set_obj_black(bottom_line, true);
    lv_obj_t *return_label = make_label_with_font(screen, 24, 252, 352, 22, "Hold KEY to return", &lv_font_montserrat_14);
    lv_obj_set_style_text_align(return_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void prepare_clock_preview_screen(lv_obj_t *screen)
{
    g_last_status_gif_frame = -1;
    g_last_day_progress_filled = -1;
    g_last_second_progress_filled = -1;
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

static void build_clock_status_header(lv_obj_t *screen)
{
    g_date_label = make_label(screen, 198, 15, 182, 26, "----/--/-- / 星期-");
    lv_obj_set_style_text_align(g_date_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    build_battery_icon(screen);

    g_alert_pill = lv_obj_create(screen);
    lv_obj_clear_flag(g_alert_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_alert_pill, 64, 11);
    lv_obj_set_size(g_alert_pill, 128, 26);
    lv_obj_set_style_bg_color(g_alert_pill, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_alert_pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_alert_pill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_alert_pill, 13, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alert_pill, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_alert_pill, LV_OBJ_FLAG_HIDDEN);

    g_alert_icon_canvas = lv_canvas_create(g_alert_pill);
    lv_obj_clear_flag(g_alert_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_alert_icon_canvas, 4, 4);
    lv_obj_set_size(g_alert_icon_canvas, WARNING_ICON_WIDTH, WARNING_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_alert_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alert_icon_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_alert_icon_canvas,
                         g_alert_icon_canvas_pixels.data(),
                         WARNING_ICON_WIDTH,
                         WARNING_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    draw_1bit_icon(g_alert_icon_canvas,
                   WARNING_ICON_WIDTH,
                   WARNING_ICON_HEIGHT,
                   WARNING_ICON_BYTES_PER_ROW,
                   warning_icon_bits,
                   lv_color_white(),
                   lv_color_black());
    g_alert_label = make_label_with_font(g_alert_pill, 24, 4, 94, 18, "大风蓝色预警", &zh_font_16);
    lv_obj_set_style_text_color(g_alert_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(g_alert_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(g_alert_label, LV_LABEL_LONG_CLIP);

    g_chime_status_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_chime_status_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_chime_status_icon_canvas, 64, 15);
    lv_obj_set_size(g_chime_status_icon_canvas, CHIME_STATUS_ICON_WIDTH, CHIME_STATUS_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_chime_status_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_chime_status_icon_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_chime_status_icon_canvas,
                         g_chime_status_icon_canvas_pixels.data(),
                         CHIME_STATUS_ICON_WIDTH,
                         CHIME_STATUS_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    draw_1bit_icon(g_chime_status_icon_canvas,
                   CHIME_STATUS_ICON_WIDTH,
                   CHIME_STATUS_ICON_HEIGHT,
                   CHIME_STATUS_ICON_BYTES_PER_ROW,
                   chime_status_icon_bits,
                   lv_color_black(),
                   lv_color_white());

    g_wifi_status_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_wifi_status_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_wifi_status_icon_canvas, 90, 15);
    lv_obj_set_size(g_wifi_status_icon_canvas, WIFI_STATUS_ICON_WIDTH, WIFI_STATUS_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_wifi_status_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_wifi_status_icon_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_wifi_status_icon_canvas,
                         g_wifi_status_icon_canvas_pixels.data(),
                         WIFI_STATUS_ICON_WIDTH,
                         WIFI_STATUS_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    draw_1bit_icon(g_wifi_status_icon_canvas,
                   WIFI_STATUS_ICON_WIDTH,
                   WIFI_STATUS_ICON_HEIGHT,
                   WIFI_STATUS_ICON_BYTES_PER_ROW,
                   wifi_status_icon_bits,
                   lv_color_black(),
                   lv_color_white());

    g_alarm_status_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_alarm_status_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_alarm_status_icon_canvas, 116, 15);
    lv_obj_set_size(g_alarm_status_icon_canvas, ALARM_STATUS_ICON_WIDTH, ALARM_STATUS_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_alarm_status_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alarm_status_icon_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_alarm_status_icon_canvas,
                         g_alarm_status_icon_canvas_pixels.data(),
                         ALARM_STATUS_ICON_WIDTH,
                         ALARM_STATUS_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    draw_1bit_icon(g_alarm_status_icon_canvas,
                   ALARM_STATUS_ICON_WIDTH,
                   ALARM_STATUS_ICON_HEIGHT,
                   ALARM_STATUS_ICON_BYTES_PER_ROW,
                   alarm_status_icon_bits,
                   lv_color_black(),
                   lv_color_white());
}

static void build_clock_weather_summary(lv_obj_t *screen)
{
    g_weather_city_label = make_label(screen, 14, 196, 76, 20, "--");
    remember_lower_panel_object(g_weather_city_label);
    lv_obj_set_style_text_align(g_weather_city_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_weather_icon_label = make_label(screen, 91, 194, 34, 38, "");
    remember_lower_panel_object(g_weather_icon_label);
    lv_obj_set_style_text_font(g_weather_icon_label, &qweather_icons_36, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_weather_icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_weather_info_label = make_label(screen, 14, 218, 76, 20, "等待数据");
    remember_lower_panel_object(g_weather_info_label);
    lv_label_set_long_mode(g_weather_info_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(g_weather_info_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_weather_temp_label = make_label(screen, 20, 242, 68, 20, "--℃");
    g_weather_humi_label = make_label(screen, 20, 264, 68, 20, "--%");
    remember_lower_panel_object(g_weather_temp_label);
    remember_lower_panel_object(g_weather_humi_label);
    lv_obj_set_style_text_align(g_weather_temp_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_weather_humi_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void build_clock_sensor_summary(lv_obj_t *screen)
{
    g_temp_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_temp_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_temp_icon_canvas, 153, 215);
    lv_obj_set_size(g_temp_icon_canvas, TEMP_ICON_WIDTH, TEMP_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_temp_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_temp_icon_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_temp_icon_canvas,
                         g_temp_icon_canvas_pixels.data(),
                         TEMP_ICON_WIDTH,
                         TEMP_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    draw_1bit_icon(g_temp_icon_canvas,
                   TEMP_ICON_WIDTH,
                   TEMP_ICON_HEIGHT,
                   TEMP_ICON_BYTES_PER_ROW,
                   temp_icon_bits,
                   lv_color_black(),
                   lv_color_white());
    g_humi_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_humi_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_humi_icon_canvas, 151, 245);
    lv_obj_set_size(g_humi_icon_canvas, HUMI_ICON_WIDTH, HUMI_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_humi_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_humi_icon_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_humi_icon_canvas,
                         g_humi_icon_canvas_pixels.data(),
                         HUMI_ICON_WIDTH,
                         HUMI_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    draw_1bit_icon(g_humi_icon_canvas,
                   HUMI_ICON_WIDTH,
                   HUMI_ICON_HEIGHT,
                   HUMI_ICON_BYTES_PER_ROW,
                   humi_icon_bits,
                   lv_color_black(),
                   lv_color_white());
    g_temp_label = make_label(screen, 174, 214, 62, 28, "--.-℃");
    g_humi_label = make_label(screen, 174, 246, 62, 28, "--.-%");
    remember_lower_panel_object(g_temp_icon_canvas);
    remember_lower_panel_object(g_humi_icon_canvas);
    remember_lower_panel_object(g_temp_label);
    remember_lower_panel_object(g_humi_label);
    lv_obj_set_style_text_align(g_temp_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_humi_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_temp_trend_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_temp_trend_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_temp_trend_canvas, 239, 215);
    lv_obj_set_size(g_temp_trend_canvas, TREND_ICON_WIDTH, TREND_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_temp_trend_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_temp_trend_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_temp_trend_canvas,
                         g_temp_trend_canvas_pixels.data(),
                         TREND_ICON_WIDTH,
                         TREND_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    update_trend_icon(g_temp_trend_canvas, 1);
    g_humi_trend_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_humi_trend_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_humi_trend_canvas, 239, 248);
    lv_obj_set_size(g_humi_trend_canvas, TREND_ICON_WIDTH, TREND_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_humi_trend_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_humi_trend_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_humi_trend_canvas,
                         g_humi_trend_canvas_pixels.data(),
                         TREND_ICON_WIDTH,
                         TREND_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    update_trend_icon(g_humi_trend_canvas, -1);
    remember_lower_panel_object(g_temp_trend_canvas);
    remember_lower_panel_object(g_humi_trend_canvas);
}

static void build_clock_time_and_progress(lv_obj_t *screen)
{
    g_time_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_time_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_time_canvas, 18, 76);
    lv_obj_set_size(g_time_canvas, kTimeCanvasW, kTimeCanvasH);
    lv_obj_set_style_border_width(g_time_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_time_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_time_canvas, g_time_canvas_pixels.data(), kTimeCanvasW, kTimeCanvasH, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(g_time_canvas, lv_color_white(), LV_OPA_COVER);

    g_second_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_second_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_second_canvas, 320, 124);
    lv_obj_set_size(g_second_canvas, kSecondCanvasW, kSecondCanvasH);
    lv_obj_set_style_border_width(g_second_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_second_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_second_canvas, g_second_canvas_pixels.data(), kSecondCanvasW, kSecondCanvasH, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(g_second_canvas, lv_color_white(), LV_OPA_COVER);

    g_status_gif_canvas = lv_canvas_create(screen);
    remember_lower_panel_object(g_status_gif_canvas);
    lv_obj_clear_flag(g_status_gif_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_status_gif_canvas, 279, 196);
    lv_obj_set_size(g_status_gif_canvas, STATUS_GIF_WIDTH, STATUS_GIF_HEIGHT);
    lv_obj_set_style_border_width(g_status_gif_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_status_gif_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(g_status_gif_canvas,
                         g_status_gif_canvas_pixels.data(),
                         STATUS_GIF_WIDTH,
                         STATUS_GIF_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(g_status_gif_canvas, lv_color_white(), LV_OPA_COVER);
    draw_status_gif_frame(0);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    lv_obj_t *bottom_line = make_bar(screen, 18, 184, 364, 4);
    build_progress_canvas(screen, &g_day_progress_canvas, g_day_progress_canvas_pixels, 59);
    build_progress_canvas(screen, &g_second_progress_canvas, g_second_progress_canvas_pixels, 180);
    g_panel_sep_a = make_bar(screen, 139, 188, 2, 102);
    g_panel_sep_b = make_bar(screen, 260, 188, 2, 102);
    remember_lower_panel_object(g_panel_sep_a);
    remember_lower_panel_object(g_panel_sep_b);
    set_obj_black(top_line, true);
    set_obj_black(bottom_line, true);
    set_obj_black(g_panel_sep_a, true);
    set_obj_black(g_panel_sep_b, true);

    g_low_battery_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_low_battery_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_low_battery_icon_canvas, 156, 214);
    lv_obj_set_size(g_low_battery_icon_canvas, LOW_BATTERY_ICON_WIDTH, LOW_BATTERY_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_low_battery_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_low_battery_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_low_battery_icon_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_buffer(g_low_battery_icon_canvas,
                         g_low_battery_icon_canvas_pixels.data(),
                         LOW_BATTERY_ICON_WIDTH,
                         LOW_BATTERY_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    draw_1bit_icon(g_low_battery_icon_canvas,
                   LOW_BATTERY_ICON_WIDTH,
                   LOW_BATTERY_ICON_HEIGHT,
                   LOW_BATTERY_ICON_BYTES_PER_ROW,
                   low_battery_icon_bits,
                   lv_color_black(),
                   lv_color_white());
}

static void build_clock_setup_status(lv_obj_t *screen)
{
    static const int setup_y[] = {194, 212, 230, 248, 266, 284};
    static const char *setup_text[] = {
        "Setup Mode",
        "AP SSID: WeatherClock-ABCD",
        "AP Password: 12345678",
        "Portal IP: 192.168.4.1",
        "STA SSID: HomeWiFi",
        "STA IP: --",
    };
    for (int i = 0; i < 6; ++i) {
        g_setup_status_labels[i] = make_label_with_font(screen, 26, setup_y[i], 348, 18, setup_text[i], &lv_font_montserrat_14);
        lv_obj_add_flag(g_setup_status_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_clock_ui()
{
    lv_obj_t *screen = lv_scr_act();
    prepare_clock_preview_screen(screen);
    build_clock_status_header(screen);
    build_clock_weather_summary(screen);
    build_clock_sensor_summary(screen);
    build_clock_time_and_progress(screen);
    build_clock_setup_status(screen);
}

static void apply_low_battery_preview(bool low)
{
    set_obj_visible(g_second_canvas, !low);
    set_obj_visible(g_day_progress_canvas, !low);
    set_obj_visible(g_second_progress_canvas, !low);
    set_lower_panel_visible(!low);
    set_setup_panel_visible(false);
    set_obj_visible(g_panel_sep_a, true);
    set_obj_visible(g_panel_sep_b, true);
    set_obj_visible(g_low_battery_icon_canvas, low);
    set_obj_visible(g_alert_pill, false);
    set_obj_visible(g_chime_status_icon_canvas, !low);
    set_obj_visible(g_wifi_status_icon_canvas, !low);
    set_obj_visible(g_alarm_status_icon_canvas, !low);
}

static void apply_alert_preview(bool visible)
{
    set_obj_visible(g_alert_pill, visible);
    set_obj_visible(g_chime_status_icon_canvas, !visible);
    set_obj_visible(g_wifi_status_icon_canvas, !visible);
    set_obj_visible(g_alarm_status_icon_canvas, !visible);
    if (visible) {
        set_label_text_if_changed(g_alert_label, "大风蓝色预警");
    }
}

static void update_time_ui(const struct tm &local)
{
    static int last_second = -1;
    static int last_minute = -1;
    int minute_key = local.tm_hour * 60 + local.tm_min;
    if (minute_key != last_minute) {
        last_minute = minute_key;
        draw_time_canvas(local);
        int day_seconds = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
        int day_filled = (day_seconds * 60) / (24 * 3600);
        update_progress_canvas(g_day_progress_canvas, day_filled, &g_last_day_progress_filled);
    }
    if (local.tm_sec != last_second) {
        last_second = local.tm_sec;
        draw_second_canvas(local);
        draw_status_gif_frame(local.tm_sec % STATUS_GIF_FRAME_COUNT);
        update_progress_canvas(g_second_progress_canvas, local.tm_sec + 1, &g_last_second_progress_filled);
    }

    char date[48];
    format_preview_date(date, sizeof(date), local);
    set_label_text_if_changed(g_date_label, date);
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    sdl_preview_backend_flush(&g_sdl_preview, drv, area, color_p);
}

static time_t preview_time()
{
    const char *fixed = getenv("WEATHER_CLOCK_SDL_FIXED_TIME");
    if (fixed && fixed[0]) {
        return (time_t)atoll(fixed);
    }
    return time(nullptr);
}

static void init_lvgl_preview_display()
{
    lv_init();
    static lv_color_t draw_buf_1[kDisplayWidth * 40];
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, draw_buf_1, nullptr, kDisplayWidth * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = kDisplayWidth;
    disp_drv.ver_res = kDisplayHeight;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
}

static void settle_preview_frame()
{
    for (int i = 0; i < 5; ++i) {
        lv_tick_inc(16);
        lv_timer_handler();
        SDL_Delay(16);
    }
}

static bool save_boot_preview_if_requested(const char *screenshot_path, const char *preview_mode)
{
    if (!screenshot_path || !screenshot_path[0] ||
        !sdl_preview_mode::is(preview_mode, "boot")) {
        return false;
    }
    settle_preview_frame();
    sdl_preview_backend_save_ppm(&g_sdl_preview, screenshot_path);
    sdl_preview_backend_cleanup(&g_sdl_preview);
    return true;
}

static void run_boot_animation()
{
    uint32_t boot_start = SDL_GetTicks();
    uint32_t boot_last_tick = boot_start;
    uint32_t boot_last_frame_tick = boot_start;
    int boot_anim_frame = 0;
    while (SDL_GetTicks() - boot_start < 3000) {
        uint32_t now_tick = SDL_GetTicks();
        lv_tick_inc(now_tick - boot_last_tick);
        boot_last_tick = now_tick;
        if (now_tick - boot_last_frame_tick >= kBootAnimRunFrameMs) {
            boot_last_frame_tick = now_tick;
            boot_anim_frame = (boot_anim_frame + 1) % BOOT_ANIM_FRAME_COUNT;
        }
        draw_boot_anim_frame_index(boot_anim_frame);
        lv_timer_handler();
        SDL_Delay(30);
    }
    draw_boot_anim_frame_index(BOOT_ANIM_FRAME_COUNT - 1);
    lv_timer_handler();
    SDL_Delay(100);
    lv_obj_clean(lv_scr_act());
    g_boot_anim_canvas = nullptr;
}

static sdl_preview_mode::Selection build_selected_preview(const char *preview_mode)
{
    sdl_preview_mode::Selection selection =
        sdl_preview_mode::selection_for(preview_mode);
    if (selection.history) {
        build_history_preview_ui();
    } else if (selection.gallery) {
        build_gallery_preview_ui();
    } else if (selection.flip_clock) {
        build_flip_clock_preview_ui();
    } else if (selection.xiaozhi) {
        build_xiaozhi_preview_ui(preview_mode);
    } else if (selection.calendar) {
        build_calendar_preview_ui();
    } else if (selection.weather_board) {
        build_weather_board_preview_ui();
    } else if (selection.info) {
        build_info_preview_ui();
    } else {
        build_clock_ui();
        set_label_text_if_changed(g_temp_label, "24.6℃");
        set_label_text_if_changed(g_humi_label, "58.0%");
        set_label_text_if_changed(g_weather_city_label, "杭州");
        set_label_text_if_changed(g_weather_info_label, "晴");
        set_label_text_if_changed(g_weather_temp_label, "26℃");
        set_label_text_if_changed(g_weather_humi_label, "58%");
        set_label_text_if_changed(g_weather_icon_label, preview_weather_icon_text("100"));
        update_battery_icon(76);
    }
    return selection;
}

static void apply_screenshot_preview_state(const char *preview_mode,
                                           const sdl_preview_mode::Selection &selection)
{
    if (selection.alternate_work_page()) {
            // Alternate work pages are already built above.
    } else if (sdl_preview_mode::is_settings(preview_mode)) {
        build_settings_preview_page(preview_mode);
    } else if (sdl_preview_mode::is(preview_mode, "setup")) {
        set_lower_panel_visible(false);
        set_setup_panel_visible(true);
        set_obj_visible(g_chime_status_icon_canvas, false);
        set_obj_visible(g_wifi_status_icon_canvas, false);
        set_obj_visible(g_alarm_status_icon_canvas, false);
    } else if (sdl_preview_mode::is(preview_mode, "alert")) {
        apply_alert_preview(true);
    } else if (sdl_preview_mode::is(preview_mode, "low")) {
        update_battery_icon(4);
        apply_low_battery_preview(true);
    }

    time_t now = preview_time();
    struct tm local;
    localtime_r(&now, &local);
    if (!selection.alternate_work_page() &&
        !sdl_preview_mode::is_settings(preview_mode)) {
        update_time_ui(local);
        if (sdl_preview_mode::is(preview_mode, "low")) {
            apply_low_battery_preview(true);
        }
    }
}

static bool save_preview_if_requested(const char *screenshot_path,
                                      const char *preview_mode,
                                      const sdl_preview_mode::Selection &selection)
{
    if (!screenshot_path || !screenshot_path[0]) {
        return false;
    }
    apply_screenshot_preview_state(preview_mode, selection);
    settle_preview_frame();
    sdl_preview_backend_save_ppm(&g_sdl_preview, screenshot_path);
    sdl_preview_backend_cleanup(&g_sdl_preview);
    return true;
}

static void run_interactive_preview()
{
    uint32_t last_tick = SDL_GetTicks();
    time_t last_sec = 0;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        uint32_t now_tick = SDL_GetTicks();
        lv_tick_inc(now_tick - last_tick);
        last_tick = now_tick;

        time_t now = preview_time();
        if (now != last_sec) {
            last_sec = now;
            struct tm local = {};
            localtime_r(&now, &local);
            update_time_ui(local);
        }

        lv_timer_handler();
        SDL_Delay(5);
    }
}

int main(int, char **)
{
    if (!sdl_preview_backend_init(&g_sdl_preview,
                                  "WeatherClock LVGL SDL Preview",
                                  kWindowScale)) {
        return 1;
    }

    init_lvgl_preview_display();
    const char *screenshot_path = getenv("WEATHER_CLOCK_SDL_SCREENSHOT");
    const char *preview_mode = getenv("WEATHER_CLOCK_SDL_MODE");

    show_boot_screen();
    if (save_boot_preview_if_requested(screenshot_path, preview_mode)) {
        return 0;
    }
    run_boot_animation();
    sdl_preview_mode::Selection selection = build_selected_preview(preview_mode);
    if (save_preview_if_requested(screenshot_path, preview_mode, selection)) {
        return 0;
    }

    run_interactive_preview();

    sdl_preview_backend_cleanup(&g_sdl_preview);
    return 0;
}
