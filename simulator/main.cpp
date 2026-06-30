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
#include "clock_gallery_images.h"
#include "status_gif_60.h"
#include "ui_icons.h"

LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_font_16);

static constexpr int kDisplayWidth = 400;
static constexpr int kDisplayHeight = 300;
static constexpr int kWindowScale = 2;
static const char *APP_VERSION = "v1.4.39";
static constexpr int kTimeCanvasW = 292;
static constexpr int kTimeCanvasH = 92;
static constexpr int kSecondCanvasW = 60;
static constexpr int kSecondCanvasH = 40;
static constexpr int kBootAnimRunFrameMs = 50;

static SDL_Window *g_window = nullptr;
static SDL_Renderer *g_renderer = nullptr;
static SDL_Texture *g_texture = nullptr;
static std::vector<uint32_t> g_framebuffer(kDisplayWidth * kDisplayHeight, 0xFFFFFFFF);

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
static lv_obj_t *g_settings_labels[5];
static lv_obj_t *g_settings_feedback_label;
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
static constexpr int kHistoryCanvasW = 364;
static constexpr int kHistoryCanvasH = 190;
static std::vector<lv_color_t> g_history_chart_canvas_pixels(kHistoryCanvasW * kHistoryCanvasH);
static constexpr int kGalleryTimeCanvasW = 96;
static constexpr int kGalleryTimeCanvasH = 126;
static std::vector<lv_color_t> g_gallery_image_canvas_pixels(CLOCK_GALLERY_IMAGE_WIDTH * CLOCK_GALLERY_IMAGE_HEIGHT);
static std::vector<lv_color_t> g_gallery_time_canvas_pixels(112 * 208);
static constexpr int kCalendarCanvasW = 364;
static constexpr int kCalendarCanvasH = 228;
static std::vector<lv_color_t> g_calendar_canvas_pixels(kCalendarCanvasW * kCalendarCanvasH);
static constexpr int kFlipCardW = 112;
static constexpr int kFlipCardH = 112;
static constexpr int kFlipCardRadius = 8;
static std::vector<lv_color_t> g_flip_card_pixels[3] = {
    std::vector<lv_color_t>(kFlipCardW * kFlipCardH),
    std::vector<lv_color_t>(kFlipCardW * kFlipCardH),
    std::vector<lv_color_t>(kFlipCardW * kFlipCardH),
};
static std::vector<lv_color_t> g_flip_day_progress_pixels(kProgressCanvasW * kProgressCanvasH);
static std::vector<lv_color_t> g_flip_second_progress_pixels(kProgressCanvasW * kProgressCanvasH);

struct PreviewHistorySample {
    float temp;
    float humi;
};

static void update_time_ui(const struct tm &local);
static time_t preview_time();
static const char *weather_icon_text(const char *code);
static void build_battery_icon(lv_obj_t *parent);
static void update_battery_icon(int percent);

static void set_obj_black(lv_obj_t *obj, bool active)
{
    lv_obj_set_style_bg_color(obj, active ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN);
}

static lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    set_obj_black(bar, false);
    return bar;
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

static void draw_1bit_icon(lv_obj_t *canvas,
                           int width,
                           int height,
                           int bytes_per_row,
                           const uint8_t *bits,
                           lv_color_t fg,
                           lv_color_t bg)
{
    if (!canvas || !bits) return;
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = bits + y * bytes_per_row;
        for (int x = 0; x < width; ++x) {
            if (row[x / 8] & (0x80 >> (x & 7))) {
                lv_canvas_set_px_color(canvas, x, y, fg);
            }
        }
    }
    lv_obj_invalidate(canvas);
}

static const char *const kBlockDigits[10][7] = {
    {"11111", "10001", "10011", "10101", "11001", "10001", "11111"},
    {"00100", "01100", "00100", "00100", "00100", "00100", "01110"},
    {"11110", "00001", "00001", "11110", "10000", "10000", "11111"},
    {"11110", "00001", "00001", "01110", "00001", "00001", "11110"},
    {"10010", "10010", "10010", "11111", "00010", "00010", "00010"},
    {"11111", "10000", "10000", "11110", "00001", "00001", "11110"},
    {"01111", "10000", "10000", "11110", "10001", "10001", "01110"},
    {"11111", "00001", "00010", "00100", "01000", "01000", "01000"},
    {"01110", "10001", "10001", "01110", "10001", "10001", "01110"},
    {"01110", "10001", "10001", "01111", "00001", "00001", "11110"},
};

static void canvas_fill_rect(lv_obj_t *canvas, int x, int y, int w, int h, lv_color_t color)
{
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            lv_canvas_set_px_color(canvas, xx, yy, color);
        }
    }
}

static void apply_preview_card_rounding(lv_obj_t *canvas)
{
    int radius = kFlipCardRadius;
    int r2 = radius * radius;
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            int dx = radius - 1 - x;
            int dy = radius - 1 - y;
            if (dx * dx + dy * dy > r2) {
                lv_canvas_set_px_color(canvas, x, y, lv_color_white());
                lv_canvas_set_px_color(canvas, kFlipCardW - 1 - x, y, lv_color_white());
                lv_canvas_set_px_color(canvas, x, kFlipCardH - 1 - y, lv_color_white());
                lv_canvas_set_px_color(canvas, kFlipCardW - 1 - x, kFlipCardH - 1 - y, lv_color_white());
            }
        }
    }
}

static void canvas_dot_rect(lv_obj_t *canvas, int x, int y, int w, int h)
{
    for (int yy = y; yy < y + h; yy += 3) {
        for (int xx = x; xx < x + w; xx += 4) {
            lv_canvas_set_px_color(canvas, xx, yy, lv_color_black());
        }
    }
    int right = x + w - 1;
    int bottom = y + h - 1;
    for (int yy = y; yy <= bottom; yy += 3) {
        lv_canvas_set_px_color(canvas, right, yy, lv_color_black());
    }
    for (int xx = x; xx <= right; xx += 4) {
        lv_canvas_set_px_color(canvas, xx, bottom, lv_color_black());
    }
    lv_canvas_set_px_color(canvas, right, bottom, lv_color_black());
}

static void canvas_fill_round_rect(lv_obj_t *canvas, int x, int y, int w, int h, int radius, lv_color_t color)
{
    int r2 = radius * radius;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            int dx = 0;
            if (xx < radius) {
                dx = radius - xx;
            } else if (xx >= w - radius) {
                dx = xx - (w - radius - 1);
            }
            int dy = 0;
            if (yy < radius) {
                dy = radius - yy;
            } else if (yy >= h - radius) {
                dy = yy - (h - radius - 1);
            }
            if (dx == 0 || dy == 0 || dx * dx + dy * dy <= r2) {
                lv_canvas_set_px_color(canvas, x + xx, y + yy, color);
            }
        }
    }
}

static void draw_block_digit(lv_obj_t *canvas, int digit, int x, int y, int scale)
{
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (kBlockDigits[digit][row][col] == '1') {
                canvas_fill_rect(canvas, x + col * scale, y + row * scale, scale - 1, scale - 1, lv_color_black());
            }
        }
    }
}

static void draw_block_number(lv_obj_t *canvas, int value, int y)
{
    constexpr int scale = 10;
    constexpr int digit_w = 5 * scale;
    constexpr int gap = 8;
    constexpr int total_w = digit_w * 2 + gap;
    int x = (112 - total_w) / 2;
    draw_block_digit(canvas, value / 10, x, y, scale);
    draw_block_digit(canvas, value % 10, x + digit_w + gap, y, scale);
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

static lv_obj_t *make_label_with_font(lv_obj_t *parent, int x, int y, int w, int h, const char *text, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
    return label;
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h, const char *text)
{
    return make_label_with_font(parent, x, y, w, h, text, &zh_font_16);
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

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *current = lv_label_get_text(label);
    if (current == nullptr || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
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

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static void canvas_set_px_safe(lv_obj_t *canvas, int x, int y, int w, int h, lv_color_t color)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    lv_canvas_set_px_color(canvas, x, y, color);
}

static void canvas_draw_line(lv_obj_t *canvas, int w, int h, int x0, int y0, int x1, int y1, lv_color_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        canvas_set_px_safe(canvas, x0, y0, w, h, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void canvas_draw_dashed_hline(lv_obj_t *canvas, int w, int h, int x1, int x2, int y, lv_color_t color)
{
    for (int x = x1; x <= x2; ++x) {
        if (((x - x1) / 5) % 2 == 0) {
            canvas_set_px_safe(canvas, x, y, w, h, color);
        }
    }
}

static void canvas_draw_filled_circle(lv_obj_t *canvas, int w, int h, int cx, int cy, int radius, lv_color_t color)
{
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                canvas_set_px_safe(canvas, cx + x, cy + y, w, h, color);
            }
        }
    }
}

static int value_to_plot_y(float value, float min_value, float max_value, int y, int h)
{
    float range = max_value - min_value;
    if (range < 0.01f) range = 1.0f;
    float normalized = (value - min_value) / range;
    int offset = (int)(normalized * (h - 1) + 0.5f);
    return y + h - 1 - offset;
}

static void style_history_badge(lv_obj_t *label)
{
    lv_obj_set_style_bg_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(label, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_left(label, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_right(label, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(label, 0, LV_PART_MAIN);
}

static void place_badge(lv_obj_t *label, const char *text, int point_x, int point_y, int plot_x, int plot_y, int plot_w, int plot_h)
{
    set_label_text_if_changed(label, text);
    int label_w = 40;
    int label_h = 16;
    int x = 18 + point_x - label_w / 2;
    int min_y = 82 + plot_y;
    int y = 82 + point_y - label_h - 4;
    if (y < min_y) {
        y = 82 + point_y + 4;
    }
    x = clamp_int(x, 18 + plot_x, 18 + plot_x + plot_w - label_w);
    y = clamp_int(y, min_y, 82 + plot_y + plot_h - label_h);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, label_w, label_h);
}

static void format_axis_hour(time_t value, char *out, size_t out_len)
{
    struct tm local = {};
    localtime_r(&value, &local);
    snprintf(out, out_len, "%02d:00", local.tm_hour);
}

static void draw_preview_history_panel(lv_obj_t *canvas,
                                       const PreviewHistorySample *samples,
                                       bool temperature,
                                       int plot_x,
                                       int plot_y,
                                       int plot_w,
                                       int plot_h,
                                       lv_obj_t *max_label,
                                       lv_obj_t *min_label,
                                       lv_obj_t **axis_labels)
{
    float min_value = temperature ? samples[0].temp : samples[0].humi;
    float max_value = min_value;
    int min_index = 0;
    int max_index = 0;
    for (int i = 1; i < 24; ++i) {
        float value = temperature ? samples[i].temp : samples[i].humi;
        if (value < min_value) {
            min_value = value;
            min_index = i;
        }
        if (value > max_value) {
            max_value = value;
            max_index = i;
        }
    }
    for (int i = 0; i < 4; ++i) {
        int y = plot_y + (plot_h * i) / 3;
        canvas_draw_dashed_hline(canvas, kHistoryCanvasW, kHistoryCanvasH, plot_x, plot_x + plot_w, y, lv_color_black());
    }
    canvas_draw_line(canvas, kHistoryCanvasW, kHistoryCanvasH, plot_x, plot_y + plot_h, plot_x + plot_w, plot_y + plot_h, lv_color_black());

    float pad = temperature ? 0.6f : 3.0f;
    float axis_min = min_value - pad;
    float axis_max = max_value + pad;
    float axis_mid = (axis_min + axis_max) * 0.5f;
    char text[16];
    snprintf(text, sizeof(text), temperature ? "%.0f℃" : "%.0f%%", axis_max);
    set_label_text_if_changed(axis_labels[0], text);
    snprintf(text, sizeof(text), temperature ? "%.0f℃" : "%.0f%%", axis_mid);
    set_label_text_if_changed(axis_labels[1], text);
    snprintf(text, sizeof(text), temperature ? "%.0f℃" : "%.0f%%", axis_min);
    set_label_text_if_changed(axis_labels[2], text);

    int prev_x = 0;
    int prev_y = 0;
    for (int i = 0; i < 24; ++i) {
        int x = plot_x + ((i + 1) * plot_w) / 24;
        float value = temperature ? samples[i].temp : samples[i].humi;
        int y = value_to_plot_y(value, axis_min, axis_max, plot_y, plot_h);
        if (i > 0) {
            canvas_draw_line(canvas, kHistoryCanvasW, kHistoryCanvasH, prev_x, prev_y, x, y, lv_color_black());
        }
        prev_x = x;
        prev_y = y;
    }

    snprintf(text, sizeof(text), temperature ? "%.1f" : "%.0f", temperature ? samples[max_index].temp : samples[max_index].humi);
    canvas_draw_filled_circle(canvas,
                              kHistoryCanvasW,
                              kHistoryCanvasH,
                              plot_x + ((max_index + 1) * plot_w) / 24,
                              value_to_plot_y(max_value, axis_min, axis_max, plot_y, plot_h),
                              3,
                              lv_color_black());
    place_badge(max_label,
                text,
                plot_x + ((max_index + 1) * plot_w) / 24,
                value_to_plot_y(max_value, axis_min, axis_max, plot_y, plot_h),
                plot_x,
                plot_y,
                plot_w,
                plot_h);
    snprintf(text, sizeof(text), temperature ? "%.1f" : "%.0f", temperature ? samples[min_index].temp : samples[min_index].humi);
    canvas_draw_filled_circle(canvas,
                              kHistoryCanvasW,
                              kHistoryCanvasH,
                              plot_x + ((min_index + 1) * plot_w) / 24,
                              value_to_plot_y(min_value, axis_min, axis_max, plot_y, plot_h),
                              3,
                              lv_color_black());
    place_badge(min_label,
                text,
                plot_x + ((min_index + 1) * plot_w) / 24,
                value_to_plot_y(min_value, axis_min, axis_max, plot_y, plot_h),
                plot_x,
                plot_y,
                plot_w,
                plot_h);
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

    lv_obj_t *temp_title = make_label(screen, 24, 67, 80, 24, "温度");
    lv_obj_set_style_text_font(temp_title, &zh_font_16, LV_PART_MAIN);
    lv_obj_t *humi_title = make_label(screen, 24, 172, 80, 24, "湿度");
    lv_obj_set_style_text_font(humi_title, &zh_font_16, LV_PART_MAIN);

    lv_obj_t *chart = lv_canvas_create(screen);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(chart, 18, 82);
    lv_obj_set_size(chart, kHistoryCanvasW, kHistoryCanvasH);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(chart, g_history_chart_canvas_pixels.data(), kHistoryCanvasW, kHistoryCanvasH, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(chart, lv_color_white(), LV_OPA_COVER);
    lv_obj_move_foreground(temp_title);
    lv_obj_move_foreground(humi_title);

    const int time_x[5] = {42, 110, 178, 246, 314};
    local.tm_min = 0;
    local.tm_sec = 0;
    time_t end_hour = mktime(&local);
    time_t start_hour = end_hour - 24 * 3600;
    const int tick_hours[5] = {0, 6, 12, 18, 24};
    for (int i = 0; i < 5; ++i) {
        char text[8];
        format_axis_hour(start_hour + tick_hours[i] * 3600, text, sizeof(text));
        lv_obj_t *label = make_label_with_font(screen, time_x[i] - 20, 274, 48, 18, text, &lv_font_montserrat_14);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    lv_obj_t *temp_axis[3] = {};
    lv_obj_t *humi_axis[3] = {};
    for (int i = 0; i < 3; ++i) {
        temp_axis[i] = make_label(screen, 332, 84 + i * 30, 56, 18, "--");
        humi_axis[i] = make_label(screen, 332, 186 + i * 30, 56, 18, "--");
        lv_obj_set_style_text_align(temp_axis[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_set_style_text_align(humi_axis[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    }

    lv_obj_t *temp_max = make_label_with_font(screen, 0, 0, 40, 16, "--", &lv_font_montserrat_12);
    lv_obj_t *temp_min = make_label_with_font(screen, 0, 0, 40, 16, "--", &lv_font_montserrat_12);
    lv_obj_t *humi_max = make_label_with_font(screen, 0, 0, 40, 16, "--", &lv_font_montserrat_12);
    lv_obj_t *humi_min = make_label_with_font(screen, 0, 0, 40, 16, "--", &lv_font_montserrat_12);
    style_history_badge(temp_max);
    style_history_badge(temp_min);
    style_history_badge(humi_max);
    style_history_badge(humi_min);

    PreviewHistorySample samples[24] = {
        {26.9f, 58}, {26.8f, 57}, {27.2f, 64}, {27.7f, 64}, {26.8f, 59}, {26.3f, 64},
        {26.2f, 64}, {26.3f, 65}, {26.3f, 65}, {26.4f, 66}, {26.4f, 66}, {26.4f, 65},
        {26.2f, 66}, {26.3f, 65}, {26.0f, 66}, {26.1f, 65}, {26.0f, 66}, {25.9f, 68},
        {26.1f, 66}, {26.2f, 65}, {26.4f, 65}, {26.6f, 63}, {26.8f, 63}, {27.0f, 62},
    };
    draw_preview_history_panel(chart, samples, true, 34, 10, 276, 62, temp_max, temp_min, temp_axis);
    draw_preview_history_panel(chart, samples, false, 34, 112, 276, 62, humi_max, humi_min, humi_axis);
    lv_obj_invalidate(chart);

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
    static const char *week_days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    char date_text[48];
    snprintf(date_text,
             sizeof(date_text),
             "%04d/%02d/%02d / %s",
             local.tm_year + 1900,
             local.tm_mon + 1,
             local.tm_mday,
             week_days[local.tm_wday]);
    set_label_text_if_changed(g_date_label, date_text);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);

    lv_obj_t *image_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(image_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(image_canvas, 20, 62);
    lv_obj_set_size(image_canvas, CLOCK_GALLERY_IMAGE_WIDTH, CLOCK_GALLERY_IMAGE_HEIGHT);
    lv_obj_set_style_border_width(image_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(image_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(image_canvas,
                         g_gallery_image_canvas_pixels.data(),
                         CLOCK_GALLERY_IMAGE_WIDTH,
                         CLOCK_GALLERY_IMAGE_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    draw_1bit_icon(image_canvas,
                   CLOCK_GALLERY_IMAGE_WIDTH,
                   CLOCK_GALLERY_IMAGE_HEIGHT,
                   CLOCK_GALLERY_IMAGE_BYTES_PER_ROW,
                   clock_gallery_images[local.tm_hour % CLOCK_GALLERY_IMAGE_COUNT],
                   lv_color_black(),
                   lv_color_white());

    lv_obj_t *divider = make_bar(screen, 252, 66, 3, 188);
    set_obj_black(divider, true);

    lv_obj_t *time_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(time_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(time_canvas, 268, 62);
    lv_obj_set_size(time_canvas, 112, 198);
    lv_obj_set_style_border_width(time_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(time_canvas, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(time_canvas, g_gallery_time_canvas_pixels.data(), 112, 198, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(time_canvas, lv_color_white(), LV_OPA_COVER);
    draw_block_number(time_canvas, local.tm_hour, 15);
    draw_block_number(time_canvas, local.tm_min, 116);
    lv_obj_invalidate(time_canvas);

    lv_obj_t *saying = make_label(screen, 18, 272, 364, 26, "今日无事，适合慢慢来。");
    lv_obj_set_style_text_font(saying, &zh_font_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(saying, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(saying, LV_LABEL_LONG_DOT);

    update_time_ui(local);
}

static int preview_days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

static int preview_first_weekday(int year, int month)
{
    struct tm local = {};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = 1;
    local.tm_hour = 12;
    mktime(&local);
    return local.tm_wday;
}

static void draw_preview_calendar_text(lv_obj_t *canvas,
                                       const char *text,
                                       int x,
                                       int y,
                                       int w,
                                       const lv_font_t *font,
                                       lv_color_t color)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = color;
    dsc.font = font;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    lv_canvas_draw_text(canvas, x, y, w, &dsc, text);
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

    lv_obj_t *calendar = lv_canvas_create(screen);
    lv_obj_clear_flag(calendar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(calendar, 18, 62);
    lv_obj_set_size(calendar, kCalendarCanvasW, kCalendarCanvasH);
    lv_obj_set_style_border_width(calendar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(calendar, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(calendar, g_calendar_canvas_pixels.data(), kCalendarCanvasW, kCalendarCanvasH, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(calendar, lv_color_white(), LV_OPA_COVER);

    static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    static const char *const lunar_preview[] = {
        "初一", "初二", "初三", "清明", "初五", "初六", "初七", "初八",
        "初九", "初十", "十一", "十二", "十三", "十四", "十五", "十六",
        "十七", "十八", "十九", "二十", "廿一", "廿二", "廿三", "廿四",
        "廿五", "廿六", "廿七", "廿八", "廿九", "三十", "初一",
    };
    constexpr int cell_w = 52;
    constexpr int cell_h = 41;
    constexpr int header_h = 24;
    canvas_fill_rect(calendar, 0, 2, cell_w, 18, lv_color_black());
    canvas_fill_rect(calendar, cell_w * 6, 2, cell_w, 18, lv_color_black());
    canvas_dot_rect(calendar, cell_w, 2, cell_w * 5, 18);
    for (int col = 0; col < 7; ++col) {
        int x = col * cell_w;
        if (col == 0 || col == 6) {
            draw_preview_calendar_text(calendar, weekdays[col], x, 2, cell_w, &zh_font_16, lv_color_white());
        } else {
            draw_preview_calendar_text(calendar, weekdays[col], x, 2, cell_w, &zh_font_16, lv_color_black());
        }
    }
    int first = preview_first_weekday(local.tm_year + 1900, local.tm_mon + 1);
    int days = preview_days_in_month(local.tm_year + 1900, local.tm_mon + 1);
    for (int day = 1; day <= days; ++day) {
        int idx = first + day - 1;
        int col = idx % 7;
        int row = idx / 7;
        int x = col * cell_w;
        int y = header_h + 5 + row * cell_h;
        bool today = day == local.tm_mday;
        if (today) {
            canvas_fill_round_rect(calendar, x + 4, y + 3, cell_w - 8, cell_h - 5, 5, lv_color_black());
        }
        char day_text[4];
        snprintf(day_text, sizeof(day_text), "%d", day);
        draw_preview_calendar_text(calendar, day_text, x, y + 2, cell_w, &lv_font_montserrat_16, today ? lv_color_white() : lv_color_black());
        draw_preview_calendar_text(calendar,
                                   lunar_preview[(day - 1) % (int)(sizeof(lunar_preview) / sizeof(lunar_preview[0]))],
                                   x,
                                   y + 20,
                                   cell_w,
                                   &zh_font_16,
                                   today ? lv_color_white() : lv_color_black());
    }
    lv_obj_invalidate(calendar);

    update_time_ui(local);
}

static void style_weather_preview_card(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
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

    lv_obj_t *city = make_label(screen, 20, 66, 135, 24, "杭州");
    lv_obj_t *temp = make_label_with_font(screen, 20, 86, 88, 54, "26", &lv_font_montserrat_48);
    lv_obj_t *unit = make_label_with_font(screen, 88, 96, 24, 32, "C", &lv_font_montserrat_24);
    (void)city;
    (void)temp;
    (void)unit;
    lv_obj_t *icon = make_label(screen, 20, 143, 42, 40, weather_icon_text("100"));
    lv_obj_set_style_text_font(icon, &qweather_icons_36, LV_PART_MAIN);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    make_label(screen, 62, 151, 92, 24, "晴");
    make_label(screen, 20, 179, 134, 22, "今日 22/29C");

    static const char *const days[] = {"周二\n23日", "周三\n24日", "周四\n25日", "周五\n26日", "周六\n27日", "周日\n28日"};
    static const char *const texts[] = {"晴", "多云转晴", "小到中雨", "阴", "雨夹雪", "大到暴雨"};
    static const char *const icons[] = {"100", "101", "305", "104", "404", "306"};
    static const char *const ranges[] = {"22/29", "23/30", "-3/2", "22/28", "-8/-2", "18/24"};
    static const int card_x[6] = {138, 180, 222, 264, 306, 348};
    static const int card_y = 66;
    for (int i = 0; i < 6; ++i) {
        int x = card_x[i];
        int y = card_y;
        lv_obj_t *card = lv_obj_create(screen);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_size(card, 34, 126);
        style_weather_preview_card(card);
        lv_obj_t *date = make_label_with_font(screen, x, y, 34, 30, days[i], &zh_font_16);
        lv_label_set_long_mode(date, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_t *small_icon = make_label(screen, x, y + 35, 34, 36, weather_icon_text(icons[i]));
        lv_obj_set_style_text_font(small_icon, &qweather_icons_36, LV_PART_MAIN);
        lv_obj_set_style_text_align(small_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_t *text = make_label_with_font(screen, x, y + 72, 34, 34, texts[i], &zh_font_16);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_t *range = make_label_with_font(screen, x, y + 108, 34, 16, ranges[i], &lv_font_montserrat_12);
        lv_obj_set_style_text_align(range, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        (void)date;
        (void)text;
        (void)range;
    }

    lv_obj_t *detail_line = make_bar(screen, 20, 196, 360, 2);
    set_obj_black(detail_line, true);
    make_label(screen, 20, 202, 110, 22, "AQI 42 优");
    make_label(screen, 132, 202, 86, 22, "湿度 58%");
    make_label(screen, 228, 202, 152, 22, "东北风 3级");
    make_label(screen, 20, 224, 110, 20, "日出 05:01");
    make_label(screen, 132, 224, 120, 20, "日落 19:06");
    lv_obj_t *alert = make_label(screen, 20, 246, 360, 22, "预警 大风蓝 / 暴雨黄 / 雷电橙");
    lv_obj_t *advice = make_label(screen, 20, 272, 360, 20, "天气平稳，适合轻装出行。");
    lv_label_set_long_mode(advice, LV_LABEL_LONG_WRAP);

    update_time_ui(local);
}

static void draw_preview_flip_card(lv_obj_t *canvas, int value)
{
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
    constexpr int scale_num = 3;
    constexpr int scale_den = 4;
    const DsegGlyph *tens = find_dseg_glyph(kDSEG84Font, (char)('0' + value / 10));
    const DsegGlyph *ones = find_dseg_glyph(kDSEG84Font, (char)('0' + value % 10));
    if (!tens || !ones) {
        return;
    }
    auto draw_scaled = [&](const DsegGlyph *glyph, int origin_x, int origin_y) {
        int dst_w = (glyph->width * scale_num + scale_den - 1) / scale_den;
        int dst_h = (glyph->height * scale_num + scale_den - 1) / scale_den;
        int dst_x = origin_x + (glyph->x_offset * scale_num) / scale_den;
        int dst_y = origin_y + (glyph->y_offset * scale_num) / scale_den;
        for (int yy = 0; yy < dst_h; ++yy) {
            int src_y = (yy * glyph->height) / dst_h;
            for (int xx = 0; xx < dst_w; ++xx) {
                int src_x = (xx * glyph->width) / dst_w;
                uint32_t bit = (uint32_t)src_y * glyph->width + src_x;
                uint8_t byte = kDSEG84Font.bitmap[glyph->bitmap_offset + bit / 8];
                if (byte & (0x80 >> (bit & 7))) {
                    lv_canvas_set_px_color(canvas, dst_x + xx, dst_y + yy, lv_color_white());
                }
            }
        }
    };
    auto scaled = [=](int v) {
        return (v * scale_num) / scale_den;
    };
    auto scaled_size = [=](int v) {
        return (v * scale_num + scale_den - 1) / scale_den;
    };
    int tens_origin = 0;
    int ones_origin = scaled(tens->x_advance);
    int left = std::min(tens_origin + scaled(tens->x_offset),
                        ones_origin + scaled(ones->x_offset));
    int right = std::max(tens_origin + scaled(tens->x_offset) + scaled_size(tens->width),
                         ones_origin + scaled(ones->x_offset) + scaled_size(ones->width));
    int x = (kFlipCardW - (right - left)) / 2 - left;
    int baseline_y = 84;
    draw_scaled(tens, x, baseline_y);
    x += scaled(tens->x_advance);
    draw_scaled(ones, x, baseline_y);
    apply_preview_card_rounding(canvas);
    lv_obj_invalidate(canvas);
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
    static const char *flip_week_days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    char flip_date_text[48];
    snprintf(flip_date_text,
             sizeof(flip_date_text),
             "%04d/%02d/%02d / %s",
             local.tm_year + 1900,
             local.tm_mon + 1,
             local.tm_mday,
             flip_week_days[local.tm_wday]);
    set_label_text_if_changed(g_date_label, flip_date_text);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);
    lv_obj_t *day_progress = nullptr;
    build_progress_canvas(screen, &day_progress, g_flip_day_progress_pixels, 59);
    int last_day = -1;
    int seconds_of_day = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    update_progress_canvas(day_progress, (seconds_of_day * 60) / (24 * 3600), &last_day);

    static const int card_x[3] = {18, 144, 270};
    int values[3] = {local.tm_hour, local.tm_min, local.tm_sec};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *card = lv_canvas_create(screen);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(card, card_x[i], 66);
        lv_obj_set_size(card, kFlipCardW, kFlipCardH);
        lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
        lv_canvas_set_buffer(card, g_flip_card_pixels[i].data(), kFlipCardW, kFlipCardH, LV_IMG_CF_TRUE_COLOR);
        draw_preview_flip_card(card, values[i]);
    }
    lv_obj_t *sensor = make_label(screen, 18, 232, 364, 28, "温度 25.6C  湿度 46%");
    lv_obj_set_style_text_align(sensor, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(sensor, &zh_font_16, LV_PART_MAIN);
}

static uint32_t weather_icon_codepoint(const char *code)
{
    int icon = atoi(code);
    if (icon >= 100 && icon <= 104) return 0xF101 + (uint32_t)(icon - 100);
    if (icon >= 150 && icon <= 153) return 0xF106 + (uint32_t)(icon - 150);
    if (icon >= 300 && icon <= 318) return 0xF10A + (uint32_t)(icon - 300);
    if (icon >= 350 && icon <= 351) return 0xF11D + (uint32_t)(icon - 350);
    if (icon == 399) return 0xF11F;
    if (icon >= 400 && icon <= 410) return 0xF120 + (uint32_t)(icon - 400);
    if (icon >= 456 && icon <= 457) return 0xF12B + (uint32_t)(icon - 456);
    if (icon == 499) return 0xF12D;
    if (icon >= 500 && icon <= 504) return 0xF12E + (uint32_t)(icon - 500);
    if (icon >= 507 && icon <= 515) return 0xF133 + (uint32_t)(icon - 507);
    if (icon >= 800 && icon <= 807) return 0xF13C + (uint32_t)(icon - 800);
    if (icon == 900) return 0xF144;
    if (icon == 901) return 0xF145;
    if (icon == 9999) return 0xF1CB;
    return 0xF146;
}

static const char *weather_icon_text(const char *code)
{
    static char text[5];
    uint32_t cp = weather_icon_codepoint(code);
    text[0] = (char)(0xE0 | (cp >> 12));
    text[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    text[2] = (char)(0x80 | (cp & 0x3F));
    text[3] = '\0';
    return text;
}

static void build_clock_ui()
{
    lv_obj_t *screen = lv_scr_act();
    g_last_status_gif_frame = -1;
    g_last_day_progress_filled = -1;
    g_last_second_progress_filled = -1;
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

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
}

static void apply_alert_preview(bool visible)
{
    set_obj_visible(g_alert_pill, visible);
    set_obj_visible(g_chime_status_icon_canvas, !visible);
    set_obj_visible(g_wifi_status_icon_canvas, !visible);
    if (visible) {
        set_label_text_if_changed(g_alert_label, "大风蓝色预警");
    }
}

static void style_settings_item(lv_obj_t *label, bool selected)
{
    lv_obj_set_style_bg_color(label, selected ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(label, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(label, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(label, 5, LV_PART_MAIN);
}

static void build_settings_page()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(screen, 24, 18, 352, 28, "设置");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_t *top_line = make_bar(screen, 24, 52, 352, 3);
    set_obj_black(top_line, true);

    const char *items[] = {"整点提醒 ON", "同步时间", "同步天气", "确认恢复出厂设置", "关于本机"};
    static const int y_positions[] = {62, 100, 138, 176, 214};
    for (int i = 0; i < 5; ++i) {
        g_settings_labels[i] = make_label(screen, 48, y_positions[i], 304, 30, items[i]);
        lv_label_set_long_mode(g_settings_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(g_settings_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        style_settings_item(g_settings_labels[i], i == 1);
    }

    g_settings_feedback_label = make_label(screen, 24, 246, 352, 20, "正在同步时间...");
    lv_obj_set_style_text_align(g_settings_feedback_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *hint = make_label_with_font(screen, 24, 270, 352, 22, "KEY: Select    BOOT: OK", &lv_font_montserrat_14);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
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

    static const char *week_days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    char date[48];
    snprintf(date, sizeof(date), "%04d/%02d/%02d / %s",
             local.tm_year + 1900,
             local.tm_mon + 1,
             local.tm_mday,
             week_days[local.tm_wday]);
    set_label_text_if_changed(g_date_label, date);
}

static uint32_t lv_color_to_argb(lv_color_t color)
{
    uint16_t c = color.full;
    uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
    uint8_t g = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
    uint8_t b = (uint8_t)((c & 0x1F) * 255 / 31);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x >= 0 && x < kDisplayWidth && y >= 0 && y < kDisplayHeight) {
                g_framebuffer[y * kDisplayWidth + x] = lv_color_to_argb(*color_p);
            }
            ++color_p;
        }
    }
    SDL_UpdateTexture(g_texture, nullptr, g_framebuffer.data(), kDisplayWidth * (int)sizeof(uint32_t));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
    lv_disp_flush_ready(drv);
}

static void save_screenshot_ppm(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "P6\n%d %d\n255\n", kDisplayWidth, kDisplayHeight);
    for (uint32_t argb : g_framebuffer) {
        uint8_t rgb[3] = {
            (uint8_t)((argb >> 16) & 0xFF),
            (uint8_t)((argb >> 8) & 0xFF),
            (uint8_t)(argb & 0xFF),
        };
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
}

static time_t preview_time()
{
    const char *fixed = getenv("WEATHER_CLOCK_SDL_FIXED_TIME");
    if (fixed && fixed[0]) {
        return (time_t)atoll(fixed);
    }
    return time(nullptr);
}

int main(int, char **)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    g_window = SDL_CreateWindow("WeatherClock LVGL SDL Preview",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                kDisplayWidth * kWindowScale, kDisplayHeight * kWindowScale,
                                SDL_WINDOW_SHOWN);
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                  kDisplayWidth, kDisplayHeight);

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

    const char *screenshot_path = getenv("WEATHER_CLOCK_SDL_SCREENSHOT");
    const char *preview_mode = getenv("WEATHER_CLOCK_SDL_MODE");

    show_boot_screen();
    if (screenshot_path && screenshot_path[0] && preview_mode && strcmp(preview_mode, "boot") == 0) {
        for (int i = 0; i < 5; ++i) {
            lv_tick_inc(16);
            lv_timer_handler();
            SDL_Delay(16);
        }
        save_screenshot_ppm(screenshot_path);
        return 0;
    }
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
    bool history_preview = preview_mode && strcmp(preview_mode, "history") == 0;
    bool gallery_preview = preview_mode && strcmp(preview_mode, "gallery") == 0;
    bool flip_clock_preview = preview_mode && strcmp(preview_mode, "flip_clock") == 0;
    bool calendar_preview = preview_mode && strcmp(preview_mode, "calendar") == 0;
    bool weather_board_preview = preview_mode && strcmp(preview_mode, "weather_board") == 0;
    if (history_preview) {
        build_history_preview_ui();
    } else if (gallery_preview) {
        build_gallery_preview_ui();
    } else if (flip_clock_preview) {
        build_flip_clock_preview_ui();
    } else if (calendar_preview) {
        build_calendar_preview_ui();
    } else if (weather_board_preview) {
        build_weather_board_preview_ui();
    } else {
        build_clock_ui();
        set_label_text_if_changed(g_temp_label, "24.6℃");
        set_label_text_if_changed(g_humi_label, "58.0%");
        set_label_text_if_changed(g_weather_city_label, "杭州");
        set_label_text_if_changed(g_weather_info_label, "晴");
        set_label_text_if_changed(g_weather_temp_label, "26℃");
        set_label_text_if_changed(g_weather_humi_label, "58%");
        set_label_text_if_changed(g_weather_icon_label, weather_icon_text("100"));
        update_battery_icon(76);
    }

    if (screenshot_path && screenshot_path[0]) {
        if (history_preview || gallery_preview || flip_clock_preview || calendar_preview || weather_board_preview) {
            // Alternate work pages are already built above.
        } else if (preview_mode && strcmp(preview_mode, "settings") == 0) {
            build_settings_page();
        } else if (preview_mode && strcmp(preview_mode, "setup") == 0) {
            set_lower_panel_visible(false);
            set_setup_panel_visible(true);
            set_obj_visible(g_chime_status_icon_canvas, false);
            set_obj_visible(g_wifi_status_icon_canvas, false);
        } else if (preview_mode && strcmp(preview_mode, "alert") == 0) {
            apply_alert_preview(true);
        } else if (preview_mode && strcmp(preview_mode, "low") == 0) {
            update_battery_icon(4);
            apply_low_battery_preview(true);
        }
        time_t now = preview_time();
        struct tm local;
        localtime_r(&now, &local);
        if (!history_preview && !gallery_preview && !flip_clock_preview && !calendar_preview && !weather_board_preview &&
            !(preview_mode && strcmp(preview_mode, "settings") == 0)) {
            update_time_ui(local);
            if (preview_mode && strcmp(preview_mode, "low") == 0) {
                apply_low_battery_preview(true);
            }
        }
        for (int i = 0; i < 5; ++i) {
            lv_tick_inc(16);
            lv_timer_handler();
            SDL_Delay(16);
        }
        save_screenshot_ppm(screenshot_path);
        return 0;
    }

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

    SDL_DestroyTexture(g_texture);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
