// 运行天气时钟 LVGL SDL 预览并生成各页面截图。
#include "sdl_preview_sample_data.h"
#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "sdl_preview_backend.h"
#include "sdl_preview_boot.h"
#include "sdl_preview_calendar.h"
#include "sdl_preview_clock.h"
#include "sdl_preview_flip_clock.h"
#include "ui_aggregate_clock_view.h"
#include "qweather_icons.h"
#include "sdl_preview_gallery.h"
#include "sdl_preview_history.h"
#include "sdl_preview_mode.h"
#include "sdl_preview_progress.h"
#include "sdl_preview_settings.h"
#include "sdl_preview_weather.h"
#include "sdl_preview_widgets.h"
#include "sdl_preview_work_status.h"
#include "sdl_preview_xiaozhi.h"
#include "ui_work_page_layout.h"

using sdl_preview_widgets::make_black_bar;
using sdl_preview_widgets::make_label_with_font;

static constexpr int kDisplayWidth = 400;
static constexpr int kDisplayHeight = 300;
static constexpr int kWindowScale = 2;
static const char *APP_VERSION = "v1.6.4";

static SdlPreviewBackend g_sdl_preview(kDisplayWidth, kDisplayHeight);
static sdl_preview_progress::Canvas g_work_page_day_progress;
static sdl_preview_work_status::Bar g_work_status;
static SdlPreviewClock g_clock(g_work_status);

static time_t preview_time();

static void build_history_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    g_work_status.build(screen, local);
    make_black_bar(
        screen,
        ui_work_page_layout::kTopSeparatorX,
        ui_work_page_layout::kTopSeparatorY,
        ui_work_page_layout::kTopSeparatorWidth,
        ui_work_page_layout::kTopSeparatorHeight);
    g_work_page_day_progress.build_day(screen, local, 59);

    build_history_preview_body(screen, &local);
    g_clock.update_time(local);
}

static void build_gallery_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    g_work_status.build(screen, local, false);
    g_work_status.update_date(local);

    make_black_bar(
        screen,
        ui_work_page_layout::kTopSeparatorX,
        ui_work_page_layout::kTopSeparatorY,
        ui_work_page_layout::kTopSeparatorWidth,
        ui_work_page_layout::kTopSeparatorHeight);
    g_work_page_day_progress.build_day(screen, local, 59);

    build_gallery_preview_body(screen, &local);

    g_clock.update_time(local);
}

static void build_calendar_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    g_work_status.build(screen, local);
    make_black_bar(
        screen,
        ui_work_page_layout::kTopSeparatorX,
        ui_work_page_layout::kTopSeparatorY,
        ui_work_page_layout::kTopSeparatorWidth,
        ui_work_page_layout::kTopSeparatorHeight);
    g_work_page_day_progress.build_day(screen, local, 59);

    build_calendar_preview_body(screen, &local);
    g_clock.update_time(local);
}

static void build_weather_board_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    g_work_status.build(screen, local);
    make_black_bar(
        screen,
        ui_work_page_layout::kTopSeparatorX,
        ui_work_page_layout::kTopSeparatorY,
        ui_work_page_layout::kTopSeparatorWidth,
        ui_work_page_layout::kTopSeparatorHeight);
    g_work_page_day_progress.build_day(screen, local, 59);

    build_weather_board_preview_body(screen);
    g_clock.update_time(local);
}

static void build_flip_clock_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    g_work_status.build(screen, local, false, false);
    g_work_status.update_date(local);

    make_black_bar(
        screen,
        ui_work_page_layout::kTopSeparatorX,
        ui_work_page_layout::kTopSeparatorY,
        ui_work_page_layout::kTopSeparatorWidth,
        ui_work_page_layout::kTopSeparatorHeight);
    g_work_page_day_progress.build_day(screen, local, 59);
    build_flip_clock_preview_body(screen, &local);
}

static void build_aggregate_clock_preview_ui()
{
    lv_obj_t *screen=lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen,lv_color_white(),0);
    lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    time_t now=preview_time(); struct tm local={}; localtime_r(&now,&local);
    g_work_status.build(screen,local,false,false); g_work_status.update_date(local);
    make_black_bar(screen,18,54,364,4);
    g_work_page_day_progress.build_day(screen,local,59);
    static lv_color_t pixels[3][kAggregateDigitWidth*kAggregateDigitHeight];
    lv_color_t *buffers[3]={pixels[0],pixels[1],pixels[2]};
    static AggregateClockView view;
    aggregate_clock_view_build(screen,view,buffers);
    aggregate_clock_view_time(view,local.tm_hour,local.tm_min,local.tm_sec);
    aggregate_clock_set_text(view.city,kPreviewCityLabel);
    aggregate_clock_set_text(view.weather,"多云");
    aggregate_clock_set_text(view.icon,weather_icon_text("101").c_str());
    aggregate_clock_set_text(view.temperature,"26 C");
    aggregate_clock_set_text(view.range,"最高 29 C  最低 22 C");
    aggregate_clock_set_text(view.day,"8");
    aggregate_clock_set_text(view.month,"五月");
    aggregate_clock_set_text(view.lunar,"初八");
    aggregate_clock_set_text(view.local_temp,"25.6 C");
    aggregate_clock_set_text(view.humidity,"58%");
    const char *theme=getenv("WEATHER_CLOCK_SDL_WEATHER_THEME");
    aggregate_clock_weather_theme(view,0);
    if(theme && (strcmp(theme,"day")==0 || strcmp(theme,"rain")==0 || strcmp(theme,"snow")==0)) {
        const bool rain=strcmp(theme,"rain")==0;
        const bool snow=strcmp(theme,"snow")==0;
        if(snow) {
            aggregate_clock_set_text(view.temperature,"-2 C");
            aggregate_clock_set_text(view.range,"最高 1 C  最低 -5 C");
        }
        aggregate_clock_weather_theme(view,snow?3:rain?2:1);
        aggregate_clock_set_text(view.icon,weather_icon_text(snow?"400":rain?"305":"100").c_str());
        aggregate_clock_set_text(view.weather,snow?"小雪":rain?"小雨":"晴");
    }
}

static void build_xiaozhi_preview_ui(const char *preview_mode)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    XiaozhiPreviewMode mode = classify_xiaozhi_preview_mode(preview_mode);
    g_work_status.build(screen, local, mode.pomodoro_visible(), true);

    make_black_bar(
        screen,
        ui_work_page_layout::kTopSeparatorX,
        ui_work_page_layout::kTopSeparatorY,
        ui_work_page_layout::kTopSeparatorWidth,
        ui_work_page_layout::kTopSeparatorHeight);
    g_work_page_day_progress.build_day(screen, local, 59);
    build_xiaozhi_preview_body(screen, &local, mode);
}

static void build_info_preview_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label_with_font(screen, 24, 18, 352, 26, "SYSTEM INFO", &lv_font_montserrat_16);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    make_black_bar(screen, 24, 50, 352, 3);

    static const char *const info_lines[] = {
        "Last NTP: 2026-07-01 09:30",
        "WiFi: HomeWiFi",
        "Last Weather: 2026-07-01 10:00",
        "Battery: 76%  4.05V",
        "Version: v1.4.40 / 2026-07-01",
        "Source: github.com/wickenzh/ESP32-S3-RLCD-4.2",
    };
    static const int info_y[] = {70, 104, 138, 172, 206, 276};
    for (size_t i = 0; i < sizeof(info_lines) / sizeof(info_lines[0]); ++i) {
        const bool source_line = i == (sizeof(info_lines) / sizeof(info_lines[0])) - 1;
        lv_obj_t *label = make_label_with_font(screen,
                                               source_line ? 0 : 30,
                                               info_y[i],
                                               source_line ? 400 : 340,
                                               source_line ? 18 : 24,
                                               info_lines[i],
                                               source_line ? &lv_font_montserrat_12 : &lv_font_montserrat_14);
        if (source_line) {
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        }
    }

    make_black_bar(screen, 24, 238, 352, 3);
    lv_obj_t *return_label = make_label_with_font(screen, 24, 252, 352, 22, "Hold KEY to return", &lv_font_montserrat_14);
    lv_obj_set_style_text_align(return_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    sdl_preview_backend_flush(&g_sdl_preview, drv, area, color_p);
}

static time_t preview_time()
{
    const char *fixed = getenv("WEATHER_CLOCK_SDL_FIXED_TIME");
    if (fixed && fixed[0]) {
        return (time_t)atoll(fixed);
    }
    return time(nullptr);
}

static void init_lvgl_preview_display()
{
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
}

static void settle_preview_frame()
{
    for (int i = 0; i < 5; ++i) {
        lv_tick_inc(16);
        lv_timer_handler();
        SDL_Delay(16);
    }
}

static sdl_preview_mode::Selection build_selected_preview(const char *preview_mode)
{
    sdl_preview_mode::Selection selection =
        sdl_preview_mode::selection_for(preview_mode);
    if (selection.history) {
        build_history_preview_ui();
    } else if (selection.gallery) {
        build_gallery_preview_ui();
    } else if (selection.aggregate_clock) {
        build_aggregate_clock_preview_ui();
    } else if (selection.flip_clock) {
        build_flip_clock_preview_ui();
    } else if (selection.xiaozhi) {
        build_xiaozhi_preview_ui(preview_mode);
    } else if (selection.calendar) {
        build_calendar_preview_ui();
    } else if (selection.weather_board) {
        build_weather_board_preview_ui();
    } else if (selection.info) {
        build_info_preview_ui();
    } else {
        g_clock.build(lv_scr_act());
        g_clock.populate_sample_data();
    }
    return selection;
}

static void apply_screenshot_preview_state(const char *preview_mode,
                                           const sdl_preview_mode::Selection &selection)
{
    if (selection.alternate_work_page()) {
            // Alternate work pages are already built above.
    } else if (sdl_preview_mode::is_settings(preview_mode)) {
        build_settings_preview_page(preview_mode);
    } else if (sdl_preview_mode::is(preview_mode, "setup")) {
        g_clock.show_setup_status();
    } else if (sdl_preview_mode::is(preview_mode, "alert")) {
        g_clock.apply_alert(true);
    } else if (sdl_preview_mode::is(preview_mode, "low")) {
        g_clock.update_battery(4);
        g_clock.apply_low_battery(true);
    }

    time_t now = preview_time();
    struct tm local;
    localtime_r(&now, &local);
    if (!selection.alternate_work_page() &&
        !sdl_preview_mode::is_settings(preview_mode)) {
        g_clock.update_time(local);
        if (sdl_preview_mode::is(preview_mode, "low")) {
            g_clock.apply_low_battery(true);
        }
    }
}

static bool save_preview_if_requested(const char *screenshot_path,
                                      const char *preview_mode,
                                      const sdl_preview_mode::Selection &selection)
{
    if (!screenshot_path || !screenshot_path[0]) {
        return false;
    }
    apply_screenshot_preview_state(preview_mode, selection);
    settle_preview_frame();
    sdl_preview_backend_save_ppm(&g_sdl_preview, screenshot_path);
    sdl_preview_backend_cleanup(&g_sdl_preview);
    return true;
}

static void run_interactive_preview()
{
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

        time_t now = preview_time();
        if (now != last_sec) {
            last_sec = now;
            struct tm local = {};
            localtime_r(&now, &local);
            g_clock.update_time(local);
        }

        lv_timer_handler();
        SDL_Delay(5);
    }
}

int main(int, char **)
{
    if (!sdl_preview_backend_init(&g_sdl_preview,
                                  "WeatherClock LVGL SDL Preview",
                                  kWindowScale)) {
        return 1;
    }

    init_lvgl_preview_display();
    const char *screenshot_path = getenv("WEATHER_CLOCK_SDL_SCREENSHOT");
    const char *preview_mode = getenv("WEATHER_CLOCK_SDL_MODE");

    build_boot_preview_screen(APP_VERSION);
    if (save_boot_preview_if_requested(&g_sdl_preview, screenshot_path, preview_mode)) {
        return 0;
    }
    run_boot_preview_animation();
    sdl_preview_mode::Selection selection = build_selected_preview(preview_mode);
    if (save_preview_if_requested(screenshot_path, preview_mode, selection)) {
        return 0;
    }

    run_interactive_preview();

    sdl_preview_backend_cleanup(&g_sdl_preview);
    return 0;
}
