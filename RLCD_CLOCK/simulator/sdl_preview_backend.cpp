// 实现 SDL 预览显示后端和确定性 PPM 截图写出。
#include "sdl_preview_backend.h"
#include "ui_canvas_primitives.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace {
uint32_t lv_color_to_argb(lv_color_t color)
{
    uint16_t packed = color.full;
    uint8_t red = static_cast<uint8_t>(((packed >> 11) & 0x1F) * 255 / 31);
    uint8_t green = static_cast<uint8_t>(((packed >> 5) & 0x3F) * 255 / 63);
    uint8_t blue = static_cast<uint8_t>((packed & 0x1F) * 255 / 31);
    return 0xFF000000u |
           (static_cast<uint32_t>(red) << 16) |
           (static_cast<uint32_t>(green) << 8) |
           blue;
}
} // namespace

void invalidate_canvas_rect(lv_obj_t *canvas, int x1, int y1, int x2, int y2)
{
    if (!canvas) {
        return;
    }
    if (x1 > x2) {
        const int tmp = x1;
        x1 = x2;
        x2 = tmp;
    }
    if (y1 > y2) {
        const int tmp = y1;
        y1 = y2;
        y2 = tmp;
    }
    lv_area_t coords = {};
    lv_obj_get_coords(canvas, &coords);
    lv_area_t area = {
        static_cast<lv_coord_t>(coords.x1 + x1),
        static_cast<lv_coord_t>(coords.y1 + y1),
        static_cast<lv_coord_t>(coords.x1 + x2),
        static_cast<lv_coord_t>(coords.y1 + y2),
    };
    lv_obj_invalidate_area(canvas, &area);
}

SdlPreviewBackend::SdlPreviewBackend(int display_width, int display_height)
    : width(display_width),
      height(display_height),
      framebuffer(static_cast<size_t>(display_width * display_height), 0xFFFFFFFF)
{
}

bool sdl_preview_backend_init(SdlPreviewBackend *backend,
                              const char *window_title,
                              int window_scale)
{
    if (!backend || !window_title || window_scale <= 0) {
        fprintf(stderr, "SDL preview backend init received invalid arguments\n");
        return false;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    backend->window = SDL_CreateWindow(window_title,
                                       SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED,
                                       backend->width * window_scale,
                                       backend->height * window_scale,
                                       SDL_WINDOW_SHOWN);
    if (!backend->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        sdl_preview_backend_cleanup(backend);
        return false;
    }
    backend->renderer = SDL_CreateRenderer(backend->window,
                                           -1,
                                           SDL_RENDERER_ACCELERATED |
                                               SDL_RENDERER_PRESENTVSYNC);
    if (!backend->renderer) {
        backend->renderer = SDL_CreateRenderer(backend->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!backend->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        sdl_preview_backend_cleanup(backend);
        return false;
    }
    backend->texture = SDL_CreateTexture(backend->renderer,
                                         SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         backend->width,
                                         backend->height);
    if (!backend->texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        sdl_preview_backend_cleanup(backend);
        return false;
    }
    return true;
}

void sdl_preview_backend_flush(SdlPreviewBackend *backend,
                               lv_disp_drv_t *driver,
                               const lv_area_t *area,
                               lv_color_t *color)
{
    if (!backend || !driver || !area || !color ||
        !backend->texture || !backend->renderer) {
        if (driver) {
            lv_disp_flush_ready(driver);
        }
        return;
    }
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x >= 0 && x < backend->width && y >= 0 && y < backend->height) {
                backend->framebuffer[static_cast<size_t>(y * backend->width + x)] =
                    lv_color_to_argb(*color);
            }
            ++color;
        }
    }
    SDL_UpdateTexture(backend->texture,
                      nullptr,
                      backend->framebuffer.data(),
                      backend->width * static_cast<int>(sizeof(uint32_t)));
    SDL_RenderClear(backend->renderer);
    SDL_RenderCopy(backend->renderer, backend->texture, nullptr, nullptr);
    SDL_RenderPresent(backend->renderer);
    lv_disp_flush_ready(driver);
}

void sdl_preview_backend_save_ppm(const SdlPreviewBackend *backend, const char *path)
{
    if (!backend || !path || path[0] == '\0') {
        fprintf(stderr, "SDL preview screenshot received invalid arguments\n");
        return;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "SDL preview screenshot open failed: %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", backend->width, backend->height);
    for (uint32_t argb : backend->framebuffer) {
        uint8_t rgb[3] = {
            static_cast<uint8_t>((argb >> 16) & 0xFF),
            static_cast<uint8_t>((argb >> 8) & 0xFF),
            static_cast<uint8_t>(argb & 0xFF),
        };
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
}

void sdl_preview_backend_cleanup(SdlPreviewBackend *backend)
{
    if (!backend) {
        return;
    }
    if (backend->texture) {
        SDL_DestroyTexture(backend->texture);
        backend->texture = nullptr;
    }
    if (backend->renderer) {
        SDL_DestroyRenderer(backend->renderer);
        backend->renderer = nullptr;
    }
    if (backend->window) {
        SDL_DestroyWindow(backend->window);
        backend->window = nullptr;
    }
    SDL_Quit();
}
