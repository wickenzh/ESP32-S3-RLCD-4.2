// 构建 SDL 温湿时钟数字牌、传感器状态和日期农历主体。
#include "sdl_preview_flip_clock.h"
#include "ui_celsius_marker.h"

#include <cstddef>
#include <cstdio>
#include <vector>

#include "flip_sensor_icons.h"
#include "sdl_preview_flip_cards.h"
#include "sdl_preview_widgets.h"

LV_FONT_DECLARE(zh_flip_lunar_22);

namespace {

std::vector<lv_color_t> g_flip_temp_mood_pixels(FLIP_SENSOR_ICON_WIDTH * FLIP_SENSOR_ICON_HEIGHT);
std::vector<lv_color_t> g_flip_humi_mood_pixels(FLIP_SENSOR_ICON_WIDTH * FLIP_SENSOR_ICON_HEIGHT);

void style_white_left_label(lv_obj_t *label)
{
    if (!label) {
        return;
    }
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
}

void style_white_center_labels(lv_obj_t *const *labels, size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        lv_obj_t *label = labels[index];
        if (!label) {
            continue;
        }
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    }
}

} // namespace

void build_flip_clock_preview_body(lv_obj_t *screen, const struct tm *local)
{
    if (!screen || !local) {
        return;
    }
    static const int card_x[3] = {18, 144, 270};
    int values[3] = {local->tm_hour, local->tm_min, local->tm_sec};
    for (int index = 0; index < 3; ++index) {
        lv_obj_t *card = sdl_preview_flip_cards::create_preview_flip_card(
            screen, index, card_x[index], 66);
        sdl_preview_flip_cards::draw_preview_flip_card(card, values[index]);
    }

    lv_obj_t *sensor_panel = sdl_preview_widgets::make_black_bar(screen, 18, 198, 238, 88);
    lv_obj_set_style_radius(sensor_panel, 18, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(sensor_panel, true, LV_PART_MAIN);
    lv_obj_t *temp = sdl_preview_widgets::make_label_with_font(
        screen, 34, 204, 148, 36, "25.6 C", &lv_font_montserrat_24);
    style_white_left_label(temp);
    ui_enable_celsius_marker(temp);
    lv_obj_t *temp_bold = sdl_preview_widgets::make_label_with_font(
        screen, 35, 204, 148, 36, "25.6 C", &lv_font_montserrat_24);
    style_white_left_label(temp_bold);
    ui_enable_celsius_marker(temp_bold);
    lv_obj_t *humi = sdl_preview_widgets::make_label_with_font(
        screen, 34, 243, 148, 36, "46%", &lv_font_montserrat_24);
    style_white_left_label(humi);
    lv_obj_t *humi_bold = sdl_preview_widgets::make_label_with_font(
        screen, 35, 243, 148, 36, "46%", &lv_font_montserrat_24);
    style_white_left_label(humi_bold);

    lv_obj_t *temp_mood = lv_canvas_create(screen);
    lv_obj_clear_flag(temp_mood, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(temp_mood, 200, 204);
    lv_obj_set_size(temp_mood, FLIP_SENSOR_ICON_WIDTH, FLIP_SENSOR_ICON_HEIGHT);
    lv_obj_set_style_border_width(temp_mood, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(temp_mood, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(temp_mood,
                         g_flip_temp_mood_pixels.data(),
                         FLIP_SENSOR_ICON_WIDTH,
                         FLIP_SENSOR_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    sdl_preview_widgets::draw_1bit_icon(temp_mood,
                                        FLIP_SENSOR_ICON_WIDTH,
                                        FLIP_SENSOR_ICON_HEIGHT,
                                        FLIP_SENSOR_ICON_BYTES_PER_ROW,
                                        flip_temp_comfort_icon_bits,
                                        lv_color_white(),
                                        lv_color_black());
    lv_obj_t *humi_mood = lv_canvas_create(screen);
    lv_obj_clear_flag(humi_mood, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(humi_mood, 200, 244);
    lv_obj_set_size(humi_mood, FLIP_SENSOR_ICON_WIDTH, FLIP_SENSOR_ICON_HEIGHT);
    lv_obj_set_style_border_width(humi_mood, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(humi_mood, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(humi_mood,
                         g_flip_humi_mood_pixels.data(),
                         FLIP_SENSOR_ICON_WIDTH,
                         FLIP_SENSOR_ICON_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    sdl_preview_widgets::draw_1bit_icon(humi_mood,
                                        FLIP_SENSOR_ICON_WIDTH,
                                        FLIP_SENSOR_ICON_HEIGHT,
                                        FLIP_SENSOR_ICON_BYTES_PER_ROW,
                                        flip_humi_comfort_icon_bits,
                                        lv_color_white(),
                                        lv_color_black());

    lv_obj_t *date_panel = sdl_preview_widgets::make_black_bar(screen, 270, 198, 112, 88);
    lv_obj_set_style_radius(date_panel, 18, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(date_panel, true, LV_PART_MAIN);
    char day_text[8];
    std::snprintf(day_text, sizeof(day_text), "%d", local->tm_mday);
    lv_obj_t *day = sdl_preview_widgets::make_label_with_font(
        screen, 270, 196, 112, 52, day_text, &lv_font_montserrat_48);
    lv_obj_t *day_bold_x = sdl_preview_widgets::make_label_with_font(
        screen, 271, 196, 112, 52, day_text, &lv_font_montserrat_48);
    lv_obj_t *day_bold_y = sdl_preview_widgets::make_label_with_font(
        screen, 270, 197, 112, 52, day_text, &lv_font_montserrat_48);
    lv_obj_t *day_labels[] = {day, day_bold_x, day_bold_y};
    style_white_center_labels(day_labels, sizeof(day_labels) / sizeof(day_labels[0]));

    lv_obj_t *lunar = sdl_preview_widgets::make_label_with_font(
        screen, 270, 243, 112, 42, "初八", &zh_flip_lunar_22);
    lv_obj_t *lunar_bold_x = sdl_preview_widgets::make_label_with_font(
        screen, 271, 243, 112, 42, "初八", &zh_flip_lunar_22);
    lv_obj_t *lunar_bold_y = sdl_preview_widgets::make_label_with_font(
        screen, 270, 244, 112, 42, "初八", &zh_flip_lunar_22);
    lv_obj_t *lunar_bold_xy = sdl_preview_widgets::make_label_with_font(
        screen, 271, 244, 112, 42, "初八", &zh_flip_lunar_22);
    lv_obj_t *lunar_labels[] = {lunar, lunar_bold_x, lunar_bold_y, lunar_bold_xy};
    style_white_center_labels(lunar_labels, sizeof(lunar_labels) / sizeof(lunar_labels[0]));
}
