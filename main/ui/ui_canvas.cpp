// 提供 LVGL canvas 安全打点、线段、虚线和圆点等基础绘图工具。
#include "ui_views.h"

int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

void canvas_set_px_safe(lv_obj_t *canvas, int x, int y, int w, int h, lv_color_t color)
{
    if (!canvas || w <= 0 || h <= 0 || x < 0 || y < 0 || x >= w || y >= h) {
        return;
    }
    lv_canvas_set_px_color(canvas, x, y, color);
}

void canvas_draw_line(lv_obj_t *canvas, int w, int h, int x0, int y0, int x1, int y1, lv_color_t color)
{
    if (!canvas || w <= 0 || h <= 0) {
        return;
    }
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        canvas_set_px_safe(canvas, x0, y0, w, h, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
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

void canvas_draw_dashed_hline(lv_obj_t *canvas, int w, int h, int x1, int x2, int y, lv_color_t color)
{
    if (!canvas || w <= 0 || h <= 0 || y < 0 || y >= h) {
        return;
    }
    if (x2 < x1) {
        int tmp = x1;
        x1 = x2;
        x2 = tmp;
    }
    for (int x = x1; x <= x2; ++x) {
        if (((x - x1) / 5) % 2 == 0) {
            canvas_set_px_safe(canvas, x, y, w, h, color);
        }
    }
}

void canvas_draw_filled_circle(lv_obj_t *canvas, int w, int h, int cx, int cy, int radius, lv_color_t color)
{
    if (!canvas || w <= 0 || h <= 0 || radius < 0) {
        return;
    }
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                canvas_set_px_safe(canvas, cx + x, cy + y, w, h, color);
            }
        }
    }
}
