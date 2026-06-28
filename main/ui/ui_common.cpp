// 提供 UI 通用控件、字体绘制、GIF、进度条和显示 flush 工具。
#include "ui_views.h"

#include "custom_assets.h"

void notify_ui_task()
{
    if (g_ui_task_handle) {
        xTaskNotifyGive(g_ui_task_handle);
    }
}

lv_color_t *alloc_canvas_buffer(int width, int height)
{
    if (width <= 0 || height <= 0) {
        ESP_LOGW(TAG, "canvas buffer invalid size %dx%d", width, height);
        return nullptr;
    }
    size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / sizeof(lv_color_t)) {
        ESP_LOGW(TAG, "canvas buffer size overflow %dx%d", width, height);
        return nullptr;
    }
    lv_color_t *buf = (lv_color_t *)heap_caps_calloc(pixel_count,
                                                     sizeof(lv_color_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (lv_color_t *)calloc(pixel_count, sizeof(lv_color_t));
    }
    if (!buf) {
        ESP_LOGW(TAG, "canvas buffer alloc failed %dx%d", width, height);
    }
    return buf;
}

void set_obj_black(lv_obj_t *obj, bool active)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_bg_color(obj, active ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN);
}

lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int w, int h)
{
    if (!parent) {
        ESP_LOGW(TAG, "bar parent unavailable");
        return nullptr;
    }
    lv_obj_t *bar = lv_obj_create(parent);
    if (!bar) {
        ESP_LOGW(TAG, "bar create failed");
        return nullptr;
    }
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    set_obj_black(bar, false);
    return bar;
}

static constexpr int kProgressSegmentCount = 60;
static constexpr int kProgressSegmentW = 5;
static constexpr int kProgressSegmentH = 3;
static constexpr int kProgressSegmentGap = 1;
static constexpr int kProgressCanvasW = kProgressSegmentCount * kProgressSegmentW + (kProgressSegmentCount - 1) * kProgressSegmentGap;
static constexpr int kProgressCanvasH = kProgressSegmentH;

void draw_progress_segment(lv_obj_t *canvas, int index, bool filled)
{
    if (!canvas || index < 0 || index >= kProgressSegmentCount) {
        return;
    }
    int x0 = index * (kProgressSegmentW + kProgressSegmentGap);
    for (int y = 0; y < kProgressSegmentH; ++y) {
        for (int x = 0; x < kProgressSegmentW; ++x) {
            bool border = x == 0 || x == kProgressSegmentW - 1 || y == 0 || y == kProgressSegmentH - 1;
            lv_canvas_set_px_color(canvas, x0 + x, y, (filled || border) ? lv_color_black() : lv_color_white());
        }
    }
}

void invalidate_progress_segment(lv_obj_t *canvas, int index)
{
    if (!canvas || index < 0 || index >= kProgressSegmentCount) {
        return;
    }
    int x0 = index * (kProgressSegmentW + kProgressSegmentGap);
    lv_area_t area = {};
    area.x1 = static_cast<lv_coord_t>(x0);
    area.y1 = 0;
    area.x2 = static_cast<lv_coord_t>(x0 + kProgressSegmentW - 1);
    area.y2 = static_cast<lv_coord_t>(kProgressSegmentH - 1);
    lv_obj_invalidate_area(canvas, &area);
}

void build_progress_canvas(lv_obj_t *parent, lv_obj_t **canvas, lv_color_t **buf, int y)
{
    if (!parent || !canvas || !buf) {
        ESP_LOGW(TAG, "progress canvas build invalid arg");
        return;
    }
    if (!*buf) {
        *buf = alloc_canvas_buffer(kProgressCanvasW, kProgressCanvasH);
    }
    *canvas = lv_canvas_create(parent);
    if (!*canvas) {
        ESP_LOGW(TAG, "progress canvas create failed");
        return;
    }
    lv_obj_clear_flag(*canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(*canvas, 20, y);
    lv_obj_set_size(*canvas, kProgressCanvasW, kProgressCanvasH);
    lv_obj_set_style_border_width(*canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(*canvas, 0, LV_PART_MAIN);
    if (*buf) {
        lv_canvas_set_buffer(*canvas, *buf, kProgressCanvasW, kProgressCanvasH, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(*canvas, lv_color_white(), LV_OPA_COVER);
        for (int i = 0; i < kProgressSegmentCount; ++i) {
            draw_progress_segment(*canvas, i, false);
        }
        lv_obj_invalidate(*canvas);
    }
}

void update_progress_canvas(lv_obj_t *canvas, int filled, int *last_filled)
{
    if (!canvas || !last_filled) {
        return;
    }
    if (filled < 0) {
        filled = 0;
    } else if (filled > kProgressSegmentCount) {
        filled = kProgressSegmentCount;
    }
    if (*last_filled < 0 || filled < *last_filled) {
        for (int i = 0; i < kProgressSegmentCount; ++i) {
            draw_progress_segment(canvas, i, i < filled);
        }
        lv_obj_invalidate(canvas);
        *last_filled = filled;
        return;
    }
    if (filled == *last_filled) {
        return;
    }
    for (int i = *last_filled; i < filled; ++i) {
        draw_progress_segment(canvas, i, true);
        invalidate_progress_segment(canvas, i);
    }
    *last_filled = filled;
}

void draw_1bit_icon(lv_obj_t *canvas,
                           int width,
                           int height,
                           int bytes_per_row,
                           const uint8_t *bits,
                           lv_color_t fg,
                           lv_color_t bg)
{
    if (!canvas || !bits) {
        return;
    }
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = bits + y * bytes_per_row;
        for (int x = 0; x < width; ++x) {
            bool set = row[x / 8] & (0x80 >> (x & 7));
            if (set) {
                lv_canvas_set_px_color(canvas, x, y, fg);
            }
        }
    }
    lv_obj_invalidate(canvas);
}

bool update_trend_icon(lv_obj_t *canvas, int trend, int *last_trend)
{
    if (!canvas) {
        return false;
    }
    if (last_trend && *last_trend == trend) {
        return false;
    }
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
    } else {
        lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
        lv_obj_invalidate(canvas);
    }
    if (last_trend) {
        *last_trend = trend;
    }
    return true;
}

const DsegGlyph *find_dseg_glyph(const DsegFont &font, char ch)
{
    const char *pos = strchr(font.chars, ch);
    if (!pos) {
        return nullptr;
    }
    return &font.glyphs[pos - font.chars];
}

int draw_dseg_text(lv_obj_t *canvas, const DsegFont &font, const char *text, int cursor_x, int baseline_y)
{
    int x_cursor = cursor_x;
    for (const char *p = text; *p; ++p) {
        const DsegGlyph *glyph = find_dseg_glyph(font, *p);
        if (!glyph) {
            continue;
        }
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

void draw_time_canvas(const struct tm &local)
{
    if (!g_time_canvas) {
        return;
    }
    lv_canvas_fill_bg(g_time_canvas, lv_color_white(), LV_OPA_COVER);

    char hm[6];
    snprintf(hm, sizeof(hm), "%02d:%02d", local.tm_hour, local.tm_min);
    draw_dseg_text(g_time_canvas, kDSEG84Font, hm, 0, 88);
    lv_obj_invalidate(g_time_canvas);
}

void draw_second_canvas(const struct tm &local)
{
    if (!g_second_canvas) {
        return;
    }
    lv_canvas_fill_bg(g_second_canvas, lv_color_white(), LV_OPA_COVER);
    char ss[3] = {
        (char)('0' + local.tm_sec / 10),
        (char)('0' + local.tm_sec % 10),
        '\0',
    };
    draw_dseg_text(g_second_canvas, kDSEG36Font, ss, 0, 40);
    lv_obj_invalidate(g_second_canvas);
}

void draw_status_gif_frame(int frame)
{
    if (!g_status_gif_canvas) {
        return;
    }
    if (frame < 0) {
        frame = 0;
    } else if (frame >= STATUS_GIF_FRAME_COUNT) {
        frame = STATUS_GIF_FRAME_COUNT - 1;
    }
    static uint8_t custom_frame[STATUS_GIF_BYTES_PER_FRAME];
    static uint8_t custom_prev_frame[STATUS_GIF_BYTES_PER_FRAME];
    static bool custom_prev_valid = false;
    const uint8_t *pixels = status_gif_frames[frame];
    const uint8_t *prev_pixels = g_last_status_gif_frame >= 0 ? status_gif_frames[g_last_status_gif_frame] : nullptr;
    bool using_custom = false;
    if (custom_assets_read_main_gif_frame(frame, custom_frame, sizeof(custom_frame))) {
        pixels = custom_frame;
        using_custom = true;
        prev_pixels = custom_prev_valid ? custom_prev_frame : nullptr;
    } else {
        custom_prev_valid = false;
    }
    uint32_t bit = 0;
    bool changed = false;
    int min_x = STATUS_GIF_WIDTH;
    int min_y = STATUS_GIF_HEIGHT;
    int max_x = -1;
    int max_y = -1;
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
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (changed || g_last_status_gif_frame != frame) {
        if (changed && min_x <= max_x && min_y <= max_y) {
            lv_area_t area = {};
            area.x1 = static_cast<lv_coord_t>(min_x);
            area.y1 = static_cast<lv_coord_t>(min_y);
            area.x2 = static_cast<lv_coord_t>(max_x);
            area.y2 = static_cast<lv_coord_t>(max_y);
            lv_obj_invalidate_area(g_status_gif_canvas, &area);
        } else {
            lv_obj_invalidate(g_status_gif_canvas);
        }
    }
    g_last_status_gif_frame = frame;
    if (using_custom) {
        memcpy(custom_prev_frame, custom_frame, sizeof(custom_prev_frame));
        custom_prev_valid = true;
    }
}

lv_obj_t *make_label_with_font(lv_obj_t *parent, int x, int y, int w, int h, const char *text, const lv_font_t *font)
{
    if (!parent) {
        ESP_LOGW(TAG, "label parent unavailable");
        return nullptr;
    }
    lv_obj_t *label = lv_label_create(parent);
    if (!label) {
        ESP_LOGW(TAG, "label create failed");
        return nullptr;
    }
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_text(label, text ? text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    if (font) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
    return label;
}

lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h, const char *text)
{
    return make_label_with_font(parent, x, y, w, h, text, &zh_font_16);
}

bool set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label) {
        return false;
    }
    if (!text) {
        text = "";
    }
    const char *current = lv_label_get_text(label);
    if (current == nullptr || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
        return true;
    }
    return false;
}

void format_time_or_dash(time_t value, char *out, size_t out_len)
{
    if (value <= 0) {
        strlcpy(out, "--", out_len);
        return;
    }
    struct tm local = {};
    localtime_r(&value, &local);
    const int year = local.tm_year + 1900;
    if (year < kMinValidYear || year > kMaxValidYear) {
        strlcpy(out, "--", out_len);
        return;
    }
    char formatted[32];
    snprintf(formatted, sizeof(formatted), "%04d-%02d-%02d %02d:%02d:%02d",
             year,
             local.tm_mon + 1,
             local.tm_mday,
             local.tm_hour,
             local.tm_min,
             local.tm_sec);
    strlcpy(out, formatted, out_len);
}
