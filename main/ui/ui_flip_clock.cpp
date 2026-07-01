// 绘制第六页翻页时钟，秒级只局部刷新对应数字牌。
#include "ui_views.h"

#include <algorithm>

namespace {

static constexpr int kCardCount = 3;
static constexpr int kCardW = 112;
static constexpr int kCardH = 112;
static constexpr int kCardY = 66;
static constexpr int kCardRadius = 8;
static constexpr int kCardX[kCardCount] = {18, 144, 270};
static constexpr int kHourCardIndex = 0;
static constexpr int kMinuteCardIndex = 1;
static constexpr int kSecondCardIndex = 2;
static constexpr int kSecondsPerMinute = 60;
static constexpr int kMinutesPerHour = 60;
static constexpr int kHoursPerDay = 24;
static constexpr int kProgressSegmentCount = 60;
static constexpr int kSecondsPerHour = kMinutesPerHour * kSecondsPerMinute;
static constexpr int kSecondsPerDay = kHoursPerDay * kSecondsPerHour;
static constexpr int kDigitScaleNumerator = 3;
static constexpr int kDigitScaleDenominator = 4;
static constexpr int kDigitBaselineY = 84;
static constexpr size_t kFlipSensorTextSize = 48;
static constexpr const char *kFlipSensorPlaceholder = "温度 --.-C  湿度 --%";
static constexpr const char *kFlipSensorFormat = "温度 %.1fC  湿度 %.0f%%";

void apply_card_rounding(lv_obj_t *canvas)
{
    if (!canvas) {
        return;
    }
    int radius = kCardRadius;
    int r2 = radius * radius;
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            int dx = radius - 1 - x;
            int dy = radius - 1 - y;
            if (dx * dx + dy * dy > r2) {
                canvas_set_px_safe(canvas, x, y, kCardW, kCardH, lv_color_white());
                canvas_set_px_safe(canvas, kCardW - 1 - x, y, kCardW, kCardH, lv_color_white());
                canvas_set_px_safe(canvas, x, kCardH - 1 - y, kCardW, kCardH, lv_color_white());
                canvas_set_px_safe(canvas, kCardW - 1 - x, kCardH - 1 - y, kCardW, kCardH, lv_color_white());
            }
        }
    }
}

bool dseg_pixel_on(const DsegFont &font, const DsegGlyph *glyph, int x, int y)
{
    uint32_t bit = (uint32_t)y * glyph->width + x;
    return packed_1bit_bit_is_set(font.bitmap + glyph->bitmap_offset, bit);
}

void draw_scaled_dseg_digit(lv_obj_t *canvas,
                            const DsegGlyph *glyph,
                            int origin_x,
                            int origin_y,
                            int scale_num,
                            int scale_den,
                            int clip_y0 = 0,
                            int clip_y1 = kCardH)
{
    if (!canvas || !glyph || scale_num <= 0 || scale_den <= 0) {
        return;
    }
    int dst_w = (glyph->width * scale_num + scale_den - 1) / scale_den;
    int dst_h = (glyph->height * scale_num + scale_den - 1) / scale_den;
    int dst_x = origin_x + (glyph->x_offset * scale_num) / scale_den;
    int dst_y = origin_y + (glyph->y_offset * scale_num) / scale_den;
    for (int y = 0; y < dst_h; ++y) {
        int src_y = (y * glyph->height) / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            int src_x = (x * glyph->width) / dst_w;
            int py = dst_y + y;
            if (py >= clip_y0 && py < clip_y1 && dseg_pixel_on(kDSEG84Font, glyph, src_x, src_y)) {
                canvas_set_px_safe(canvas, dst_x + x, dst_y + y, kCardW, kCardH, lv_color_white());
            }
        }
    }
}

void draw_card_shell(lv_obj_t *canvas)
{
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
}

void draw_card_digits(lv_obj_t *canvas, int value, int clip_y0 = 0, int clip_y1 = kCardH)
{
    const DsegGlyph *tens = find_dseg_glyph(kDSEG84Font, (char)('0' + value / 10));
    const DsegGlyph *ones = find_dseg_glyph(kDSEG84Font, (char)('0' + value % 10));
    if (!tens || !ones) {
        return;
    }
    auto scaled = [=](int value) {
        return (value * kDigitScaleNumerator) / kDigitScaleDenominator;
    };
    auto scaled_size = [=](int value) {
        return (value * kDigitScaleNumerator + kDigitScaleDenominator - 1) / kDigitScaleDenominator;
    };
    int tens_origin = 0;
    int ones_origin = scaled(tens->x_advance);
    int left = std::min(tens_origin + scaled(tens->x_offset),
                        ones_origin + scaled(ones->x_offset));
    int right = std::max(tens_origin + scaled(tens->x_offset) + scaled_size(tens->width),
                         ones_origin + scaled(ones->x_offset) + scaled_size(ones->width));
    int x = (kCardW - (right - left)) / 2 - left;
    draw_scaled_dseg_digit(canvas,
                           tens,
                           x,
                           kDigitBaselineY,
                           kDigitScaleNumerator,
                           kDigitScaleDenominator,
                           clip_y0,
                           clip_y1);
    x += scaled(tens->x_advance);
    draw_scaled_dseg_digit(canvas,
                           ones,
                           x,
                           kDigitBaselineY,
                           kDigitScaleNumerator,
                           kDigitScaleDenominator,
                           clip_y0,
                           clip_y1);
}

void draw_flip_card(int card_index, int value)
{
    if (card_index < 0 || card_index >= kCardCount || !g_flip_clock_card_canvas[card_index]) {
        return;
    }
    lv_obj_t *canvas = g_flip_clock_card_canvas[card_index];
    draw_card_shell(canvas);
    draw_card_digits(canvas, value);
    apply_card_rounding(canvas);
    lv_obj_invalidate(canvas);
}

bool update_flip_sensor_text()
{
    if (!g_flip_clock_sensor_label) {
        return false;
    }
    char text[kFlipSensorTextSize];
    if (g_sensor_ok) {
        snprintf(text, sizeof(text), kFlipSensorFormat, g_temperature, g_humidity);
    } else {
        strlcpy(text, kFlipSensorPlaceholder, sizeof(text));
    }
    return set_label_text_if_changed(g_flip_clock_sensor_label, text);
}

} // namespace

void build_flip_clock_page()
{
    if (g_flip_clock_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    g_flip_clock_root = screen;
    lv_obj_add_flag(g_flip_clock_root, LV_OBJ_FLAG_HIDDEN);

    build_battery_icon(screen, g_flip_clock_battery_segments);
    build_work_page_status_bar(screen,
                               kWorkPageFlipClock,
                               &g_flip_clock_date_label,
                               nullptr,
                               &g_flip_clock_status_time_label,
                               false);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);
    build_progress_canvas(screen, &g_flip_clock_day_progress_canvas, &g_flip_clock_day_progress_canvas_buf, 59);

    for (int i = 0; i < kCardCount; ++i) {
        if (!g_flip_clock_card_canvas_buf[i]) {
            g_flip_clock_card_canvas_buf[i] = alloc_canvas_buffer(kCardW, kCardH);
        }
        g_flip_clock_card_canvas[i] = lv_canvas_create(screen);
        if (!g_flip_clock_card_canvas[i]) {
            ESP_LOGW(TAG, "flip clock card %d canvas create failed", i);
            continue;
        }
        lv_obj_clear_flag(g_flip_clock_card_canvas[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(g_flip_clock_card_canvas[i], kCardX[i], kCardY);
        lv_obj_set_size(g_flip_clock_card_canvas[i], kCardW, kCardH);
        lv_obj_set_style_border_width(g_flip_clock_card_canvas[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(g_flip_clock_card_canvas[i], 0, LV_PART_MAIN);
        if (g_flip_clock_card_canvas_buf[i]) {
            lv_canvas_set_buffer(g_flip_clock_card_canvas[i],
                                 g_flip_clock_card_canvas_buf[i],
                                 kCardW,
                                 kCardH,
                                 LV_IMG_CF_TRUE_COLOR);
            lv_canvas_fill_bg(g_flip_clock_card_canvas[i], lv_color_black(), LV_OPA_COVER);
        }
    }

    g_flip_clock_sensor_label = make_label(screen, 18, 232, 364, 28, kFlipSensorPlaceholder);
    if (g_flip_clock_sensor_label) {
        lv_obj_set_style_text_align(g_flip_clock_sensor_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(g_flip_clock_sensor_label, &zh_font_16, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "flip clock sensor label create failed");
    }

    update_battery_segments(g_flip_clock_battery_segments, g_battery_percent);
    g_last_flip_clock_hour = -1;
    g_last_flip_clock_minute = -1;
    g_last_flip_clock_second = -1;
    g_last_flip_day_progress_filled = -1;
    g_last_flip_second_progress_filled = -1;
    g_last_flip_sensor_minute = -1;
}

bool update_flip_clock_page(const struct tm &local)
{
    build_flip_clock_page();
    if (!g_flip_clock_root) {
        return false;
    }
    bool changed = false;
    int hour = local.tm_hour;
    int minute = local.tm_min;
    int second = local.tm_sec;
    if (hour != g_last_flip_clock_hour) {
        g_last_flip_clock_hour = hour;
        draw_flip_card(kHourCardIndex, hour);
        changed = true;
    }
    if (minute != g_last_flip_clock_minute) {
        g_last_flip_clock_minute = minute;
        draw_flip_card(kMinuteCardIndex, minute);
        changed = true;
    }
    if (second != g_last_flip_clock_second) {
        g_last_flip_clock_second = second;
        draw_flip_card(kSecondCardIndex, second);
        changed = true;
    }

    int seconds_of_day = local.tm_hour * kSecondsPerHour + local.tm_min * kSecondsPerMinute + local.tm_sec;
    int day_filled = (seconds_of_day * kProgressSegmentCount) / kSecondsPerDay;
    update_progress_canvas(g_flip_clock_day_progress_canvas, day_filled, &g_last_flip_day_progress_filled);
    if (minute != g_last_flip_sensor_minute) {
        g_last_flip_sensor_minute = minute;
        changed |= update_flip_sensor_text();
    }
    return changed;
}
