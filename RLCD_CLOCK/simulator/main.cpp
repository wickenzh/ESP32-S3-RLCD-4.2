// 运行天气时钟 LVGL SDL 预览并生成各页面截图。
#include "sdl_preview_sample_data.h"
#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef WEATHER_CLOCK_WEB_SIMULATOR
#include <emscripten.h>
#include <new>
#include "web_demo_state.h"
#include "web_build_info.h"
#endif

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
#ifdef WEATHER_CLOCK_WEB_SIMULATOR
static constexpr int kWindowScale = 1;
static time_t g_web_time = 0;
#else
static constexpr int kWindowScale = 2;
#endif
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

static void build_xiaozhi_preview_ui(const char *preview_mode, const XiaozhiPreviewMode *override_mode = nullptr)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    time_t now = preview_time();
    struct tm local = {};
    localtime_r(&now, &local);
    XiaozhiPreviewMode mode = override_mode ? *override_mode : classify_xiaozhi_preview_mode(preview_mode);
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
#ifdef WEATHER_CLOCK_WEB_SIMULATOR
    return g_web_time;
#else
    const char *fixed = getenv("WEATHER_CLOCK_SDL_FIXED_TIME");
    if (fixed && fixed[0]) {
        return (time_t)atoll(fixed);
    }
    return time(nullptr);
#endif
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

#ifdef WEATHER_CLOCK_WEB_SIMULATOR
static WebDemoState g_demo;
static double g_demo_now = 0;
static time_t g_demo_second = -1;
static const char *kDemoModes[] = {"main","gallery","weather_board","flip_clock","calendar","history","xiaozhi","aggregate_clock"};

static void render_web_demo()
{
    // 删除LVGL对象后再释放其像素缓冲，防止切页访问上一次画布。
    lv_obj_clean(lv_scr_act());
    g_clock.~SdlPreviewClock();
    new (&g_clock) SdlPreviewClock(g_work_status);
    g_work_status.~Bar();
    new (&g_work_status) sdl_preview_work_status::Bar();
    g_work_page_day_progress.~Canvas();
    new (&g_work_page_day_progress) sdl_preview_progress::Canvas();
    auto *root=lv_scr_act();
    lv_obj_set_style_bg_color(root,lv_color_white(),0);
    lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    if(g_demo.scene==WebDemoState::Settings || g_demo.scene==WebDemoState::Pages || g_demo.scene==WebDemoState::Order) {
        build_web_settings_page(g_demo,WEB_FIRMWARE_VERSION);
    } else if(g_demo.scene==WebDemoState::Work) {
        if(g_demo.weather) setenv("WEATHER_CLOCK_SDL_WEATHER_THEME",g_demo.weather==1?"day":g_demo.weather==2?"rain":"snow",1);
        else unsetenv("WEATHER_CLOCK_SDL_WEATHER_THEME");
        const char *mode=kDemoModes[g_demo.page];
        if(g_demo.page==6) {
            auto xi=classify_xiaozhi_preview_mode("xiaozhi");
            xi.pomodoro_running=g_demo.pomodoro_until>0;
            xi.pomodoro_completed=g_demo.pomodoro_completed;
            xi.remaining_seconds=xi.pomodoro_running?static_cast<int>((g_demo.pomodoro_until-g_demo_now+999)/1000):0;
            const char *states[]={"待命（模拟）","聆听中（模拟）","思考中（模拟）","正在说话（模拟）"};
            const char *messages[]={"你好，今天也要保持好心情。","正在接收演示对话...","正在准备演示回复...","当地今天多云，适合散步。记得带好饮用水。"};
            xi.status=states[g_demo.conversation];xi.subtitle=messages[g_demo.conversation];
            build_xiaozhi_preview_ui(mode,&xi);
        } else build_selected_preview(mode);
        if(g_demo.page==0) {
            struct tm local={};localtime_r(&g_web_time,&local);g_clock.update_time(local);
        }
    } else if(g_demo.scene==WebDemoState::Setup || g_demo.scene==WebDemoState::Alert || g_demo.scene==WebDemoState::Low) {
        g_clock.build(root);g_clock.populate_sample_data();
        struct tm local={};localtime_r(&g_web_time,&local);g_clock.update_time(local);
        if(g_demo.scene==WebDemoState::Setup)g_clock.show_setup_status();
        if(g_demo.scene==WebDemoState::Alert)g_clock.apply_alert(true);
        if(g_demo.scene==WebDemoState::Low){g_clock.update_battery(4);g_clock.apply_low_battery(true);}
    } else if(g_demo.scene==WebDemoState::Boot) {
        build_boot_preview_screen(WEB_FIRMWARE_VERSION);
    } else {
        const char *title=g_demo.scene==WebDemoState::Info?"关于本机":g_demo.scene==WebDemoState::Diagnostics?"网络检测":"检查更新";
        auto *heading=sdl_preview_widgets::make_label(root,24,18,352,28,title);
        lv_obj_set_style_text_align(heading,LV_TEXT_ALIGN_CENTER,0);
        make_black_bar(root,24,52,352,3);
        char body[512];
        if(g_demo.scene==WebDemoState::Info) {
            std::snprintf(body,sizeof(body),"WeatherClock %s\nWi-Fi: Demo-WiFi\nBattery: 76%%  4.05V\n400 x 300 / ESP32-S3\nLocal simulation",WEB_FIRMWARE_VERSION);
            make_label_with_font(root,24,68,352,180,body,&lv_font_montserrat_16);
        } else if(g_demo.scene==WebDemoState::Diagnostics) {
            const char *items[]={"本地网络","公网地址","定位服务","DNS","和风天气","时间同步","每日文字","公网连接","OTA清单"};
            const int completed=g_demo.progress*9/100;
            for(int i=0;i<9;++i) {
                sdl_preview_widgets::make_label(root,30,64+i*20,190,20,items[i]);
                sdl_preview_widgets::make_label(root,238,64+i*20,130,20,i<completed?(g_demo.offline?"不可用":"通过"):"等待");
            }
            auto *label=sdl_preview_widgets::make_label(root,24,248,352,20,"检测结果为模拟数据");lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
        } else {
            std::snprintf(body,sizeof(body),"%s\n\n%s\n\n%d%%",g_demo.scene==WebDemoState::Diagnostics?"网络检测（模拟数据）":"固件更新（模拟数据）",g_demo.feedback,g_demo.progress);
            auto *label=sdl_preview_widgets::make_label(root,24,68,352,180,body);
            lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
        }
        auto *hint=sdl_preview_widgets::make_label(root,24,270,352,22,"长按 KEY 返回");lv_obj_set_style_text_align(hint,LV_TEXT_ALIGN_CENTER,0);
    }
    if(g_demo.scene==WebDemoState::Work) g_work_status.set_simulated_status(!g_demo.offline,g_demo.hourly||g_demo.all_day,g_demo.alarm);
    lv_refr_now(nullptr);
    g_demo.dirty=false;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE const char *demo_build_info() { return WEB_BUILD_INFO; }
EMSCRIPTEN_KEEPALIVE void demo_tick(double epoch, double now) {
    g_web_time=static_cast<time_t>(epoch);
    const auto delta=static_cast<uint32_t>(now>g_demo_now?now-g_demo_now:0);
    g_demo_now=now;
    lv_tick_inc(delta>1000?1000:delta);
    g_demo.advance(now);
    if(g_demo.dirty || (g_demo.scene==WebDemoState::Work && g_demo_second!=g_web_time)) render_web_demo();
    g_demo_second=g_web_time;
    lv_timer_handler();
}
EMSCRIPTEN_KEEPALIVE void demo_key(int key, int held) {
    if(key!=0&&key!=1)return;
    g_demo.press(key,held!=0,g_demo_now);
    render_web_demo();
}
EMSCRIPTEN_KEEPALIVE void demo_reset() {g_demo.reset(g_demo_now);render_web_demo();}
EMSCRIPTEN_KEEPALIVE void demo_scene(int scene) {
    if(scene>=0&&scene<8){if(g_demo.enabled&(1<<scene)){g_demo.work(g_demo_now);g_demo.page=scene;}}
    else if(scene==8)g_demo.settings(g_demo_now);
    else if(scene>=9&&scene<=15){
        const WebDemoState::Scene scenes[]={WebDemoState::Info,WebDemoState::Diagnostics,WebDemoState::Ota,WebDemoState::Setup,WebDemoState::Alert,WebDemoState::Low,WebDemoState::Boot};
        g_demo.work(g_demo_now);g_demo.scene=scenes[scene-9];
        if(scene==10)g_demo.start_operation("正在网络检测...",g_demo_now,3000);
    } else if(scene>=20&&scene<=23){g_demo.work(g_demo_now);g_demo.page=7;g_demo.weather=scene-20;g_demo.dirty=true;}
    else if(scene==30){g_demo.page=6;g_demo.work(g_demo_now);g_demo.conversation=1;g_demo.conversation_until=g_demo_now+1000;}
    else if(scene==31){g_demo.page=6;g_demo.work(g_demo_now);g_demo.pomodoro_completed=false;g_demo.pomodoro_until=g_demo.pomodoro_until?0:g_demo_now+25*60*1000;}
    render_web_demo();
}
EMSCRIPTEN_KEEPALIVE const char *demo_state() {
    static char json[384];
    lv_mem_monitor_t memory; lv_mem_monitor(&memory);
    std::snprintf(json,sizeof(json),"{\"page\":%d,\"scene\":%d,\"primary\":%d,\"selection\":%d,\"secondary\":%s,\"enabled\":%u,\"volume\":%d,\"offline\":%s,\"progress\":%d,\"freeMemory\":%u,\"clock\":%ld}",g_demo.page,g_demo.scene,g_demo.primary,g_demo.selection,g_demo.secondary?"true":"false",g_demo.enabled,g_demo.volume,g_demo.offline?"true":"false",g_demo.progress,static_cast<unsigned>(memory.free_size),static_cast<long>(g_web_time));
    return json;
}
}

int main() {
    if(!sdl_preview_backend_init(&g_sdl_preview,"WeatherClock",1))return 1;
    init_lvgl_preview_display();
    g_web_time=time(nullptr);
    render_web_demo();
    return 0;
}
#else
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
#endif
