#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "lvgl.h"
#include "dseg_digits.h"
#include "boot_anim.h"

LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_font_16);

static constexpr int kDisplayWidth = 400;
static constexpr int kDisplayHeight = 300;
static constexpr int kWindowScale = 2;
static const char *APP_VERSION = "v0.0.39";
static constexpr int kTimeCanvasW = 292;
static constexpr int kTimeCanvasH = 92;
static constexpr int kSecondCanvasW = 60;
static constexpr int kSecondCanvasH = 40;
static constexpr int kBootAnimFrameMs = 100;

static SDL_Window *g_window = nullptr;
static SDL_Renderer *g_renderer = nullptr;
static SDL_Texture *g_texture = nullptr;
static std::vector<uint32_t> g_framebuffer(kDisplayWidth * kDisplayHeight, 0xFFFFFFFF);

static lv_obj_t *g_date_label;
static lv_obj_t *g_temp_label;
static lv_obj_t *g_humi_label;
static lv_obj_t *g_weather_city_label;
static lv_obj_t *g_weather_info_label;
static lv_obj_t *g_weather_icon_label;
static lv_obj_t *g_battery_segments[5];
static lv_obj_t *g_time_canvas;
static lv_obj_t *g_second_canvas;
static lv_obj_t *g_boot_anim_canvas;
static std::vector<lv_color_t> g_time_canvas_pixels(kTimeCanvasW * kTimeCanvasH);
static std::vector<lv_color_t> g_second_canvas_pixels(kSecondCanvasW * kSecondCanvasH);
static std::vector<lv_color_t> g_boot_anim_canvas_pixels(BOOT_ANIM_WIDTH * BOOT_ANIM_HEIGHT);

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

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
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
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    g_date_label = make_label(screen, 116, 15, 264, 26, "----/--/-- / 星期-");
    lv_obj_set_style_text_align(g_date_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    build_battery_icon(screen);
    g_temp_label = make_label(screen, 20, 258, 170, 28, "本地 --.-℃");
    g_humi_label = make_label(screen, 205, 258, 170, 28, "湿度 --.-%");
    g_weather_city_label = make_label(screen, 20, 216, 140, 28, "城市 --");
    g_weather_info_label = make_label(screen, 166, 216, 174, 28, "天气等待");
    g_weather_icon_label = make_label(screen, 344, 204, 42, 44, "");
    lv_obj_set_style_text_font(g_weather_icon_label, &qweather_icons_36, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_weather_icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
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

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    lv_obj_t *bottom_line = make_bar(screen, 18, 184, 364, 4);
    set_obj_black(top_line, true);
    set_obj_black(bottom_line, true);
}

static void update_time_ui(const struct tm &local)
{
    static int last_second = -1;
    static int last_minute = -1;
    int minute_key = local.tm_hour * 60 + local.tm_min;
    if (minute_key != last_minute) {
        last_minute = minute_key;
        draw_time_canvas(local);
    }
    if (local.tm_sec != last_second) {
        last_second = local.tm_sec;
        draw_second_canvas(local);
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

    show_boot_screen();
    uint32_t boot_start = SDL_GetTicks();
    uint32_t boot_last_tick = boot_start;
    uint32_t boot_last_frame_tick = boot_start;
    int boot_anim_frame = 0;
    while (SDL_GetTicks() - boot_start < 5700) {
        uint32_t now_tick = SDL_GetTicks();
        lv_tick_inc(now_tick - boot_last_tick);
        boot_last_tick = now_tick;
        if (now_tick - boot_last_frame_tick >= kBootAnimFrameMs) {
            boot_last_frame_tick = now_tick;
            boot_anim_frame = (boot_anim_frame + 1) % BOOT_ANIM_FRAME_COUNT;
        }
        draw_boot_anim_frame_index(boot_anim_frame);
        lv_timer_handler();
        SDL_Delay(30);
    }
    lv_obj_clean(lv_scr_act());
    g_boot_anim_canvas = nullptr;
    build_clock_ui();
    set_label_text_if_changed(g_temp_label, "本地 24.6℃");
    set_label_text_if_changed(g_humi_label, "湿度 58.0%");
    set_label_text_if_changed(g_weather_city_label, "城市 杭州");
    set_label_text_if_changed(g_weather_info_label, "晴 26℃ 58%");
    set_label_text_if_changed(g_weather_icon_label, weather_icon_text("100"));
    update_battery_icon(76);

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

        time_t now = time(nullptr);
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
