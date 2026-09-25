// 读取内置或自定义状态 GIF 帧，并执行差分局部绘制。
#include "ui_status_gif.h"

#include <cstring>
#include <esp_attr.h>

#include "custom_assets.h"
#include "packed_1bit_diff.h"
#include "status_gif_60.h"
#include "ui_canvas_primitives.h"
#include "ui_clock_surface_objects.h"
#include "ui_draw_cache.h"

namespace {
static_assert(STATUS_GIF_FRAME_COUNT > 0, "status gif must contain at least one frame");
int s_last_status_gif_frame = -1;
EXT_RAM_BSS_ATTR uint8_t s_custom_frame[STATUS_GIF_BYTES_PER_FRAME];
EXT_RAM_BSS_ATTR uint8_t s_custom_prev_frame[STATUS_GIF_BYTES_PER_FRAME];
bool s_custom_prev_valid = false;

bool is_status_gif_frame_index(int frame)
{
    return frame >= 0 && frame < STATUS_GIF_FRAME_COUNT;
}

int clamp_status_gif_frame(int frame)
{
    if (frame < 0) {
        return 0;
    }
    if (frame >= STATUS_GIF_FRAME_COUNT) {
        return STATUS_GIF_FRAME_COUNT - 1;
    }
    return frame;
}

const uint8_t *builtin_status_gif_frame_or_null(int frame)
{
    return is_status_gif_frame_index(frame) ? status_gif_frames[frame] : nullptr;
}

void expand_area_to_include_pixel(int x, int y, int *min_x, int *min_y, int *max_x, int *max_y)
{
    if (!min_x || !min_y || !max_x || !max_y) {
        return;
    }
    if (x < *min_x) {
        *min_x = x;
    }
    if (x > *max_x) {
        *max_x = x;
    }
    if (y < *min_y) {
        *min_y = y;
    }
    if (y > *max_y) {
        *max_y = y;
    }
}

bool change_area_valid(bool changed, int min_x, int min_y, int max_x, int max_y)
{
    return changed && min_x <= max_x && min_y <= max_y;
}

}

void invalidate_status_gif_draw_cache()
{
    s_last_status_gif_frame = -1;
}

void draw_status_gif_frame(int frame)
{
    lv_obj_t *canvas = clock_surface_object_refs().status_gif_canvas;
    if (!canvas) {
        return;
    }
    lv_img_dsc_t *image = lv_canvas_get_img(canvas);
    if (!image) {
        return;
    }
    frame = clamp_status_gif_frame(frame);
    const uint8_t *pixels = builtin_status_gif_frame_or_null(frame);
    const uint8_t *prev_pixels = builtin_status_gif_frame_or_null(s_last_status_gif_frame);
    bool using_custom = false;
    if (custom_assets_read_main_gif_frame(frame,
                                          s_custom_frame,
                                          sizeof(s_custom_frame))) {
        pixels = s_custom_frame;
        using_custom = true;
        prev_pixels = s_custom_prev_valid ? s_custom_prev_frame : nullptr;
    } else {
        if (s_custom_prev_valid) {
            prev_pixels = nullptr;
        }
        s_custom_prev_valid = false;
    }
    bool changed = false;
    int min_x = STATUS_GIF_WIDTH;
    int min_y = STATUS_GIF_HEIGHT;
    int max_x = -1;
    int max_y = -1;
    Packed1BitDiffCursor diff;
    packed_1bit_diff_begin(&diff,
                           pixels,
                           prev_pixels,
                           STATUS_GIF_WIDTH * STATUS_GIF_HEIGHT);
    uint32_t bit = 0;
    bool black = false;
    while (packed_1bit_diff_next(&diff, &bit, &black)) {
        const int y = static_cast<int>(bit / STATUS_GIF_WIDTH);
        const int x = static_cast<int>(bit - y * STATUS_GIF_WIDTH);
        lv_img_buf_set_px_color(image,
                                x,
                                y,
                                black ? lv_color_black() : lv_color_white());
        changed = true;
        expand_area_to_include_pixel(x, y, &min_x, &min_y, &max_x, &max_y);
    }
    if (changed || s_last_status_gif_frame != frame) {
        if (change_area_valid(changed, min_x, min_y, max_x, max_y)) {
            invalidate_canvas_rect(canvas, min_x, min_y, max_x, max_y);
        } else {
            lv_obj_invalidate(canvas);
        }
    }
    s_last_status_gif_frame = frame;
    if (using_custom) {
        memcpy(s_custom_prev_frame,
               s_custom_frame,
               sizeof(s_custom_prev_frame));
        s_custom_prev_valid = true;
    }
}
