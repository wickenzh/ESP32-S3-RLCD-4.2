// 提供 LVGL canvas 安全打点、线段、虚线和圆点等基础绘图工具。
#include "ui_views.h"

namespace {
constexpr int kDashedLineRunPixels = 5;
constexpr int kDashedLinePeriodPixels = kDashedLineRunPixels * 2;
constexpr int kBresenhamErrorScale = 2;

bool canvas_size_valid(int w, int h)
{
    return w > 0 && h > 0;
}

void order_int_pair(int *first, int *second)
{
    if (!first || !second || *first <= *second) {
        return;
    }
    int tmp = *first;
    *first = *second;
    *second = tmp;
}

int abs_delta(int start, int end)
{
    return end > start ? end - start : start - end;
}

int line_step(int start, int end)
{
    return start < end ? 1 : -1;
}

int square_int(int value)
{
    return value * value;
}
} // namespace

int clamp_int(int value, int min_value, int max_value)
{
    order_int_pair(&min_value, &max_value);
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
    if (!canvas || !canvas_size_valid(w, h) || x < 0 || y < 0 || x >= w || y >= h) {
        return;
    }
    lv_canvas_set_px_color(canvas, x, y, color);
}

void canvas_draw_line(lv_obj_t *canvas, int w, int h, int x0, int y0, int x1, int y1, lv_color_t color)
{
    if (!canvas || !canvas_size_valid(w, h)) {
        return;
    }
    int dx = abs_delta(x0, x1);
    int sx = line_step(x0, x1);
    int dy = -abs_delta(y0, y1);
    int sy = line_step(y0, y1);
    int err = dx + dy;
    for (;;) {
        canvas_set_px_safe(canvas, x0, y0, w, h, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = kBresenhamErrorScale * err;
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
    if (!canvas || !canvas_size_valid(w, h) || y < 0 || y >= h) {
        return;
    }
    order_int_pair(&x1, &x2);
    if (x2 < 0 || x1 >= w) {
        return;
    }
    int draw_x1 = clamp_int(x1, 0, w - 1);
    int draw_x2 = clamp_int(x2, 0, w - 1);
    for (int x = draw_x1; x <= draw_x2; ++x) {
        if (((x - x1) % kDashedLinePeriodPixels) < kDashedLineRunPixels) {
            canvas_set_px_safe(canvas, x, y, w, h, color);
        }
    }
}

void canvas_draw_filled_circle(lv_obj_t *canvas, int w, int h, int cx, int cy, int radius, lv_color_t color)
{
    if (!canvas || !canvas_size_valid(w, h) || radius < 0) {
        return;
    }
    if (cx + radius < 0 || cx - radius >= w || cy + radius < 0 || cy - radius >= h) {
        return;
    }
    const int radius_squared = square_int(radius);
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (square_int(x) + square_int(y) <= radius_squared) {
                canvas_set_px_safe(canvas, cx + x, cy + y, w, h, color);
            }
        }
    }
}
