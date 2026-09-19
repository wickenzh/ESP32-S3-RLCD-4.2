// 在温度标签的空格+C预留区绘制随字体缩放的度数环，无需新增字体。
#pragma once
#include "lvgl.h"

inline void ui_celsius_marker_draw(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_target(event);
    const char *text = lv_label_get_text(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_font_glyph_dsc_t glyph = {};
    if (!text || !font || !lv_font_get_glyph_dsc(font, &glyph, 'C', 0)) {
        return;
    }
    lv_area_t content;
    lv_obj_get_content_coords(label, &content);
    uint32_t byte = 0, index = 0, previous = 0;
    while (text[byte]) {
        const uint32_t codepoint = _lv_txt_encoded_next(text, &byte);
        if (codepoint == 'C' && previous == ' ' && index > 0) {
            lv_point_t space, letter;
            lv_label_get_letter_pos(label, index - 1, &space);
            lv_label_get_letter_pos(label, index, &letter);
            const int gap = letter.x - space.x;
            int diameter = font->line_height / 6;
            if (diameter < 3) diameter = 3;
            if (diameter > gap - 1) diameter = gap - 1;
            if (diameter >= 2 && space.y == letter.y) {
                const lv_coord_t x = content.x1 + space.x + (gap - diameter) / 2;
                const lv_coord_t y = content.y1 + letter.y + font->line_height -
                                     font->base_line - glyph.box_h - glyph.ofs_y;
                lv_area_t ring = {x, y, static_cast<lv_coord_t>(x + diameter - 1),
                                 static_cast<lv_coord_t>(y + diameter - 1)};
                lv_draw_rect_dsc_t style;
                lv_draw_rect_dsc_init(&style);
                style.bg_opa = LV_OPA_TRANSP;
                style.border_color = lv_obj_get_style_text_color(label, LV_PART_MAIN);
                style.border_opa = lv_obj_get_style_text_opa(label, LV_PART_MAIN);
                style.border_width = 1;
                style.radius = LV_RADIUS_CIRCLE;
                lv_draw_rect(lv_event_get_draw_ctx(event), &style, &ring);
            }
        }
        previous = codepoint;
        ++index;
    }
}

inline void ui_enable_celsius_marker(lv_obj_t *label)
{
    if (label) {
        lv_obj_add_event_cb(label, ui_celsius_marker_draw, LV_EVENT_DRAW_MAIN_END, nullptr);
    }
}
