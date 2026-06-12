#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "lvgl.h"

LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_font_18);

static constexpr int kDisplayWidth = 400;
static constexpr int kDisplayHeight = 300;
static constexpr int kWindowScale = 2;

struct SegDigit {
    lv_obj_t *seg[7] = {};
};

static SDL_Window *g_window = nullptr;
static SDL_Renderer *g_renderer = nullptr;
static SDL_Texture *g_texture = nullptr;
static std::vector<uint32_t> g_framebuffer(kDisplayWidth * kDisplayHeight, 0xFFFFFFFF);

static lv_obj_t *g_date_label;
static lv_obj_t *g_week_label;
static lv_obj_t *g_temp_label;
static lv_obj_t *g_humi_label;
static lv_obj_t *g_weather_city_label;
static lv_obj_t *g_weather_info_label;
static lv_obj_t *g_weather_icon_label;
static lv_obj_t *g_wifi_label;
static lv_obj_t *g_sync_label;
static lv_obj_t *g_battery_fill;
static lv_obj_t *g_colon1[2];
static lv_obj_t *g_colon2[2];
static SegDigit g_digits[6];

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

static SegDigit make_digit(lv_obj_t *parent, int x, int y, int w, int h, int t)
{
    SegDigit digit;
    digit.seg[0] = make_bar(parent, x + t, y, w - 2 * t, t);
    digit.seg[1] = make_bar(parent, x + w - t, y + t, t, h / 2 - t);
    digit.seg[2] = make_bar(parent, x + w - t, y + h / 2, t, h / 2 - t);
    digit.seg[3] = make_bar(parent, x + t, y + h - t, w - 2 * t, t);
    digit.seg[4] = make_bar(parent, x, y + h / 2, t, h / 2 - t);
    digit.seg[5] = make_bar(parent, x, y + t, t, h / 2 - t);
    digit.seg[6] = make_bar(parent, x + t, y + h / 2 - t / 2, w - 2 * t, t);
    return digit;
}

static void set_digit(SegDigit &digit, int value)
{
    static const bool map[10][7] = {
        {true, true, true, true, true, true, false},
        {false, true, true, false, false, false, false},
        {true, true, false, true, true, false, true},
        {true, true, true, true, false, false, true},
        {false, true, true, false, false, true, true},
        {true, false, true, true, false, true, true},
        {true, false, true, true, true, true, true},
        {true, true, true, false, false, false, false},
        {true, true, true, true, true, true, true},
        {true, true, true, true, false, true, true},
    };
    for (int i = 0; i < 7; ++i) {
        set_obj_black(digit.seg[i], value >= 0 && value <= 9 && map[value][i]);
    }
}

static void set_colon(lv_obj_t *dots[2], bool active)
{
    set_obj_black(dots[0], active);
    set_obj_black(dots[1], active);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &zh_font_18, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
    return label;
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (current == nullptr || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void style_battery_part(lv_obj_t *obj, bool filled)
{
    lv_obj_set_style_bg_color(obj, filled ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void style_battery_frame(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void build_battery_icon(lv_obj_t *parent)
{
    lv_obj_t *frame = lv_obj_create(parent);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(frame, 320, 18);
    lv_obj_set_size(frame, 46, 20);
    style_battery_frame(frame);

    lv_obj_t *tip = lv_obj_create(parent);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tip, 368, 24);
    lv_obj_set_size(tip, 4, 8);
    style_battery_part(tip, true);
    lv_obj_set_style_radius(tip, 2, LV_PART_MAIN);

    g_battery_fill = lv_obj_create(frame);
    lv_obj_clear_flag(g_battery_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_battery_fill, 3, 3);
    lv_obj_set_size(g_battery_fill, 0, 14);
    style_battery_part(g_battery_fill, true);
    lv_obj_set_style_border_width(g_battery_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_battery_fill, 3, LV_PART_MAIN);
}

static void update_battery_icon(int percent)
{
    int filled = percent;
    if (percent >= 0) {
        if (percent > 100) percent = 100;
        filled = percent;
    } else {
        filled = 0;
    }
    int width = (filled * 40 + 99) / 100;
    if (filled > 0 && width < 2) {
        width = 2;
    }
    lv_obj_set_width(g_battery_fill, width);
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

    g_date_label = make_label(screen, 20, 16, 198, 30, "----/--/--");
    g_week_label = make_label(screen, 242, 16, 68, 30, "---");
    build_battery_icon(screen);
    g_temp_label = make_label(screen, 20, 232, 160, 26, "本地 --.-℃");
    g_humi_label = make_label(screen, 200, 232, 160, 26, "湿度 --.-%");
    g_weather_city_label = make_label(screen, 20, 202, 126, 26, "城市 --");
    g_weather_info_label = make_label(screen, 150, 202, 190, 26, "天气等待");
    g_weather_icon_label = make_label(screen, 344, 186, 42, 44, "");
    lv_obj_set_style_text_font(g_weather_icon_label, &qweather_icons_36, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_weather_icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_wifi_label = make_label(screen, 20, 270, 230, 26, "SDL PREVIEW");
    g_sync_label = make_label(screen, 265, 270, 120, 26, "NTP OK");

    const int y = 74;
    const int w = 42;
    const int h = 100;
    const int t = 8;
    const int gap = 8;
    int x = 19;
    g_digits[0] = make_digit(screen, x, y, w, h, t);
    x += w + gap;
    g_digits[1] = make_digit(screen, x, y, w, h, t);
    x += w + 12;
    g_colon1[0] = make_bar(screen, x, y + 28, 8, 8);
    g_colon1[1] = make_bar(screen, x, y + 64, 8, 8);
    x += 20;
    g_digits[2] = make_digit(screen, x, y, w, h, t);
    x += w + gap;
    g_digits[3] = make_digit(screen, x, y, w, h, t);
    x += w + 12;
    g_colon2[0] = make_bar(screen, x, y + 28, 8, 8);
    g_colon2[1] = make_bar(screen, x, y + 64, 8, 8);
    x += 20;
    g_digits[4] = make_digit(screen, x, y, w, h, t);
    x += w + gap;
    g_digits[5] = make_digit(screen, x, y, w, h, t);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    lv_obj_t *bottom_line = make_bar(screen, 18, 180, 364, 4);
    set_obj_black(top_line, true);
    set_obj_black(bottom_line, true);
}

static void update_time_ui(const struct tm &local)
{
    int values[6] = {
        local.tm_hour / 10, local.tm_hour % 10,
        local.tm_min / 10, local.tm_min % 10,
        local.tm_sec / 10, local.tm_sec % 10,
    };
    for (int i = 0; i < 6; ++i) set_digit(g_digits[i], values[i]);
    bool colon_on = (local.tm_sec % 2) == 0;
    set_colon(g_colon1, colon_on);
    set_colon(g_colon2, colon_on);

    char date[32];
    snprintf(date, sizeof(date), "%04d/%02d/%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    set_label_text_if_changed(g_date_label, date);

    static const char *week_days[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    set_label_text_if_changed(g_week_label, week_days[local.tm_wday]);
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
