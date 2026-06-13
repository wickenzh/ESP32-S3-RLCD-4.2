#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "miniz.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "display_bsp.h"
#include "i2c_bsp.h"
#include "i2c_equipment.h"
#include "lvgl_bsp.h"
#include "dseg_digits.h"
#include "boot_anim.h"
#include "status_gif_60.h"
#include "ui_icons.h"

LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_font_16);

static const char *TAG = "WeatherClock";
static const char *APP_VERSION = "v0.0.59";

static constexpr int kDisplayWidth = 400;
static constexpr int kDisplayHeight = 300;
static constexpr int kWifiConnectedBit = BIT0;
static constexpr int kTimeSyncedBit = BIT1;
static constexpr int kWeatherReadyBit = BIT2;
static constexpr int kProvisioningSyncBit = BIT3;
static constexpr int kManualNtpSyncBit = BIT4;
static constexpr int kManualWeatherSyncBit = BIT5;
static constexpr gpio_num_t kBootButtonGpio = GPIO_NUM_0;
static constexpr gpio_num_t kKeyButtonGpio = GPIO_NUM_18;
static constexpr const char *kSetupApPassword = "12345678";
static constexpr int kBootInfoHoldMs = 5000;
static constexpr int kBootSetupHoldMs = 20000;
static constexpr int kSettingsHoldMs = 2000;
static constexpr int kSettingsTimeoutMs = 5000;
static constexpr int kSettingsManualSyncTimeoutMs = 60000;
static constexpr int kButtonIdlePollMs = 250;
static constexpr int kButtonActivePollMs = 100;
static constexpr int kButtonPressedPollMs = 50;
static constexpr int kBootAnimRunFrameMs = 50;
static constexpr int kBootWifiConnectTimeoutMs = 5000;
static constexpr int kBootNtpRetries = 2;
static constexpr int kBootStartupBudgetMs = 6000;
static constexpr int kHttpDefaultTimeoutMs = 10000;
static constexpr int kHttpBootTimeoutMs = 2500;
static constexpr int kMinValidYear = 2024;
static constexpr int kMaxValidYear = 2035;
static constexpr int kLowBatteryEnterPercent = 5;
static constexpr int kLowBatteryExitPercent = 8;
static constexpr int kDisplayPartialMaxWidth = (kDisplayWidth * 7) / 10;
static constexpr int kMaxFlushRanges = 8;
static constexpr int kFlushRangeMergeGap = 8;

enum SettingsSyncOp {
    kSettingsSyncNone = 0,
    kSettingsSyncNtp = 1,
    kSettingsSyncWeather = 2,
};

static DisplayPort g_display(12, 11, 5, 40, 41, kDisplayWidth, kDisplayHeight);
static I2cMasterBus g_i2c(14, 13, 0);
static Shtc3Port *g_shtc3 = nullptr;
static adc_oneshot_unit_handle_t g_battery_adc = nullptr;
static adc_cali_handle_t g_battery_adc_cali = nullptr;
static bool g_battery_adc_ready = false;
static bool g_battery_adc_cali_ready = false;
static EventGroupHandle_t g_app_events;
static httpd_handle_t g_http_server = nullptr;

static char g_wifi_ssid[33] = {};
static char g_wifi_pass[65] = {};
static char g_weather_api_key[96] = {};
static char g_ap_ssid[33] = {};
static char g_sta_ip[16] = {};
static bool g_have_wifi_creds = false;
static bool g_have_weather_key = false;
static bool g_hourly_chime_enabled = false;
static bool g_ntp_started = false;
static bool g_wifi_radio_on = false;
static bool g_wifi_stop_requested = false;
static bool g_setup_portal_active = false;
static int g_last_wifi_disconnect_reason = 0;
static int g_http_timeout_ms = kHttpDefaultTimeoutMs;
static int64_t g_boot_sync_deadline_us = 0;
static float g_temperature = 0.0f;
static float g_humidity = 0.0f;
static bool g_sensor_ok = false;
static int g_battery_percent = -1;
static float g_battery_voltage = -1.0f;
static uint32_t g_battery_version = 0;
static time_t g_last_ntp_sync_time = 0;
static time_t g_last_weather_sync_time = 0;
static volatile bool g_boot_info_requested = false;
static volatile bool g_settings_requested = false;
static volatile int g_settings_selection = 0;
static volatile uint32_t g_settings_action_seq = 0;
static volatile TickType_t g_settings_last_activity_tick = 0;
static volatile int g_settings_sync_op = kSettingsSyncNone;
static volatile TickType_t g_settings_sync_deadline_tick = 0;
static TickType_t g_settings_feedback_until_tick = 0;
static bool g_factory_reset_confirm_pending = false;
static char g_settings_feedback[48] = {};

struct WeatherData {
    char city[32] = {};
    char text[32] = {};
    char icon[8] = {};
    char temp[8] = {};
    char humidity[8] = {};
    char lat[16] = {};
    char lon[16] = {};
};

struct WeatherAlertData {
    bool active = false;
    char title[64] = {};
    char icon[8] = {};
    time_t updated_at = 0;
};

static WeatherData g_weather;
static WeatherAlertData g_weather_alert;
static bool g_low_battery_mode = false;

static lv_obj_t *g_clock_root;
static lv_obj_t *g_info_root;
static lv_obj_t *g_settings_root;
static lv_obj_t *g_date_label;
static lv_obj_t *g_temp_label;
static lv_obj_t *g_humi_label;
static lv_obj_t *g_weather_city_label;
static lv_obj_t *g_weather_info_label;
static lv_obj_t *g_weather_icon_label;
static lv_obj_t *g_weather_temp_label;
static lv_obj_t *g_weather_humi_label;
static lv_obj_t *g_alert_pill;
static lv_obj_t *g_alert_icon_canvas;
static lv_color_t *g_alert_icon_canvas_buf;
static lv_obj_t *g_alert_label;
static lv_obj_t *g_low_battery_icon_canvas;
static lv_color_t *g_low_battery_icon_canvas_buf;
static lv_obj_t *g_panel_sep_a;
static lv_obj_t *g_panel_sep_b;
static lv_obj_t *g_battery_segments[5];
static lv_obj_t *g_time_canvas;
static lv_color_t *g_time_canvas_buf;
static lv_obj_t *g_second_canvas;
static lv_color_t *g_second_canvas_buf;
static lv_obj_t *g_status_gif_canvas;
static lv_color_t *g_status_gif_canvas_buf;
static lv_obj_t *g_day_progress_canvas;
static lv_color_t *g_day_progress_canvas_buf;
static lv_obj_t *g_second_progress_canvas;
static lv_color_t *g_second_progress_canvas_buf;
static lv_obj_t *g_lower_panel_objects[11];
static lv_obj_t *g_setup_status_labels[6];
static lv_obj_t *g_boot_status_label;
static lv_obj_t *g_boot_detail_label;
static lv_obj_t *g_boot_anim_canvas;
static lv_color_t *g_boot_anim_canvas_buf;
static lv_obj_t *g_info_labels[5];
static lv_obj_t *g_settings_labels[4];
static lv_obj_t *g_settings_feedback_label;
static volatile bool g_boot_anim_running = false;
static volatile int g_boot_anim_current_frame = 0;
static TaskHandle_t g_boot_anim_task_handle = nullptr;
static TaskHandle_t g_boot_sync_task_handle = nullptr;
static int g_last_ui_second = -1;
static int g_last_ui_minute = -1;
static int g_last_day_progress_filled = -1;
static int g_last_second_progress_filled = -1;
static int g_last_status_gif_frame = -1;

static void apply_station_config(bool reconnect);
static bool wait_for_wifi_connected(uint32_t timeout_ms);
static void trim_ascii(char *text);
static void build_clock_ui();
static void run_boot_connectivity_sync();

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

static constexpr int kProgressSegmentCount = 60;
static constexpr int kProgressSegmentW = 5;
static constexpr int kProgressSegmentH = 3;
static constexpr int kProgressSegmentGap = 1;
static constexpr int kProgressCanvasW = kProgressSegmentCount * kProgressSegmentW + (kProgressSegmentCount - 1) * kProgressSegmentGap;
static constexpr int kProgressCanvasH = kProgressSegmentH;

static void draw_progress_segment(lv_obj_t *canvas, int index, bool filled)
{
    if (!canvas || index < 0 || index >= kProgressSegmentCount) {
        return;
    }
    int x0 = index * (kProgressSegmentW + kProgressSegmentGap);
    for (int y = 0; y < kProgressSegmentH; ++y) {
        for (int x = 0; x < kProgressSegmentW; ++x) {
            bool border = x == 0 || x == kProgressSegmentW - 1 || y == 0 || y == kProgressSegmentH - 1;
            lv_canvas_set_px_color(canvas, x0 + x, y, (filled || border) ? lv_color_black() : lv_color_white());
        }
    }
}

static void invalidate_progress_segment(lv_obj_t *canvas, int index)
{
    if (!canvas || index < 0 || index >= kProgressSegmentCount) {
        return;
    }
    int x0 = index * (kProgressSegmentW + kProgressSegmentGap);
    lv_area_t area = {};
    area.x1 = static_cast<lv_coord_t>(x0);
    area.y1 = 0;
    area.x2 = static_cast<lv_coord_t>(x0 + kProgressSegmentW - 1);
    area.y2 = static_cast<lv_coord_t>(kProgressSegmentH - 1);
    lv_obj_invalidate_area(canvas, &area);
}

static void build_progress_canvas(lv_obj_t *parent, lv_obj_t **canvas, lv_color_t **buf, int y)
{
    if (!*buf) {
        *buf = (lv_color_t *)heap_caps_calloc(kProgressCanvasW * kProgressCanvasH,
                                              sizeof(lv_color_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!*buf) {
            *buf = (lv_color_t *)calloc(kProgressCanvasW * kProgressCanvasH, sizeof(lv_color_t));
        }
    }
    *canvas = lv_canvas_create(parent);
    lv_obj_clear_flag(*canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(*canvas, 20, y);
    lv_obj_set_size(*canvas, kProgressCanvasW, kProgressCanvasH);
    lv_obj_set_style_border_width(*canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(*canvas, 0, LV_PART_MAIN);
    if (*buf) {
        lv_canvas_set_buffer(*canvas, *buf, kProgressCanvasW, kProgressCanvasH, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(*canvas, lv_color_white(), LV_OPA_COVER);
        for (int i = 0; i < kProgressSegmentCount; ++i) {
            draw_progress_segment(*canvas, i, false);
        }
        lv_obj_invalidate(*canvas);
    }
}

static void update_progress_canvas(lv_obj_t *canvas, int filled, int *last_filled)
{
    if (!canvas) {
        return;
    }
    if (filled < 0) {
        filled = 0;
    } else if (filled > kProgressSegmentCount) {
        filled = kProgressSegmentCount;
    }
    if (*last_filled < 0 || filled < *last_filled) {
        for (int i = 0; i < kProgressSegmentCount; ++i) {
            draw_progress_segment(canvas, i, i < filled);
        }
        lv_obj_invalidate(canvas);
        *last_filled = filled;
        return;
    }
    if (filled == *last_filled) {
        return;
    }
    for (int i = *last_filled; i < filled; ++i) {
        draw_progress_segment(canvas, i, true);
        invalidate_progress_segment(canvas, i);
    }
    *last_filled = filled;
}

static void draw_1bit_icon(lv_obj_t *canvas,
                           int width,
                           int height,
                           int bytes_per_row,
                           const uint8_t *bits,
                           lv_color_t fg,
                           lv_color_t bg)
{
    if (!canvas || !bits) {
        return;
    }
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = bits + y * bytes_per_row;
        for (int x = 0; x < width; ++x) {
            bool set = row[x / 8] & (0x80 >> (x & 7));
            if (set) {
                lv_canvas_set_px_color(canvas, x, y, fg);
            }
        }
    }
    lv_obj_invalidate(canvas);
}

static const DsegGlyph *find_dseg_glyph(const DsegFont &font, char ch)
{
    const char *pos = strchr(font.chars, ch);
    if (!pos) {
        return nullptr;
    }
    return &font.glyphs[pos - font.chars];
}

static int draw_dseg_text(lv_obj_t *canvas, const DsegFont &font, const char *text, int cursor_x, int baseline_y)
{
    int x_cursor = cursor_x;
    for (const char *p = text; *p; ++p) {
        const DsegGlyph *glyph = find_dseg_glyph(font, *p);
        if (!glyph) {
            continue;
        }
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
    if (!g_time_canvas) {
        return;
    }
    lv_canvas_fill_bg(g_time_canvas, lv_color_white(), LV_OPA_COVER);

    char hm[6];
    snprintf(hm, sizeof(hm), "%02d:%02d", local.tm_hour, local.tm_min);
    draw_dseg_text(g_time_canvas, kDSEG84Font, hm, 0, 88);
    lv_obj_invalidate(g_time_canvas);
}

static void draw_second_canvas(const struct tm &local)
{
    if (!g_second_canvas) {
        return;
    }
    lv_canvas_fill_bg(g_second_canvas, lv_color_white(), LV_OPA_COVER);
    char ss[3] = {
        (char)('0' + local.tm_sec / 10),
        (char)('0' + local.tm_sec % 10),
        '\0',
    };
    draw_dseg_text(g_second_canvas, kDSEG36Font, ss, 0, 40);
    lv_obj_invalidate(g_second_canvas);
}

static void draw_status_gif_frame(int frame)
{
    if (!g_status_gif_canvas) {
        return;
    }
    if (frame < 0) {
        frame = 0;
    } else if (frame >= STATUS_GIF_FRAME_COUNT) {
        frame = STATUS_GIF_FRAME_COUNT - 1;
    }
    const uint8_t *pixels = status_gif_frames[frame];
    const uint8_t *prev_pixels = g_last_status_gif_frame >= 0 ? status_gif_frames[g_last_status_gif_frame] : nullptr;
    uint32_t bit = 0;
    bool changed = false;
    for (int y = 0; y < STATUS_GIF_HEIGHT; ++y) {
        for (int x = 0; x < STATUS_GIF_WIDTH; ++x, ++bit) {
            bool black = pixels[bit / 8] & (0x80 >> (bit & 7));
            if (prev_pixels) {
                bool prev_black = prev_pixels[bit / 8] & (0x80 >> (bit & 7));
                if (black == prev_black) {
                    continue;
                }
            }
            lv_canvas_set_px_color(g_status_gif_canvas, x, y, black ? lv_color_black() : lv_color_white());
            changed = true;
        }
    }
    if (changed || g_last_status_gif_frame != frame) {
        lv_obj_invalidate(g_status_gif_canvas);
    }
    g_last_status_gif_frame = frame;
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

static lv_obj_t *create_page_root()
{
    lv_obj_t *root = lv_obj_create(lv_scr_act());
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, kDisplayWidth, kDisplayHeight);
    lv_obj_set_style_bg_color(root, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    return root;
}

static void set_page_visible(lv_obj_t *page, bool visible)
{
    if (!page) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page);
    } else {
        lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_page(lv_obj_t *page)
{
    set_page_visible(g_clock_root, page == g_clock_root);
    set_page_visible(g_info_root, page == g_info_root);
    set_page_visible(g_settings_root, page == g_settings_root);
}

static void format_time_or_dash(time_t value, char *out, size_t out_len)
{
    if (value <= 0) {
        strlcpy(out, "--", out_len);
        return;
    }
    struct tm local = {};
    localtime_r(&value, &local);
    if (local.tm_year + 1900 < 2024) {
        strlcpy(out, "--", out_len);
        return;
    }
    snprintf(out, out_len, "%04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900,
             local.tm_mon + 1,
             local.tm_mday,
             local.tm_hour,
             local.tm_min,
             local.tm_sec);
}

static void clear_clock_object_refs()
{
    g_clock_root = nullptr;
    g_date_label = nullptr;
    g_temp_label = nullptr;
    g_humi_label = nullptr;
    g_weather_city_label = nullptr;
    g_weather_info_label = nullptr;
    g_weather_icon_label = nullptr;
    g_weather_temp_label = nullptr;
    g_weather_humi_label = nullptr;
    g_alert_pill = nullptr;
    g_alert_icon_canvas = nullptr;
    g_alert_label = nullptr;
    g_low_battery_icon_canvas = nullptr;
    g_panel_sep_a = nullptr;
    g_panel_sep_b = nullptr;
    g_time_canvas = nullptr;
    g_second_canvas = nullptr;
    g_status_gif_canvas = nullptr;
    g_day_progress_canvas = nullptr;
    g_second_progress_canvas = nullptr;
    for (lv_obj_t *&segment : g_battery_segments) {
        segment = nullptr;
    }
    for (lv_obj_t *&obj : g_lower_panel_objects) {
        obj = nullptr;
    }
    for (lv_obj_t *&label : g_setup_status_labels) {
        label = nullptr;
    }
    g_last_ui_second = -1;
    g_last_ui_minute = -1;
    g_last_day_progress_filled = -1;
    g_last_second_progress_filled = -1;
    g_last_status_gif_frame = -1;
}

static void clear_info_object_refs()
{
    g_info_root = nullptr;
    g_settings_root = nullptr;
    for (lv_obj_t *&label : g_info_labels) {
        label = nullptr;
    }
    for (lv_obj_t *&label : g_settings_labels) {
        label = nullptr;
    }
    g_settings_feedback_label = nullptr;
}

static void remember_lower_panel_object(lv_obj_t *obj)
{
    for (lv_obj_t *&slot : g_lower_panel_objects) {
        if (!slot) {
            slot = obj;
            return;
        }
    }
}

static void set_lower_panel_visible(bool visible)
{
    for (lv_obj_t *obj : g_lower_panel_objects) {
        if (!obj) {
            continue;
        }
        if (visible) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void set_setup_panel_visible(bool visible)
{
    for (lv_obj_t *label : g_setup_status_labels) {
        if (!label) {
            continue;
        }
        if (visible) {
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void set_obj_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool update_low_battery_state()
{
    bool previous = g_low_battery_mode;
    if (g_battery_percent >= 0) {
        if (!g_low_battery_mode && g_battery_percent < kLowBatteryEnterPercent) {
            g_low_battery_mode = true;
        } else if (g_low_battery_mode && g_battery_percent >= kLowBatteryExitPercent) {
            g_low_battery_mode = false;
        }
    }
    return previous != g_low_battery_mode;
}

static void apply_clock_mode_visibility(bool setup_active)
{
    bool low = g_low_battery_mode && !setup_active;
    set_obj_visible(g_second_canvas, !low);
    set_obj_visible(g_day_progress_canvas, !low);
    set_obj_visible(g_second_progress_canvas, !low);
    set_obj_visible(g_low_battery_icon_canvas, low);
    set_lower_panel_visible(!setup_active && !low);
    set_setup_panel_visible(setup_active);
    set_obj_visible(g_panel_sep_a, !setup_active);
    set_obj_visible(g_panel_sep_b, !setup_active);
    if (low || setup_active) {
        set_obj_visible(g_alert_pill, false);
    }
}

static void update_alert_pill(bool show)
{
    bool visible = show && !g_low_battery_mode && g_weather_alert.active && g_weather_alert.title[0] != '\0';
    set_obj_visible(g_alert_pill, visible);
    if (visible) {
        set_label_text_if_changed(g_alert_label, g_weather_alert.title);
    }
}

static void draw_boot_anim_frame_index(int frame)
{
    if (!g_boot_anim_canvas) {
        return;
    }
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

static void boot_anim_task(void *)
{
    int frame = 0;
    while (g_boot_anim_running) {
        if (Lvgl_lock(100)) {
            draw_boot_anim_frame_index(frame);
            g_boot_anim_current_frame = frame;
            Lvgl_unlock();
        }
        frame = (frame + 1) % BOOT_ANIM_FRAME_COUNT;
        vTaskDelay(pdMS_TO_TICKS(kBootAnimRunFrameMs));
    }
    g_boot_anim_task_handle = nullptr;
    vTaskDelete(nullptr);
}

static void finish_boot_anim_to_last_frame()
{
    if (Lvgl_lock(200)) {
        draw_boot_anim_frame_index(BOOT_ANIM_FRAME_COUNT - 1);
        g_boot_anim_current_frame = BOOT_ANIM_FRAME_COUNT - 1;
        lv_refr_now(nullptr);
        Lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
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

    g_boot_status_label = make_label_with_font(screen, 28, 64, 344, 24, "Starting...", &lv_font_montserrat_16);
    lv_obj_set_style_text_align(g_boot_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    g_boot_detail_label = make_label_with_font(screen, 28, 256, 344, 22, "Preparing system", &lv_font_montserrat_14);
    lv_obj_set_style_text_align(g_boot_detail_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *version = make_label_with_font(screen, 28, 226, 344, 24, APP_VERSION, &lv_font_montserrat_16);
    lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (!g_boot_anim_canvas_buf) {
        g_boot_anim_canvas_buf = (lv_color_t *)heap_caps_calloc(BOOT_ANIM_WIDTH * BOOT_ANIM_HEIGHT,
                                                                sizeof(lv_color_t),
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_boot_anim_canvas_buf) {
            g_boot_anim_canvas_buf = (lv_color_t *)calloc(BOOT_ANIM_WIDTH * BOOT_ANIM_HEIGHT, sizeof(lv_color_t));
        }
    }
    g_boot_anim_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_boot_anim_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_boot_anim_canvas, 144, 100);
    lv_obj_set_size(g_boot_anim_canvas, BOOT_ANIM_WIDTH, BOOT_ANIM_HEIGHT);
    lv_obj_set_style_border_width(g_boot_anim_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_boot_anim_canvas, 0, LV_PART_MAIN);
    if (g_boot_anim_canvas_buf) {
        lv_canvas_set_buffer(g_boot_anim_canvas,
                             g_boot_anim_canvas_buf,
                             BOOT_ANIM_WIDTH,
                             BOOT_ANIM_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(g_boot_anim_canvas, lv_color_white(), LV_OPA_COVER);
        draw_boot_anim_frame_index(0);
    }

    lv_refr_now(nullptr);
}

static void update_boot_screen(int percent, const char *status, const char *detail)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    if (Lvgl_lock(2000)) {
        if (g_boot_status_label) {
            set_label_text_if_changed(g_boot_status_label, status);
        }
        if (g_boot_detail_label) {
            set_label_text_if_changed(g_boot_detail_label, detail);
        }
        lv_refr_now(nullptr);
        Lvgl_unlock();
    }
}

static void finish_boot_screen()
{
    if (Lvgl_lock(2000)) {
        lv_obj_clean(lv_scr_act());
        clear_clock_object_refs();
        clear_info_object_refs();
        g_boot_status_label = nullptr;
        g_boot_detail_label = nullptr;
        g_boot_anim_canvas = nullptr;
        build_clock_ui();
        show_page(g_clock_root);
        lv_refr_now(nullptr);
        Lvgl_unlock();
    }
}

static void build_boot_info_page()
{
    if (g_info_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    g_info_root = screen;
    lv_obj_add_flag(g_info_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label_with_font(screen, 24, 18, 352, 26, "SYSTEM INFO", &lv_font_montserrat_16);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *top_line = make_bar(screen, 24, 50, 352, 3);
    set_obj_black(top_line, true);

    static const int y_positions[] = {70, 104, 138, 172, 206};
    for (int i = 0; i < 5; ++i) {
        g_info_labels[i] = make_label_with_font(screen, 30, y_positions[i], 340, 24, "--", &lv_font_montserrat_14);
    }

    lv_obj_t *bottom_line = make_bar(screen, 24, 238, 352, 3);
    set_obj_black(bottom_line, true);

    lv_obj_t *hint = make_label_with_font(screen, 24, 255, 352, 22, "Hold 20s for setup  |  Release to return", &lv_font_montserrat_14);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void update_boot_info_page()
{
    char ntp[32];
    char weather[32];
    char line[96];

    format_time_or_dash(g_last_ntp_sync_time, ntp, sizeof(ntp));
    snprintf(line, sizeof(line), "Last NTP: %s", ntp);
    set_label_text_if_changed(g_info_labels[0], line);

    snprintf(line, sizeof(line), "WiFi: %s", g_wifi_ssid[0] ? g_wifi_ssid : "--");
    set_label_text_if_changed(g_info_labels[1], line);

    format_time_or_dash(g_last_weather_sync_time, weather, sizeof(weather));
    snprintf(line, sizeof(line), "Last Weather: %s", weather);
    set_label_text_if_changed(g_info_labels[2], line);

    if (g_battery_percent >= 0 && g_battery_voltage >= 0.0f) {
        snprintf(line, sizeof(line), "Battery: %d%%  %.2fV", g_battery_percent, g_battery_voltage);
    } else if (g_battery_percent >= 0) {
        snprintf(line, sizeof(line), "Battery: %d%%  --", g_battery_percent);
    } else {
        snprintf(line, sizeof(line), "Battery: --  --");
    }
    set_label_text_if_changed(g_info_labels[3], line);

    snprintf(line, sizeof(line), "Version: %s", APP_VERSION);
    set_label_text_if_changed(g_info_labels[4], line);
}

static void style_settings_item(lv_obj_t *label, bool selected)
{
    lv_obj_set_style_bg_color(label, selected ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(label, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(label, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(label, 5, LV_PART_MAIN);
}

static void build_settings_page()
{
    if (g_settings_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    g_settings_root = screen;
    lv_obj_add_flag(g_settings_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(screen, 24, 18, 352, 28, "设置");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_t *top_line = make_bar(screen, 24, 52, 352, 3);
    set_obj_black(top_line, true);

    static const int y_positions[] = {72, 118, 164, 210};
    for (int i = 0; i < 4; ++i) {
        g_settings_labels[i] = make_label(screen, 48, y_positions[i], 304, 34, "--");
        lv_label_set_long_mode(g_settings_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(g_settings_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    g_settings_feedback_label = make_label(screen, 24, 248, 352, 22, "");
    lv_obj_set_style_text_align(g_settings_feedback_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *hint = make_label_with_font(screen, 24, 270, 352, 22, "KEY: Select    BOOT: OK", &lv_font_montserrat_14);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void update_settings_page()
{
    char chime[40];
    snprintf(chime, sizeof(chime), "整点报时 %s", g_hourly_chime_enabled ? "ON" : "OFF");
    const char *items[] = {
        chime,
        "同步时间",
        "同步天气",
        g_factory_reset_confirm_pending ? "确认恢复出厂设置" : "恢复出厂设置",
    };
    int selected = g_settings_selection;
    if (selected < 0 || selected > 3) {
        selected = 0;
    }
    for (int i = 0; i < 4; ++i) {
        if (g_settings_labels[i]) {
            set_label_text_if_changed(g_settings_labels[i], items[i]);
            style_settings_item(g_settings_labels[i], i == selected);
        }
    }
    if (g_settings_feedback_label) {
        TickType_t now = xTaskGetTickCount();
        if (g_settings_feedback[0] && now < g_settings_feedback_until_tick) {
            set_label_text_if_changed(g_settings_feedback_label, g_settings_feedback);
        } else {
            g_settings_feedback[0] = '\0';
            set_label_text_if_changed(g_settings_feedback_label, "");
        }
    }
}

static void set_settings_feedback(const char *text, uint32_t duration_ms)
{
    strlcpy(g_settings_feedback, text, sizeof(g_settings_feedback));
    g_settings_feedback_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
}

static bool is_settings_sync_busy()
{
    return g_settings_sync_op != kSettingsSyncNone;
}

static void begin_settings_sync(SettingsSyncOp op, const char *text)
{
    TickType_t now = xTaskGetTickCount();
    g_settings_sync_op = op;
    g_settings_sync_deadline_tick = now + pdMS_TO_TICKS(kSettingsManualSyncTimeoutMs);
    g_settings_last_activity_tick = now;
    set_settings_feedback(text, kSettingsManualSyncTimeoutMs);
}

static void finish_settings_sync(SettingsSyncOp op, const char *text)
{
    if (g_settings_sync_op != op) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    g_settings_sync_op = kSettingsSyncNone;
    g_settings_sync_deadline_tick = 0;
    g_settings_last_activity_tick = now;
    set_settings_feedback(text, 3500);
}

static void update_setup_status_panel()
{
    char line[96];
    if (!g_setup_status_labels[0]) {
        return;
    }
    set_label_text_if_changed(g_setup_status_labels[0], "Setup Mode");
    snprintf(line, sizeof(line), "AP SSID: %s", g_ap_ssid[0] ? g_ap_ssid : "--");
    set_label_text_if_changed(g_setup_status_labels[1], line);
    snprintf(line, sizeof(line), "AP Password: %s", kSetupApPassword);
    set_label_text_if_changed(g_setup_status_labels[2], line);
    set_label_text_if_changed(g_setup_status_labels[3], "Portal IP: 192.168.4.1");
    snprintf(line, sizeof(line), "STA SSID: %s", g_wifi_ssid[0] ? g_wifi_ssid : "--");
    set_label_text_if_changed(g_setup_status_labels[4], line);
    if (g_sta_ip[0]) {
        snprintf(line, sizeof(line), "STA IP: %s", g_sta_ip);
    } else if (g_last_wifi_disconnect_reason) {
        snprintf(line, sizeof(line), "STA IP: --  reason %d", g_last_wifi_disconnect_reason);
    } else {
        snprintf(line, sizeof(line), "STA IP: --");
    }
    set_label_text_if_changed(g_setup_status_labels[5], line);
}

static void update_battery_icon(int percent)
{
    int filled = 0;
    if (percent >= 0) {
        if (percent > 100) {
            percent = 100;
        }
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

static void build_clock_ui()
{
    if (g_clock_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    g_clock_root = screen;

    g_date_label = make_label(screen, 198, 15, 182, 26, "----/--/-- / 星期-");
    lv_obj_set_style_text_align(g_date_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    build_battery_icon(screen);

    g_alert_pill = lv_obj_create(screen);
    lv_obj_clear_flag(g_alert_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_alert_pill, 64, 11);
    lv_obj_set_size(g_alert_pill, 128, 26);
    lv_obj_set_style_bg_color(g_alert_pill, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_alert_pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_alert_pill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_alert_pill, 13, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alert_pill, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_alert_pill, LV_OBJ_FLAG_HIDDEN);

    if (!g_alert_icon_canvas_buf) {
        g_alert_icon_canvas_buf = (lv_color_t *)heap_caps_calloc(WARNING_ICON_WIDTH * WARNING_ICON_HEIGHT,
                                                                 sizeof(lv_color_t),
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_alert_icon_canvas_buf) {
            g_alert_icon_canvas_buf = (lv_color_t *)calloc(WARNING_ICON_WIDTH * WARNING_ICON_HEIGHT, sizeof(lv_color_t));
        }
    }
    g_alert_icon_canvas = lv_canvas_create(g_alert_pill);
    lv_obj_clear_flag(g_alert_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_alert_icon_canvas, 8, 4);
    lv_obj_set_size(g_alert_icon_canvas, WARNING_ICON_WIDTH, WARNING_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_alert_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alert_icon_canvas, 0, LV_PART_MAIN);
    if (g_alert_icon_canvas_buf) {
        lv_canvas_set_buffer(g_alert_icon_canvas,
                             g_alert_icon_canvas_buf,
                             WARNING_ICON_WIDTH,
                             WARNING_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        draw_1bit_icon(g_alert_icon_canvas,
                       WARNING_ICON_WIDTH,
                       WARNING_ICON_HEIGHT,
                       WARNING_ICON_BYTES_PER_ROW,
                       warning_icon_bits,
                       lv_color_white(),
                       lv_color_black());
    }
    g_alert_label = make_label_with_font(g_alert_pill, 30, 4, 90, 18, "", &zh_font_16);
    lv_obj_set_style_text_color(g_alert_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(g_alert_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(g_alert_label, LV_LABEL_LONG_CLIP);

    g_weather_city_label = make_label(screen, 24, 196, 76, 20, "--");
    remember_lower_panel_object(g_weather_city_label);
    lv_obj_set_style_text_align(g_weather_city_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_weather_icon_label = make_label(screen, 101, 194, 34, 38, "");
    remember_lower_panel_object(g_weather_icon_label);
    lv_obj_set_style_text_font(g_weather_icon_label, &qweather_icons_36, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_weather_icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_weather_info_label = make_label(screen, 24, 218, 76, 20, "天气等待");
    remember_lower_panel_object(g_weather_info_label);
    lv_label_set_long_mode(g_weather_info_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(g_weather_info_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_weather_temp_label = make_label(screen, 30, 242, 68, 20, "--℃");
    g_weather_humi_label = make_label(screen, 30, 264, 68, 20, "--%");
    remember_lower_panel_object(g_weather_temp_label);
    remember_lower_panel_object(g_weather_humi_label);
    lv_obj_set_style_text_align(g_weather_temp_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_weather_humi_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    g_temp_label = make_label(screen, 152, 214, 96, 28, "温度 --.-℃");
    g_humi_label = make_label(screen, 152, 246, 96, 28, "湿度 --.-%");
    remember_lower_panel_object(g_temp_label);
    remember_lower_panel_object(g_humi_label);
    lv_obj_set_style_text_align(g_temp_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_humi_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    constexpr int canvas_w = 292;
    constexpr int canvas_h = 92;
    if (!g_time_canvas_buf) {
        g_time_canvas_buf = (lv_color_t *)heap_caps_calloc(canvas_w * canvas_h, sizeof(lv_color_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_time_canvas_buf) {
            g_time_canvas_buf = (lv_color_t *)calloc(canvas_w * canvas_h, sizeof(lv_color_t));
        }
    }
    g_time_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_time_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_time_canvas, 18, 76);
    lv_obj_set_size(g_time_canvas, canvas_w, canvas_h);
    lv_obj_set_style_border_width(g_time_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_time_canvas, 0, LV_PART_MAIN);
    if (g_time_canvas_buf) {
        lv_canvas_set_buffer(g_time_canvas, g_time_canvas_buf, canvas_w, canvas_h, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(g_time_canvas, lv_color_white(), LV_OPA_COVER);
    }

    constexpr int second_w = 60;
    constexpr int second_h = 40;
    if (!g_second_canvas_buf) {
        g_second_canvas_buf = (lv_color_t *)heap_caps_calloc(second_w * second_h, sizeof(lv_color_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_second_canvas_buf) {
            g_second_canvas_buf = (lv_color_t *)calloc(second_w * second_h, sizeof(lv_color_t));
        }
    }
    g_second_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_second_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_second_canvas, 320, 124);
    lv_obj_set_size(g_second_canvas, second_w, second_h);
    lv_obj_set_style_border_width(g_second_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_second_canvas, 0, LV_PART_MAIN);
    if (g_second_canvas_buf) {
        lv_canvas_set_buffer(g_second_canvas, g_second_canvas_buf, second_w, second_h, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(g_second_canvas, lv_color_white(), LV_OPA_COVER);
    }

    if (!g_status_gif_canvas_buf) {
        g_status_gif_canvas_buf = (lv_color_t *)heap_caps_calloc(STATUS_GIF_WIDTH * STATUS_GIF_HEIGHT,
                                                                 sizeof(lv_color_t),
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_status_gif_canvas_buf) {
            g_status_gif_canvas_buf = (lv_color_t *)calloc(STATUS_GIF_WIDTH * STATUS_GIF_HEIGHT, sizeof(lv_color_t));
        }
    }
    g_status_gif_canvas = lv_canvas_create(screen);
    remember_lower_panel_object(g_status_gif_canvas);
    lv_obj_clear_flag(g_status_gif_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_status_gif_canvas, 279, 196);
    lv_obj_set_size(g_status_gif_canvas, STATUS_GIF_WIDTH, STATUS_GIF_HEIGHT);
    lv_obj_set_style_border_width(g_status_gif_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_status_gif_canvas, 0, LV_PART_MAIN);
    if (g_status_gif_canvas_buf) {
        lv_canvas_set_buffer(g_status_gif_canvas,
                             g_status_gif_canvas_buf,
                             STATUS_GIF_WIDTH,
                             STATUS_GIF_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(g_status_gif_canvas, lv_color_white(), LV_OPA_COVER);
        draw_status_gif_frame(0);
    }

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    lv_obj_t *bottom_line = make_bar(screen, 18, 184, 364, 4);
    build_progress_canvas(screen, &g_day_progress_canvas, &g_day_progress_canvas_buf, 59);
    build_progress_canvas(screen, &g_second_progress_canvas, &g_second_progress_canvas_buf, 180);
    g_panel_sep_a = make_bar(screen, 139, 188, 2, 102);
    g_panel_sep_b = make_bar(screen, 260, 188, 2, 102);
    remember_lower_panel_object(g_panel_sep_a);
    remember_lower_panel_object(g_panel_sep_b);
    set_obj_black(top_line, true);
    set_obj_black(bottom_line, true);
    set_obj_black(g_panel_sep_a, true);
    set_obj_black(g_panel_sep_b, true);

    if (!g_low_battery_icon_canvas_buf) {
        g_low_battery_icon_canvas_buf = (lv_color_t *)heap_caps_calloc(LOW_BATTERY_ICON_WIDTH * LOW_BATTERY_ICON_HEIGHT,
                                                                       sizeof(lv_color_t),
                                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_low_battery_icon_canvas_buf) {
            g_low_battery_icon_canvas_buf = (lv_color_t *)calloc(LOW_BATTERY_ICON_WIDTH * LOW_BATTERY_ICON_HEIGHT,
                                                                 sizeof(lv_color_t));
        }
    }
    g_low_battery_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_low_battery_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_low_battery_icon_canvas, 156, 214);
    lv_obj_set_size(g_low_battery_icon_canvas, LOW_BATTERY_ICON_WIDTH, LOW_BATTERY_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_low_battery_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_low_battery_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_low_battery_icon_canvas, LV_OBJ_FLAG_HIDDEN);
    if (g_low_battery_icon_canvas_buf) {
        lv_canvas_set_buffer(g_low_battery_icon_canvas,
                             g_low_battery_icon_canvas_buf,
                             LOW_BATTERY_ICON_WIDTH,
                             LOW_BATTERY_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        draw_1bit_icon(g_low_battery_icon_canvas,
                       LOW_BATTERY_ICON_WIDTH,
                       LOW_BATTERY_ICON_HEIGHT,
                       LOW_BATTERY_ICON_BYTES_PER_ROW,
                       low_battery_icon_bits,
                       lv_color_black(),
                       lv_color_white());
    }

    static const int setup_y[] = {194, 212, 230, 248, 266, 284};
    static const char *setup_text[] = {
        "Setup Mode",
        "AP SSID: --",
        "AP Password: --",
        "Portal IP: 192.168.4.1",
        "STA SSID: --",
        "STA IP: --",
    };
    for (int i = 0; i < 6; ++i) {
        g_setup_status_labels[i] = make_label_with_font(screen,
                                                        26,
                                                        setup_y[i],
                                                        348,
                                                        18,
                                                        setup_text[i],
                                                        &lv_font_montserrat_14);
        lv_obj_add_flag(g_setup_status_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static bool load_saved_config()
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t ssid_len = sizeof(g_wifi_ssid);
    size_t pass_len = sizeof(g_wifi_pass);
    size_t key_len = sizeof(g_weather_api_key);
    esp_err_t ssid_err = nvs_get_str(nvs, "ssid", g_wifi_ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(nvs, "pass", g_wifi_pass, &pass_len);
    esp_err_t key_err = nvs_get_str(nvs, "api_key", g_weather_api_key, &key_len);
    uint8_t chime = 0;
    (void)nvs_get_u8(nvs, "hourly_chime", &chime);
    nvs_close(nvs);
    g_have_weather_key = key_err == ESP_OK && g_weather_api_key[0] != '\0';
    g_hourly_chime_enabled = chime != 0;
    return ssid_err == ESP_OK && pass_err == ESP_OK && g_wifi_ssid[0] != '\0';
}

static void save_config(const char *ssid, const char *pass, const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("wifi", NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "pass", pass));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "api_key", api_key));
    (void)nvs_erase_key(nvs, "api_host");
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));
    strlcpy(g_wifi_pass, pass, sizeof(g_wifi_pass));
    strlcpy(g_weather_api_key, api_key, sizeof(g_weather_api_key));
    g_have_wifi_creds = true;
    g_have_weather_key = g_weather_api_key[0] != '\0';
}

static void save_hourly_chime_setting()
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("wifi", NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_u8(nvs, "hourly_chime", g_hourly_chime_enabled ? 1 : 0));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
}

static void clear_saved_config()
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_erase_key(nvs, "ssid");
        (void)nvs_erase_key(nvs, "pass");
        (void)nvs_erase_key(nvs, "api_key");
        (void)nvs_erase_key(nvs, "api_host");
        ESP_ERROR_CHECK(nvs_commit(nvs));
        nvs_close(nvs);
    }
    g_wifi_ssid[0] = '\0';
    g_wifi_pass[0] = '\0';
    g_weather_api_key[0] = '\0';
    g_sta_ip[0] = '\0';
    g_have_wifi_creds = false;
    g_have_weather_key = false;
    xEventGroupClearBits(g_app_events, kWifiConnectedBit | kWeatherReadyBit);
}

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; ++si) {
        if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = {src[si + 1], src[si + 2], 0};
            dst[di++] = (char)strtol(hex, nullptr, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static void form_value(const char *body, const char *key, char *out, size_t out_len)
{
    char pattern[16];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *start = strstr(body, pattern);
    if (!start) {
        out[0] = '\0';
        return;
    }
    start += strlen(pattern);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    char encoded[160] = {};
    if (len >= sizeof(encoded)) {
        len = sizeof(encoded) - 1;
    }
    memcpy(encoded, start, len);
    encoded[len] = '\0';
    url_decode(out, out_len, encoded);
}

static void form_value_fallback(const char *body, const char *primary_key, const char *fallback_key, char *out, size_t out_len)
{
    form_value(body, primary_key, out, out_len);
    if (out[0] == '\0' && fallback_key) {
        form_value(body, fallback_key, out, out_len);
    }
}

static bool save_credentials_from_body(const char *body)
{
    char ssid[33] = {};
    char pass[65] = {};
    char api_key[96] = {};
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value_fallback(body, "pass", "password", pass, sizeof(pass));
    form_value_fallback(body, "api_key", "weather", api_key, sizeof(api_key));
    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "provisioning ignored empty ssid");
        return false;
    }
    if (api_key[0] == '\0' && g_weather_api_key[0] != '\0') {
        strlcpy(api_key, g_weather_api_key, sizeof(api_key));
    }
    ESP_LOGI(TAG, "provisioning saved ssid=%s pass_len=%u api_key=%s",
             ssid, (unsigned)strlen(pass), api_key[0] ? "set" : "empty");
    g_last_wifi_disconnect_reason = 0;
    xEventGroupClearBits(g_app_events, kWifiConnectedBit);
    save_config(ssid, pass, api_key);
    apply_station_config(true);
    return true;
}

static void html_append(char *html, size_t html_len, const char *fmt, ...)
{
    size_t used = strlen(html);
    if (used >= html_len - 1) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(html + used, html_len - used, fmt, args);
    va_end(args);
}

static void html_escape(const char *src, char *dst, size_t dst_len)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; ++si) {
        const char *rep = nullptr;
        if (src[si] == '&') {
            rep = "&amp;";
        } else if (src[si] == '<') {
            rep = "&lt;";
        } else if (src[si] == '>') {
            rep = "&gt;";
        } else if (src[si] == '"') {
            rep = "&quot;";
        }
        if (rep) {
            size_t rep_len = strlen(rep);
            if (di + rep_len >= dst_len) {
                break;
            }
            memcpy(dst + di, rep, rep_len);
            di += rep_len;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static void append_wifi_scan_list(char *html, size_t html_len)
{
    html_append(html, html_len, "<section><div class='section-title'><span>Nearby Wi-Fi</span><a href='/'>Refresh</a></div><div class='wifi-list'>");
    wifi_scan_config_t scan_config = {};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        html_append(html, html_len, "<p class='muted'>Scan busy, refresh this page.</p>");
    } else {
        uint16_t ap_count = 0;
        err = esp_wifi_scan_get_ap_num(&ap_count);
        if (err != ESP_OK) {
            html_append(html, html_len, "<p class='muted'>Scan failed, refresh this page.</p>");
            html_append(html, html_len, "</div>");
            return;
        }
        wifi_ap_record_t *records = (wifi_ap_record_t *)calloc(16, sizeof(wifi_ap_record_t));
        if (records == nullptr) {
            html_append(html, html_len, "<p class='muted'>Not enough memory to list Wi-Fi.</p>");
            html_append(html, html_len, "</div>");
            return;
        }
        uint16_t max_records = 16;
        err = esp_wifi_scan_get_ap_records(&max_records, records);
        if (err != ESP_OK) {
            free(records);
            html_append(html, html_len, "<p class='muted'>Scan failed, refresh this page.</p>");
            html_append(html, html_len, "</div>");
            return;
        }
        if (max_records == 0) {
            html_append(html, html_len, "<p class='muted'>No Wi-Fi found.</p>");
        }
        for (uint16_t i = 0; i < max_records; ++i) {
            if (records[i].ssid[0] == '\0') {
                continue;
            }
            char ssid[80] = {};
            html_escape((const char *)records[i].ssid, ssid, sizeof(ssid));
            html_append(html, html_len,
                        "<button type='button' class='wifi' data-ssid=\"%s\" onclick=\"pick(this.dataset.ssid)\"><span>%s</span><b>%d dBm</b></button>",
                        ssid, ssid, records[i].rssi);
        }
        free(records);
        (void)ap_count;
    }
    html_append(html, html_len, "</div></section>");
}

static void apply_station_config(bool reconnect)
{
    wifi_config_t sta_config = {};
    strlcpy((char *)sta_config.sta.ssid, g_wifi_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, g_wifi_pass, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    if (reconnect) {
        esp_wifi_disconnect();
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "wifi connect failed to start: %s", esp_err_to_name(err));
        }
    }
}

static void stop_http_server()
{
    if (g_http_server) {
        httpd_stop(g_http_server);
        g_http_server = nullptr;
    }
    g_setup_portal_active = false;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char safe_ssid[80] = {};
    html_escape(g_wifi_ssid, safe_ssid, sizeof(safe_ssid));
    const size_t html_len = 12288;
    char *html = (char *)calloc(1, html_len);
    if (html == nullptr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Not enough memory.");
    }
    html_append(html, html_len,
                "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>WeatherClock Setup</title><style>"
                ":root{color-scheme:light}*{box-sizing:border-box}body{margin:0;background:#eef1f5;color:#17202a;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
                ".wrap{max-width:480px;margin:0 auto;padding:22px 16px 34px}.brand{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px}"
                ".mark{width:44px;height:44px;border:2px solid #17202a;border-radius:8px;display:grid;place-items:center;font-weight:900;font-size:22px;background:#fff}"
                ".pill{border:1px solid #b7c0ca;border-radius:999px;padding:7px 10px;font-size:12px;color:#465563;background:#fff}"
                "h1{font-size:26px;line-height:1.12;margin:0 0 4px}p{margin:0}.sub{font-size:14px;color:#5d6b78}.panel{background:#fff;border:1px solid #d3dae2;border-radius:8px;padding:16px;box-shadow:0 8px 24px rgba(23,32,42,.08)}"
                "label{display:block;font-size:12px;font-weight:700;letter-spacing:.03em;color:#465563;margin:13px 0 6px;text-transform:uppercase}"
                "input{width:100%;height:46px;border:1px solid #aeb8c2;border-radius:6px;padding:0 12px;font-size:17px;background:#fbfcfd;color:#111;outline:none}"
                "input:focus{border-color:#17202a;box-shadow:0 0 0 3px rgba(23,32,42,.10)}.submit{width:100%;height:48px;border:0;border-radius:6px;margin-top:16px;background:#17202a;color:#fff;font-size:17px;font-weight:800}"
                "section{margin-top:16px}.section-title{display:flex;align-items:center;justify-content:space-between;margin:0 2px 8px;font-size:13px;font-weight:800;color:#465563}.section-title a{color:#17202a;text-decoration:none}"
                ".wifi-list{display:grid;gap:8px}.wifi{width:100%;border:1px solid #d3dae2;background:#fff;border-radius:6px;padding:12px;display:flex;justify-content:space-between;gap:12px;text-align:left;font-size:16px;color:#17202a}"
                ".wifi b{font-size:12px;color:#697784;white-space:nowrap}.muted{padding:12px;border:1px dashed #c7d0d9;border-radius:6px;color:#697784;background:#fbfcfd}"
                "</style><script>function pick(s){document.querySelector('[name=ssid]').value=s;document.querySelector('[name=pass]').focus();}</script></head>"
                "<body><main class='wrap'><div class='brand'><div><h1>WeatherClock</h1><p class='sub'>Connect the clock to your local Wi-Fi.</p></div><div class='mark'>42</div></div>"
                "<div class='panel'><div class='pill'>Setup AP: %s</div><form method='get' action='/save'>"
                "<label>Wi-Fi SSID</label><input name='ssid' placeholder='Choose or type network name' value='%s' autocomplete='off'>"
                "<label>Password</label><input name='pass' placeholder='Wi-Fi password' type='password' autocomplete='current-password'>"
                "<label>QWeather API Key</label><input name='api_key' placeholder='Leave blank to keep saved key' value='' autocomplete='off'>"
                "<button class='submit' type='submit'>Save and connect</button></form></div>",
                g_ap_ssid, safe_ssid);
    append_wifi_scan_list(html, html_len);
    html_append(html, html_len, "</main></body></html>");
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, html, strlen(html));
    free(html);
    return err;
}

static esp_err_t send_save_result_page(httpd_req_t *req, bool saved, bool connected)
{
    char safe_ssid[80] = {};
    html_escape(g_wifi_ssid, safe_ssid, sizeof(safe_ssid));
    char html[1400] = {};
    const char *title = saved ? (connected ? "Connected" : "Saved, still connecting") : "Missing Wi-Fi name";
    const char *body = saved ? (connected ? "The clock has joined your Wi-Fi network." : "The clock saved your settings but did not get an IP yet. Check the password or router signal, then try again.")
                             : "Please go back and enter a Wi-Fi network name.";
    html_append(html, sizeof(html),
                "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>WeatherClock Setup</title><style>"
                "*{box-sizing:border-box}body{margin:0;background:#eef1f5;color:#17202a;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
                ".wrap{max-width:460px;margin:0 auto;padding:28px 16px}.panel{background:#fff;border:1px solid #d3dae2;border-radius:8px;padding:18px;box-shadow:0 8px 24px rgba(23,32,42,.08)}"
                ".state{width:48px;height:48px;border-radius:8px;border:2px solid #17202a;display:grid;place-items:center;font-size:24px;font-weight:900;margin-bottom:14px}"
                "h1{font-size:24px;margin:0 0 8px}p{font-size:15px;line-height:1.45;color:#4d5b68;margin:0 0 14px}.meta{border-top:1px solid #e1e6eb;padding-top:12px;color:#697784;font-size:13px}"
                "a{display:block;height:46px;line-height:46px;text-align:center;background:#17202a;color:#fff;text-decoration:none;border-radius:6px;font-weight:800;margin-top:16px}"
                "</style></head><body><main class='wrap'><section class='panel'><div class='state'>%s</div><h1>%s</h1><p>%s</p>"
                "<div class='meta'>SSID: %s<br>Last Wi-Fi reason: %d</div><a href='/'>Back to setup</a></section></main></body></html>",
                connected ? "OK" : "!", title, body, safe_ssid, g_last_wifi_disconnect_reason);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[640] = {};
    int total = 0;
    while (total < req->content_len && total < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        total += ret;
    }
    body[total] = '\0';

    bool saved = save_credentials_from_body(body);
    bool connected = saved && wait_for_wifi_connected(12000);
    esp_err_t err = send_save_result_page(req, saved, connected);
    if (connected) {
        xEventGroupSetBits(g_app_events, kProvisioningSyncBit);
    }
    return err;
}

static esp_err_t save_get_handler(httpd_req_t *req)
{
    char query[640] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Missing query.");
    }
    bool saved = save_credentials_from_body(query);
    bool connected = saved && wait_for_wifi_connected(12000);
    esp_err_t err = send_save_result_page(req, saved, connected);
    if (connected) {
        xEventGroupSetBits(g_app_events, kProvisioningSyncBit);
    }
    return err;
}

static esp_err_t empty_asset_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, "", 0);
}

static void start_http_server()
{
    if (g_http_server) {
        g_setup_portal_active = true;
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&g_http_server, &config));

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = root_get_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_http_server, &root));

    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = save_post_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_http_server, &save));

    httpd_uri_t save_get = {};
    save_get.uri = "/save";
    save_get.method = HTTP_GET;
    save_get.handler = save_get_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_http_server, &save_get));

    httpd_uri_t favicon = {};
    favicon.uri = "/favicon.ico";
    favicon.method = HTTP_GET;
    favicon.handler = empty_asset_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_http_server, &favicon));

    httpd_uri_t apple_icon = {};
    apple_icon.uri = "/apple-touch-icon.png";
    apple_icon.method = HTTP_GET;
    apple_icon.handler = empty_asset_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_http_server, &apple_icon));

    httpd_uri_t apple_icon_precomposed = {};
    apple_icon_precomposed.uri = "/apple-touch-icon-precomposed.png";
    apple_icon_precomposed.method = HTTP_GET;
    apple_icon_precomposed.handler = empty_asset_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_http_server, &apple_icon_precomposed));
    g_setup_portal_active = true;
}

static void start_wifi_radio(bool enable_setup_portal)
{
    if (g_wifi_radio_on) {
        if (enable_setup_portal && !g_setup_portal_active) {
            start_http_server();
        }
        if (g_have_wifi_creds) {
            apply_station_config(true);
        }
        return;
    }

    g_wifi_stop_requested = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(enable_setup_portal ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    if (g_have_wifi_creds) {
        apply_station_config(false);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t ps_err = esp_wifi_set_ps(enable_setup_portal ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi power save setup failed: %s", esp_err_to_name(ps_err));
    }
    g_wifi_radio_on = true;
    if (enable_setup_portal) {
        start_http_server();
    }
}

static void stop_wifi_radio(bool force_setup_portal = false)
{
    if (!g_wifi_radio_on || !g_have_wifi_creds) {
        return;
    }
    if (g_setup_portal_active && !force_setup_portal) {
        return;
    }
    stop_http_server();
    g_wifi_stop_requested = true;
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_stop());
    g_wifi_radio_on = false;
    xEventGroupClearBits(g_app_events, kWifiConnectedBit);
    ESP_LOGI(TAG, "wifi radio off");
}

static void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && g_have_wifi_creds) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        g_last_wifi_disconnect_reason = event ? event->reason : -1;
        g_sta_ip[0] = '\0';
        ESP_LOGW(TAG, "wifi disconnected, reason=%d", event ? event->reason : -1);
        xEventGroupClearBits(g_app_events, kWifiConnectedBit);
        if (g_have_wifi_creds && g_wifi_radio_on && !g_wifi_stop_requested) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "wifi reconnect failed to start: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(g_sta_ip, sizeof(g_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(g_app_events, kWifiConnectedBit);
    }
}

static void init_wifi()
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "WeatherClock-%02X%02X", mac[4], mac[5]);

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, nullptr));

    wifi_config_t ap_config = {};
    strlcpy((char *)ap_config.ap.ssid, g_ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, kSetupApPassword, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(g_ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (!g_have_wifi_creds) {
        start_wifi_radio(true);
    }
}

static void button_task(void *)
{
    gpio_config_t button = {};
    button.intr_type = GPIO_INTR_DISABLE;
    button.mode = GPIO_MODE_INPUT;
    button.pin_bit_mask = (1ULL << kBootButtonGpio) | (1ULL << kKeyButtonGpio);
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));

    TickType_t boot_pressed_since = 0;
    bool boot_setup_triggered = false;
    bool boot_info_requested = false;
    TickType_t key_pressed_since = 0;
    bool key_long_handled = false;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool boot_pressed = gpio_get_level(kBootButtonGpio) == 0;
        bool key_pressed = gpio_get_level(kKeyButtonGpio) == 0;

        if (boot_pressed) {
            if (boot_pressed_since == 0) {
                boot_pressed_since = now;
            }
            TickType_t held = now - boot_pressed_since;
            if (!g_settings_requested && !boot_info_requested && held >= pdMS_TO_TICKS(kBootInfoHoldMs)) {
                ESP_LOGI(TAG, "boot button held for 5s, showing info page");
                g_boot_info_requested = true;
                boot_info_requested = true;
            }
            if (!g_settings_requested && !boot_setup_triggered && held >= pdMS_TO_TICKS(kBootSetupHoldMs)) {
                ESP_LOGW(TAG, "boot button held for 20s, entering setup portal");
                g_boot_info_requested = false;
                start_wifi_radio(true);
                boot_setup_triggered = true;
            }
        } else {
            if (boot_pressed_since != 0 && g_settings_requested && !boot_setup_triggered) {
                TickType_t held = now - boot_pressed_since;
                if (held >= pdMS_TO_TICKS(40) && held < pdMS_TO_TICKS(1200)) {
                    g_settings_action_seq = g_settings_action_seq + 1;
                    g_settings_last_activity_tick = now;
                }
            }
            if (boot_info_requested && !boot_setup_triggered) {
                ESP_LOGI(TAG, "boot button released before setup, returning to clock");
                g_boot_info_requested = false;
            }
            boot_pressed_since = 0;
            boot_setup_triggered = false;
            boot_info_requested = false;
        }

        if (key_pressed) {
            if (key_pressed_since == 0) {
                key_pressed_since = now;
                key_long_handled = false;
            }
            if (!key_long_handled && now - key_pressed_since >= pdMS_TO_TICKS(kSettingsHoldMs)) {
                ESP_LOGI(TAG, "key button held for 2s, showing settings page");
                g_boot_info_requested = false;
                g_settings_requested = true;
                g_settings_last_activity_tick = now;
                key_long_handled = true;
            }
        } else {
            if (key_pressed_since != 0 && !key_long_handled && g_settings_requested) {
                TickType_t held = now - key_pressed_since;
                if (held >= pdMS_TO_TICKS(40) && held < pdMS_TO_TICKS(1200)) {
                    g_settings_last_activity_tick = now;
                    if (!is_settings_sync_busy()) {
                        g_settings_selection = (g_settings_selection + 1) % 4;
                        g_factory_reset_confirm_pending = false;
                        g_settings_feedback[0] = '\0';
                    }
                }
            }
            key_pressed_since = 0;
            key_long_handled = false;
        }
        int delay_ms = kButtonIdlePollMs;
        if (boot_pressed || key_pressed) {
            delay_ms = kButtonPressedPollMs;
        } else if (g_settings_requested || g_boot_info_requested || g_setup_portal_active) {
            delay_ms = kButtonActivePollMs;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void init_power_management()
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    pm_config.min_freq_mhz = CONFIG_XTAL_FREQ;
    pm_config.light_sleep_enable = true;

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management setup failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "power management: max=%dMHz min=%dMHz light sleep enabled",
                 pm_config.max_freq_mhz, pm_config.min_freq_mhz);
    }
#else
    ESP_LOGW(TAG, "power management disabled in sdkconfig");
#endif
}

static void acquire_network_awake_lock()
{
}

static void release_network_awake_lock()
{
}

static void restore_system_time_from_rtc()
{
    rtcTimeStruct_t rtc_time = {};
    Rtc_GetTime(&rtc_time);
    if (rtc_time.year < kMinValidYear || rtc_time.year > kMaxValidYear ||
        rtc_time.month < 1 || rtc_time.month > 12 ||
        rtc_time.day < 1 || rtc_time.day > 31 ||
        rtc_time.hour > 23 || rtc_time.minute > 59 || rtc_time.second > 59) {
        ESP_LOGW(TAG, "ignore invalid RTC time: %04u-%02u-%02u %02u:%02u:%02u",
                 rtc_time.year, rtc_time.month, rtc_time.day,
                 rtc_time.hour, rtc_time.minute, rtc_time.second);
        return;
    }
    struct tm tm_time = {};
    tm_time.tm_year = rtc_time.year - 1900;
    tm_time.tm_mon = rtc_time.month - 1;
    tm_time.tm_mday = rtc_time.day;
    tm_time.tm_hour = rtc_time.hour;
    tm_time.tm_min = rtc_time.minute;
    tm_time.tm_sec = rtc_time.second;
    time_t epoch = mktime(&tm_time);
    struct tm normalized = {};
    localtime_r(&epoch, &normalized);
    if (normalized.tm_year + 1900 != rtc_time.year ||
        normalized.tm_mon + 1 != rtc_time.month ||
        normalized.tm_mday != rtc_time.day) {
        ESP_LOGW(TAG, "ignore normalized RTC time mismatch");
        return;
    }
    struct timeval now = {};
    now.tv_sec = epoch;
    settimeofday(&now, nullptr);
    ESP_LOGI(TAG, "system time restored from RTC: %04u-%02u-%02u %02u:%02u:%02u",
             rtc_time.year, rtc_time.month, rtc_time.day,
             rtc_time.hour, rtc_time.minute, rtc_time.second);
}

static void sync_rtc_from_system_time()
{
    time_t now;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    Rtc_SetTime(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

static void release_battery_gauge()
{
    if (g_battery_adc_cali_ready && g_battery_adc_cali) {
        adc_cali_delete_scheme_curve_fitting(g_battery_adc_cali);
    }
    g_battery_adc_cali = nullptr;
    g_battery_adc_cali_ready = false;

    if (g_battery_adc) {
        adc_oneshot_del_unit(g_battery_adc);
    }
    g_battery_adc = nullptr;
    g_battery_adc_ready = false;
}

static bool init_battery_gauge()
{
    if (g_battery_adc_ready) {
        return true;
    }

    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_new_unit(&init_config, &g_battery_adc);
    if (err != ESP_OK) {
        g_battery_adc = nullptr;
        ESP_LOGW(TAG, "battery adc init failed: %s", esp_err_to_name(err));
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.bitwidth = ADC_BITWIDTH_12;
    chan_config.atten = ADC_ATTEN_DB_12;
    err = adc_oneshot_config_channel(g_battery_adc, ADC_CHANNEL_3, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery adc channel config failed: %s", esp_err_to_name(err));
        release_battery_gauge();
        return false;
    }

    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.chan = ADC_CHANNEL_3;
    cali_config.atten = ADC_ATTEN_DB_12;
    cali_config.bitwidth = ADC_BITWIDTH_12;
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &g_battery_adc_cali);
    if (err == ESP_OK) {
        g_battery_adc_cali_ready = true;
    } else {
        ESP_LOGW(TAG, "battery adc calibration unavailable: %s", esp_err_to_name(err));
    }

    g_battery_adc_ready = true;
    return true;
}

static int battery_percent_from_voltage(float voltage)
{
    static constexpr float kBatteryEmptyVoltage = 3.00f;
    static constexpr float kBatteryFullVoltage = 4.12f;
    int percent = (int)(((voltage - kBatteryEmptyVoltage) * 100.0f /
                         (kBatteryFullVoltage - kBatteryEmptyVoltage)) + 0.5f);
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

static bool read_battery_percent(int *percent)
{
    if (!init_battery_gauge()) {
        return false;
    }

    int raw = 0;
    esp_err_t err = adc_oneshot_read(g_battery_adc, ADC_CHANNEL_3, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery adc read failed: %s", esp_err_to_name(err));
        release_battery_gauge();
        return false;
    }

    int adc_mv = (raw * 3300) / 4095;
    if (g_battery_adc_cali_ready) {
        err = adc_cali_raw_to_voltage(g_battery_adc_cali, raw, &adc_mv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "battery adc calibration read failed: %s", esp_err_to_name(err));
        }
    }

    float voltage = adc_mv * 0.001f * 3.0f;
    int soc = battery_percent_from_voltage(voltage);
    ESP_LOGI(TAG, "battery adc raw=%d adc_mv=%d battery=%.3fV soc=%d%%", raw, adc_mv, voltage, soc);
    *percent = soc;
    g_battery_voltage = voltage;
    release_battery_gauge();
    return true;
}

static void sample_battery()
{
    int percent = -1;
    if (read_battery_percent(&percent)) {
        g_battery_percent = percent;
    } else {
        g_battery_percent = -1;
    }
    update_low_battery_state();
    ++g_battery_version;
}

static int boot_sync_remaining_ms()
{
    if (g_boot_sync_deadline_us <= 0) {
        return INT32_MAX;
    }
    int64_t remaining_us = g_boot_sync_deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    int64_t remaining_ms = remaining_us / 1000;
    return remaining_ms > INT32_MAX ? INT32_MAX : (int)remaining_ms;
}

static bool is_system_time_plausible(struct tm *local_out = nullptr)
{
    time_t now;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    int year = local.tm_year + 1900;
    if (local_out) {
        *local_out = local;
    }
    return year >= kMinValidYear && year <= kMaxValidYear;
}

static bool perform_ntp_sync(int max_retries = 30)
{
    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    if (!g_ntp_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "ntp.aliyun.com");
        esp_sntp_setservername(2, "time.windows.com");
        esp_sntp_init();
        g_ntp_started = true;
    } else {
        esp_sntp_restart();
    }

    for (int retry = 0; retry < max_retries; ++retry) {
        struct tm local = {};
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
            is_system_time_plausible(&local)) {
            sync_rtc_from_system_time();
            time(&g_last_ntp_sync_time);
            xEventGroupSetBits(g_app_events, kTimeSyncedBit);
            ESP_LOGI(TAG, "ntp synced: %04d-%02d-%02d %02d:%02d:%02d",
                     local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                     local.tm_hour, local.tm_min, local.tm_sec);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "ntp sync timeout");
    return false;
}

static void sample_sensor()
{
    float temp = 0.0f;
    float humi = 0.0f;
    g_sensor_ok = g_shtc3 && g_shtc3->Shtc3_ReadTempHumi(&temp, &humi) == 0;
    if (g_sensor_ok) {
        g_temperature = temp;
        g_humidity = humi;
    }
}

static void housekeeping_task(void *)
{
    TickType_t next_sensor = 0;
    TickType_t next_battery = 0;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= next_sensor) {
            if (!g_low_battery_mode) {
                sample_sensor();
            }
            next_sensor = now + pdMS_TO_TICKS(60000);
        }
        if (now >= next_battery) {
            sample_battery();
            next_battery = now + pdMS_TO_TICKS(5 * 60 * 1000);
        }
        TickType_t next_wake = next_sensor < next_battery ? next_sensor : next_battery;
        TickType_t delay_ticks = next_wake > now ? next_wake - now : pdMS_TO_TICKS(1000);
        vTaskDelay(delay_ticks);
    }
}

struct HttpBuffer {
    char *data;
    size_t len;
    size_t cap;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data) {
        return ESP_OK;
    }
    HttpBuffer *buffer = (HttpBuffer *)evt->user_data;
    if (buffer->len + 1 >= buffer->cap) {
        return ESP_OK;
    }
    size_t room = buffer->cap - buffer->len - 1;
    size_t copy_len = evt->data_len < room ? evt->data_len : room;
    if (copy_len > 0) {
        memcpy(buffer->data + buffer->len, evt->data, copy_len);
        buffer->len += copy_len;
        buffer->data[buffer->len] = '\0';
    }
    return ESP_OK;
}

static bool gzip_payload_range(const uint8_t *data, size_t len, size_t *payload_offset, size_t *payload_len)
{
    if (len < 18 || data[0] != 0x1F || data[1] != 0x8B || data[2] != 8) {
        return false;
    }

    uint8_t flags = data[3];
    size_t pos = 10;
    if (flags & 0x04) {
        if (pos + 2 > len) return false;
        size_t extra_len = data[pos] | (data[pos + 1] << 8);
        pos += 2 + extra_len;
        if (pos > len) return false;
    }
    if (flags & 0x08) {
        while (pos < len && data[pos] != 0) ++pos;
        if (++pos > len) return false;
    }
    if (flags & 0x10) {
        while (pos < len && data[pos] != 0) ++pos;
        if (++pos > len) return false;
    }
    if (flags & 0x02) {
        pos += 2;
        if (pos > len) return false;
    }
    if (pos + 8 > len) {
        return false;
    }

    *payload_offset = pos;
    *payload_len = len - pos - 8;
    return true;
}

static esp_err_t decode_http_body(char *out, size_t out_len, size_t *body_len)
{
    if (*body_len < 3 || (uint8_t)out[0] != 0x1F || (uint8_t)out[1] != 0x8B) {
        return ESP_OK;
    }

    size_t payload_offset = 0;
    size_t payload_len = 0;
    if (!gzip_payload_range((const uint8_t *)out, *body_len, &payload_offset, &payload_len)) {
        ESP_LOGW(TAG, "gzip response header invalid len=%u", (unsigned)*body_len);
        return ESP_FAIL;
    }

    uint8_t *compressed = (uint8_t *)malloc(*body_len);
    if (!compressed) {
        ESP_LOGW(TAG, "gzip response alloc failed len=%u", (unsigned)*body_len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(compressed, out, *body_len);

    size_t written = tinfl_decompress_mem_to_mem(out,
                                                 out_len - 1,
                                                 compressed + payload_offset,
                                                 payload_len,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(compressed);
    if (written == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        out[0] = '\0';
        *body_len = 0;
        ESP_LOGW(TAG, "gzip response decompress failed payload_len=%u", (unsigned)payload_len);
        return ESP_FAIL;
    }

    out[written] = '\0';
    *body_len = written;
    ESP_LOGI(TAG, "gzip response decompressed len=%u", (unsigned)written);
    return ESP_OK;
}

static esp_err_t http_get_text(const char *url, char *out, size_t out_len, const char *api_key = nullptr)
{
    out[0] = '\0';
    HttpBuffer buffer = {out, 0, out_len};
    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.user_data = &buffer;
    int timeout_ms = g_http_timeout_ms;
    int remaining_ms = boot_sync_remaining_ms();
    if (remaining_ms <= 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (remaining_ms != INT32_MAX && timeout_ms > remaining_ms) {
        timeout_ms = remaining_ms;
    }
    config.timeout_ms = timeout_ms;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Accept", "application/json,text/plain,*/*");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (api_key && api_key[0] != '\0') {
        esp_http_client_set_header(client, "X-QW-Api-Key", api_key);
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "http get failed status=%d err=%s", status, esp_err_to_name(err));
        return err == ESP_OK ? ESP_FAIL : err;
    }
    ESP_LOGI(TAG, "http get ok status=%d len=%u gzip=%d",
             status,
             (unsigned)buffer.len,
             buffer.len >= 2 && (uint8_t)out[0] == 0x1F && (uint8_t)out[1] == 0x8B);
    return decode_http_body(out, out_len, &buffer.len);
}

static const char *qweather_api_host()
{
    return "devapi.qweather.com";
}

static void trim_ascii(char *text)
{
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
    char *start = text;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
}

static bool json_copy_string(cJSON *obj, const char *name, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(obj, name);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    strlcpy(out, item->valuestring, out_len);
    return true;
}

static bool url_is_unreserved(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
           ch == '.' || ch == '~';
}

static bool url_encode_component(const char *in, char *out, size_t out_len)
{
    static const char kHex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        if (url_is_unreserved((char)*p)) {
            if (pos + 1 >= out_len) {
                return false;
            }
            out[pos++] = (char)*p;
        } else {
            if (pos + 3 >= out_len) {
                return false;
            }
            out[pos++] = '%';
            out[pos++] = kHex[*p >> 4];
            out[pos++] = kHex[*p & 0x0F];
        }
    }
    out[pos] = '\0';
    return true;
}

static void log_response_preview(const char *stage, const char *response)
{
    char preview[121] = {};
    strlcpy(preview, response, sizeof(preview));
    for (char *p = preview; *p; ++p) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            *p = ' ';
        }
    }
    const unsigned char *bytes = (const unsigned char *)response;
    ESP_LOGW(TAG, "%s parse failed len=%u head=%02x %02x %02x %02x body=%s",
             stage,
             (unsigned)strlen(response),
             bytes[0],
             bytes[1],
             bytes[2],
             bytes[3],
             preview);
}

static bool ip_geolocation_lookup(char *location, size_t location_len, char *city, size_t city_len)
{
    char *response = (char *)malloc(2048);
    if (!response) {
        ESP_LOGW(TAG, "ip location response alloc failed");
        return false;
    }
    response[0] = '\0';
    if (http_get_text("http://ip-api.com/json/?fields=status,message,lat,lon,city&lang=zh-CN", response, 2048) != ESP_OK) {
        free(response);
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        free(response);
        return false;
    }
    bool ok = false;
    cJSON *status = cJSON_GetObjectItem(root, "status");
    cJSON *lat = cJSON_GetObjectItem(root, "lat");
    cJSON *lon = cJSON_GetObjectItem(root, "lon");
    cJSON *city_json = cJSON_GetObjectItem(root, "city");
    if (cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0 &&
        cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        snprintf(location, location_len, "%.2f,%.2f", lon->valuedouble, lat->valuedouble);
        if (cJSON_IsString(city_json) && city_json->valuestring) {
            strlcpy(city, city_json->valuestring, city_len);
        } else {
            strlcpy(city, location, city_len);
        }
        ESP_LOGI(TAG, "ip location resolved: %s city=%s", location, city);
        ok = true;
    }
    cJSON_Delete(root);
    free(response);
    return ok;
}

static bool qweather_lookup_city(const char *location,
                                 char *city_id,
                                 size_t city_id_len,
                                 char *city_name,
                                 size_t city_name_len,
                                 char *lat_out = nullptr,
                                 size_t lat_len = 0,
                                 char *lon_out = nullptr,
                                 size_t lon_len = 0)
{
    char encoded_location[128] = {};
    if (!url_encode_component(location, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "qweather city location too long");
        return false;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://geoapi.qweather.com/v2/city/lookup?location=%s&number=1&range=cn&lang=zh",
             encoded_location);
    ESP_LOGI(TAG, "qweather city lookup: %s via geoapi.qweather.com", location);
    char *response = (char *)malloc(8192);
    if (!response) {
        ESP_LOGW(TAG, "qweather city response alloc failed");
        return false;
    }
    response[0] = '\0';
    if (http_get_text(url, response, 8192, g_weather_api_key) != ESP_OK) {
        ESP_LOGW(TAG, "qweather city lookup http failed");
        free(response);
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather city", response);
        free(response);
        return false;
    }
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *locations = cJSON_GetObjectItem(root, "location");
    cJSON *first = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : nullptr;
    if (cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 && first) {
        ok = json_copy_string(first, "id", city_id, city_id_len) &&
             json_copy_string(first, "name", city_name, city_name_len);
        if (ok) {
            if (lat_out && lat_len > 0) {
                json_copy_string(first, "lat", lat_out, lat_len);
            }
            if (lon_out && lon_len > 0) {
                json_copy_string(first, "lon", lon_out, lon_len);
            }
            ESP_LOGI(TAG, "qweather city resolved: %s id=%s", city_name, city_id);
        }
    } else {
        ESP_LOGW(TAG, "qweather city lookup failed code=%s",
                 cJSON_IsString(code) ? code->valuestring : "missing");
    }
    cJSON_Delete(root);
    free(response);
    return ok;
}

static const char *warning_color_name(const char *code)
{
    if (!code) {
        return "";
    }
    if (strcmp(code, "blue") == 0) return "蓝色";
    if (strcmp(code, "yellow") == 0) return "黄色";
    if (strcmp(code, "orange") == 0) return "橙色";
    if (strcmp(code, "red") == 0) return "红色";
    if (strcmp(code, "white") == 0) return "白色";
    if (strcmp(code, "black") == 0) return "黑色";
    return "";
}

static bool qweather_fetch_alert(const char *lat, const char *lon, WeatherAlertData *alert)
{
    if (!lat || !lon || lat[0] == '\0' || lon[0] == '\0') {
        alert->active = false;
        return true;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://%s/weatheralert/v1/current/%s/%s?lang=zh&localTime=true",
             qweather_api_host(), lat, lon);
    ESP_LOGI(TAG, "qweather alert lookup: %s,%s via %s", lat, lon, qweather_api_host());
    char *response = (char *)malloc(16384);
    if (!response) {
        ESP_LOGW(TAG, "qweather alert response alloc failed");
        return false;
    }
    response[0] = '\0';
    if (http_get_text(url, response, 16384, g_weather_api_key) != ESP_OK) {
        ESP_LOGW(TAG, "qweather alert http failed");
        free(response);
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather alert", response);
        free(response);
        return false;
    }

    WeatherAlertData next = {};
    bool ok = true;
    cJSON *alerts = cJSON_GetObjectItem(root, "alerts");
    cJSON *first = cJSON_IsArray(alerts) ? cJSON_GetArrayItem(alerts, 0) : nullptr;
    if (first) {
        char event_name[24] = {};
        char color_code[16] = {};
        char headline[64] = {};
        cJSON *event = cJSON_GetObjectItem(first, "eventType");
        cJSON *color = cJSON_GetObjectItem(first, "color");
        if (event) {
            json_copy_string(event, "name", event_name, sizeof(event_name));
        }
        if (color) {
            json_copy_string(color, "code", color_code, sizeof(color_code));
        }
        json_copy_string(first, "headline", headline, sizeof(headline));
        json_copy_string(first, "icon", next.icon, sizeof(next.icon));

        const char *color_name = warning_color_name(color_code);
        if (event_name[0] != '\0' && color_name[0] != '\0') {
            snprintf(next.title, sizeof(next.title), "%s%s预警", event_name, color_name);
        } else if (headline[0] != '\0') {
            strlcpy(next.title, headline, sizeof(next.title));
        } else if (event_name[0] != '\0') {
            snprintf(next.title, sizeof(next.title), "%s预警", event_name);
        }
        next.active = next.title[0] != '\0';
    }
    time(&next.updated_at);
    *alert = next;

    cJSON_Delete(root);
    free(response);
    return ok;
}

static bool qweather_fetch_now(const char *city_id, WeatherData *weather)
{
    char encoded_location[128] = {};
    if (!url_encode_component(city_id, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "qweather now location too long");
        return false;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://%s/v7/weather/now?location=%s&lang=zh&unit=m",
             qweather_api_host(), encoded_location);
    ESP_LOGI(TAG, "qweather now lookup: %s via %s", city_id, qweather_api_host());
    char *response = (char *)malloc(8192);
    if (!response) {
        ESP_LOGW(TAG, "qweather now response alloc failed");
        return false;
    }
    response[0] = '\0';
    if (http_get_text(url, response, 8192, g_weather_api_key) != ESP_OK) {
        ESP_LOGW(TAG, "qweather now http failed");
        free(response);
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather now", response);
        free(response);
        return false;
    }
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *now = cJSON_GetObjectItem(root, "now");
    if (cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 && now) {
        ok = json_copy_string(now, "text", weather->text, sizeof(weather->text)) &&
             json_copy_string(now, "icon", weather->icon, sizeof(weather->icon)) &&
             json_copy_string(now, "temp", weather->temp, sizeof(weather->temp)) &&
             json_copy_string(now, "humidity", weather->humidity, sizeof(weather->humidity));
    } else {
        ESP_LOGW(TAG, "qweather now failed code=%s",
                 cJSON_IsString(code) ? code->valuestring : "missing");
    }
    cJSON_Delete(root);
    free(response);
    return ok;
}

static bool perform_weather_update()
{
    if (!g_have_weather_key || g_low_battery_mode) {
        xEventGroupClearBits(g_app_events, kWeatherReadyBit);
        return false;
    }

    char location[32] = {};
    char city_id[24] = {};
    char ip_city[32] = {};
    char lookup_city[32] = {};
    WeatherData next = {};
    if (ip_geolocation_lookup(location, sizeof(location), ip_city, sizeof(ip_city))) {
        trim_ascii(location);
        bool have_city_id = qweather_lookup_city(location,
                                                 city_id,
                                                 sizeof(city_id),
                                                 lookup_city,
                                                 sizeof(lookup_city),
                                                 next.lat,
                                                 sizeof(next.lat),
                                                 next.lon,
                                                 sizeof(next.lon));
        if (!have_city_id && ip_city[0] != '\0') {
            ESP_LOGW(TAG, "retry qweather city lookup by ip city: %s", ip_city);
            have_city_id = qweather_lookup_city(ip_city,
                                                city_id,
                                                sizeof(city_id),
                                                lookup_city,
                                                sizeof(lookup_city),
                                                next.lat,
                                                sizeof(next.lat),
                                                next.lon,
                                                sizeof(next.lon));
        }
        strlcpy(next.city, ip_city[0] ? ip_city : (lookup_city[0] ? lookup_city : location), sizeof(next.city));
        if (!have_city_id) {
            strlcpy(city_id, location, sizeof(city_id));
            char *comma = strchr(location, ',');
            if (comma) {
                size_t lon_len = comma - location;
                if (lon_len >= sizeof(next.lon)) {
                    lon_len = sizeof(next.lon) - 1;
                }
                memcpy(next.lon, location, lon_len);
                next.lon[lon_len] = '\0';
                strlcpy(next.lat, comma + 1, sizeof(next.lat));
            }
            ESP_LOGW(TAG, "using ip coordinates for weather now: %s", city_id);
        }
        if (qweather_fetch_now(city_id, &next)) {
            g_weather = next;
            if (!qweather_fetch_alert(g_weather.lat, g_weather.lon, &g_weather_alert)) {
                g_weather_alert = {};
            }
            time(&g_last_weather_sync_time);
            xEventGroupSetBits(g_app_events, kWeatherReadyBit);
            ESP_LOGI(TAG, "weather updated: %s %s %sC %s%% icon=%s",
                     g_weather.city, g_weather.text, g_weather.temp, g_weather.humidity, g_weather.icon);
            return true;
        } else {
            ESP_LOGW(TAG, "weather update failed after ip lookup");
        }
    } else {
        ESP_LOGW(TAG, "ip geolocation lookup failed");
    }
    return false;
}

static uint32_t weather_icon_codepoint(const char *code)
{
    int icon = atoi(code);
    if (icon >= 100 && icon <= 104) {
        return 0xF101 + (uint32_t)(icon - 100);
    }
    if (icon >= 150 && icon <= 153) {
        return 0xF106 + (uint32_t)(icon - 150);
    }
    if (icon >= 300 && icon <= 318) {
        return 0xF10A + (uint32_t)(icon - 300);
    }
    if (icon >= 350 && icon <= 351) {
        return 0xF11D + (uint32_t)(icon - 350);
    }
    if (icon == 399) {
        return 0xF11F;
    }
    if (icon >= 400 && icon <= 410) {
        return 0xF120 + (uint32_t)(icon - 400);
    }
    if (icon >= 456 && icon <= 457) {
        return 0xF12B + (uint32_t)(icon - 456);
    }
    if (icon == 499) {
        return 0xF12D;
    }
    if (icon >= 500 && icon <= 504) {
        return 0xF12E + (uint32_t)(icon - 500);
    }
    if (icon >= 507 && icon <= 515) {
        return 0xF133 + (uint32_t)(icon - 507);
    }
    if (icon >= 800 && icon <= 807) {
        return 0xF13C + (uint32_t)(icon - 800);
    }
    if (icon == 900) {
        return 0xF144;
    }
    if (icon == 901) {
        return 0xF145;
    }
    if (icon == 9999) {
        return 0xF1CB;
    }
    return 0xF146;
}

static const char *weather_icon_text(const char *code)
{
    static char text[5];
    uint32_t cp = weather_icon_codepoint(code);
    if (cp <= 0x7F) {
        text[0] = (char)cp;
        text[1] = '\0';
    } else if (cp <= 0x7FF) {
        text[0] = (char)(0xC0 | (cp >> 6));
        text[1] = (char)(0x80 | (cp & 0x3F));
        text[2] = '\0';
    } else if (cp <= 0xFFFF) {
        text[0] = (char)(0xE0 | (cp >> 12));
        text[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        text[2] = (char)(0x80 | (cp & 0x3F));
        text[3] = '\0';
    } else {
        text[0] = (char)(0xF0 | (cp >> 18));
        text[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        text[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        text[3] = (char)(0x80 | (cp & 0x3F));
        text[4] = '\0';
    }
    return text;
}

static bool wait_for_wifi_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(
        g_app_events,
        kWifiConnectedBit,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & kWifiConnectedBit) != 0;
}

static bool is_time_valid(struct tm *local_out = nullptr)
{
    return is_system_time_plausible(local_out);
}

static void run_boot_connectivity_sync()
{
    if (!g_have_wifi_creds) {
        char detail[64];
        snprintf(detail, sizeof(detail), "Setup AP: %s", g_ap_ssid);
        update_boot_screen(100, "Setup mode", detail);
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    update_boot_screen(18, "Connecting Wi-Fi", g_wifi_ssid);
    acquire_network_awake_lock();
    g_boot_sync_deadline_us = esp_timer_get_time() + (int64_t)kBootStartupBudgetMs * 1000;
    start_wifi_radio(false);
    int remaining_ms = boot_sync_remaining_ms();
    uint32_t wifi_timeout_ms = remaining_ms > 0 && remaining_ms < kBootWifiConnectTimeoutMs
                                   ? remaining_ms
                                   : kBootWifiConnectTimeoutMs;
    if (!wait_for_wifi_connected(wifi_timeout_ms)) {
        update_boot_screen(100, "Wi-Fi timeout", "Check SSID or password");
        vTaskDelay(pdMS_TO_TICKS(200));
        stop_wifi_radio();
        release_network_awake_lock();
        g_boot_sync_deadline_us = 0;
        return;
    }

    update_boot_screen(42, "Wi-Fi connected", "Loading weather");
    remaining_ms = boot_sync_remaining_ms();
    if (g_have_weather_key && !g_low_battery_mode && remaining_ms > 250) {
        bool weather_ok = false;
        int previous_timeout = g_http_timeout_ms;
        g_http_timeout_ms = kHttpBootTimeoutMs;
        update_boot_screen(58, "Loading weather", "Fetching API data");
        weather_ok = perform_weather_update();
        g_http_timeout_ms = previous_timeout;
        update_boot_screen(weather_ok ? 76 : 68,
                           weather_ok ? "Weather ready" : "Weather retry later",
                           weather_ok ? "Synchronizing time" : "Will sync in background");
    } else if (g_have_weather_key && !g_low_battery_mode) {
        update_boot_screen(68, "Weather retry later", "Starting clock");
    } else if (g_low_battery_mode) {
        update_boot_screen(58, "Weather skipped", "Low battery");
    } else {
        update_boot_screen(58, "Weather skipped", "API Key not configured");
    }

    bool ntp_ok = false;
    remaining_ms = boot_sync_remaining_ms();
    if (remaining_ms > 600) {
        update_boot_screen(82, "Synchronizing time", "Short NTP check");
        ntp_ok = perform_ntp_sync(kBootNtpRetries);
    }
    update_boot_screen(100,
                       ntp_ok ? "Time synchronized" : "NTP retry later",
                       "Starting clock");

    vTaskDelay(pdMS_TO_TICKS(200));
    stop_wifi_radio();
    release_network_awake_lock();
    g_boot_sync_deadline_us = 0;
}

static void boot_connectivity_task(void *)
{
    run_boot_connectivity_sync();
    g_boot_sync_task_handle = nullptr;
    vTaskDelete(nullptr);
}

static void network_sync_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(2500));
    EventBits_t initial_bits = xEventGroupGetBits(g_app_events);
    bool boot_ntp_due = (initial_bits & kTimeSyncedBit) == 0;
    time_t next_weather_at = 0;
    if (initial_bits & kWeatherReadyBit) {
        time(&next_weather_at);
        next_weather_at += 60 * 60;
    }
    time_t next_ntp_retry_at = 0;
    int last_midnight_ntp_yday = -1;

    for (;;) {
        EventBits_t loop_bits = xEventGroupGetBits(g_app_events);
        bool provisioning_sync_due = (loop_bits & kProvisioningSyncBit) != 0;
        bool manual_ntp_due = (loop_bits & kManualNtpSyncBit) != 0;
        bool manual_weather_due = (loop_bits & kManualWeatherSyncBit) != 0;
        if (!g_have_wifi_creds) {
            if (manual_ntp_due) {
                finish_settings_sync(kSettingsSyncNtp, "未配置 WiFi");
                xEventGroupClearBits(g_app_events, kManualNtpSyncBit);
            }
            if (manual_weather_due) {
                finish_settings_sync(kSettingsSyncWeather, "未配置 WiFi");
                xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (manual_weather_due && !g_have_weather_key) {
            finish_settings_sync(kSettingsSyncWeather, "未配置 API Key");
            xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (g_setup_portal_active && !provisioning_sync_due && !manual_ntp_due && !manual_weather_due) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        struct tm local = {};
        bool time_valid = is_time_valid(&local);
        bool midnight_ntp_due = time_valid &&
                                local.tm_hour == 0 &&
                                local.tm_min == 0 &&
                                local.tm_yday != last_midnight_ntp_yday;
        time_t now;
        time(&now);
        bool weather_due = g_have_weather_key && !g_low_battery_mode &&
                           (manual_weather_due || provisioning_sync_due || next_weather_at == 0 || now >= next_weather_at);
        if (g_low_battery_mode && manual_weather_due) {
            finish_settings_sync(kSettingsSyncWeather, "电量低，已跳过");
            xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        bool ntp_due = (manual_ntp_due || provisioning_sync_due || boot_ntp_due || midnight_ntp_due) && now >= next_ntp_retry_at;

        if (!ntp_due && !weather_due) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        ESP_LOGI(TAG, "wifi radio on for sync: ntp=%d weather=%d", ntp_due, weather_due);
        acquire_network_awake_lock();
        start_wifi_radio(false);
        if (wait_for_wifi_connected(45000)) {
            bool ntp_ok = false;
            bool weather_ok = false;
            if (ntp_due) {
                if (perform_ntp_sync()) {
                    ntp_ok = true;
                    boot_ntp_due = false;
                    next_ntp_retry_at = 0;
                    if (is_time_valid(&local) && local.tm_hour == 0) {
                        last_midnight_ntp_yday = local.tm_yday;
                    }
                } else {
                    time(&next_ntp_retry_at);
                    next_ntp_retry_at += 5 * 60;
                }
            }
            if (weather_due) {
                weather_ok = perform_weather_update();
                time(&next_weather_at);
                next_weather_at += 60 * 60;
            }
            if (provisioning_sync_due) {
                xEventGroupClearBits(g_app_events, kProvisioningSyncBit);
            }
            if (manual_ntp_due) {
                finish_settings_sync(kSettingsSyncNtp, ntp_ok ? "时间同步完成" : "时间同步失败");
                xEventGroupClearBits(g_app_events, kManualNtpSyncBit);
            }
            if (manual_weather_due) {
                finish_settings_sync(kSettingsSyncWeather, weather_ok ? "天气同步完成" : "天气同步失败");
                xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            }
        } else {
            ESP_LOGW(TAG, "wifi connect timeout during sync window");
            if (manual_ntp_due) {
                finish_settings_sync(kSettingsSyncNtp, "时间同步失败");
                xEventGroupClearBits(g_app_events, kManualNtpSyncBit);
            }
            if (manual_weather_due) {
                finish_settings_sync(kSettingsSyncWeather, "天气同步失败");
                xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
            }
            if (ntp_due) {
                time(&next_ntp_retry_at);
                next_ntp_retry_at += 5 * 60;
            }
            if (weather_due) {
                time(&next_weather_at);
                next_weather_at += 60 * 60;
            }
        }
        stop_wifi_radio(provisioning_sync_due);
        release_network_awake_lock();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void play_hourly_chime(int hour)
{
    ESP_LOGI(TAG, "hourly chime trigger: %02d:00 (audio driver placeholder)", hour);
}

static void update_time_ui(const struct tm &local)
{
    static int last_chime_hour_key = -1;
    int minute_key = local.tm_hour * 60 + local.tm_min;
    if (minute_key != g_last_ui_minute) {
        draw_time_canvas(local);
        if (!g_low_battery_mode) {
            int day_seconds = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
            int day_filled = (day_seconds * 60) / (24 * 3600);
            update_progress_canvas(g_day_progress_canvas, day_filled, &g_last_day_progress_filled);
        }
        g_last_ui_minute = minute_key;
    }
    if (!g_low_battery_mode && local.tm_sec != g_last_ui_second) {
        draw_second_canvas(local);
        draw_status_gif_frame(local.tm_sec % STATUS_GIF_FRAME_COUNT);
        update_progress_canvas(g_second_progress_canvas, local.tm_sec + 1, &g_last_second_progress_filled);
        g_last_ui_second = local.tm_sec;
    }

    static const char *week_days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    char date[48];
    snprintf(date, sizeof(date), "%04d/%02d/%02d / %s",
             local.tm_year + 1900,
             local.tm_mon + 1,
             local.tm_mday,
             week_days[local.tm_wday]);
    set_label_text_if_changed(g_date_label, date);

    int hour_key = local.tm_yday * 24 + local.tm_hour;
    if (g_hourly_chime_enabled && local.tm_min == 0 && local.tm_sec <= 2 && hour_key != last_chime_hour_key) {
        last_chime_hour_key = hour_key;
        play_hourly_chime(local.tm_hour);
    }
}

static void handle_settings_action()
{
    int selected = g_settings_selection;
    if (selected < 0 || selected > 3) {
        selected = 0;
    }
    g_settings_last_activity_tick = xTaskGetTickCount();
    if (is_settings_sync_busy()) {
        set_settings_feedback("请等待同步完成", 2000);
        return;
    }
    if (selected != 3) {
        g_factory_reset_confirm_pending = false;
    }
    switch (selected) {
    case 0:
        g_hourly_chime_enabled = !g_hourly_chime_enabled;
        save_hourly_chime_setting();
        set_settings_feedback(g_hourly_chime_enabled ? "整点报时 ON" : "整点报时 OFF", 2500);
        ESP_LOGI(TAG, "hourly chime %s", g_hourly_chime_enabled ? "enabled" : "disabled");
        break;
    case 1:
        begin_settings_sync(kSettingsSyncNtp, "正在同步时间...");
        ESP_LOGI(TAG, "manual ntp sync requested");
        xEventGroupSetBits(g_app_events, kManualNtpSyncBit);
        break;
    case 2:
        begin_settings_sync(kSettingsSyncWeather, "正在同步天气...");
        ESP_LOGI(TAG, "manual weather sync requested");
        xEventGroupSetBits(g_app_events, kManualWeatherSyncBit);
        break;
    case 3:
        if (!g_factory_reset_confirm_pending) {
            g_factory_reset_confirm_pending = true;
            set_settings_feedback("再次按 BOOT 确认", kSettingsTimeoutMs);
            ESP_LOGW(TAG, "factory reset confirmation requested");
            break;
        }
        ESP_LOGW(TAG, "factory reset requested from settings");
        g_settings_requested = false;
        g_factory_reset_confirm_pending = false;
        clear_saved_config();
        start_wifi_radio(true);
        break;
    default:
        break;
    }
}

static void ui_task(void *)
{
    TickType_t last_status_update = xTaskGetTickCount() - pdMS_TO_TICKS(10000);
    uint32_t last_battery_version = (uint32_t)-1;
    bool info_page_visible = false;
    bool settings_page_visible = false;
    bool setup_panel_visible = false;
    bool low_mode_visible = false;
    bool alert_visible = false;
    uint32_t last_settings_action_seq = g_settings_action_seq;

    auto delay_to_next_second = []() {
        int64_t us = esp_timer_get_time();
        int64_t until_next = 1000000 - (us % 1000000);
        int delay_ms = (int)(until_next / 1000) + 5;
        if (delay_ms < 10) {
            delay_ms = 10;
        } else if (delay_ms > 1005) {
            delay_ms = 1005;
        }
        return pdMS_TO_TICKS(delay_ms);
    };

    for (;;) {
        time_t now;
        time(&now);
        struct tm local = {};
        localtime_r(&now, &local);

        TickType_t tick_now = xTaskGetTickCount();
        bool status_due = tick_now - last_status_update >= pdMS_TO_TICKS(10000);
        bool battery_due = g_battery_version != last_battery_version;
        bool setup_due = g_setup_portal_active != setup_panel_visible;
        bool mode_due = g_low_battery_mode != low_mode_visible;

        if (Lvgl_lock(80)) {
            bool refresh_now = false;
            bool info_requested = g_boot_info_requested;
            bool settings_requested = g_settings_requested;
            if (info_requested && !settings_requested) {
                if (!info_page_visible) {
                    build_boot_info_page();
                    show_page(g_info_root);
                    info_page_visible = true;
                    settings_page_visible = false;
                }
                update_boot_info_page();
                lv_refr_now(nullptr);
                Lvgl_unlock();
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            if (info_page_visible) {
                show_page(g_clock_root);
                info_page_visible = false;
                setup_panel_visible = false;
                low_mode_visible = g_low_battery_mode;
                apply_clock_mode_visibility(false);
                status_due = true;
                battery_due = true;
                g_last_ui_second = -1;
                g_last_ui_minute = -1;
                refresh_now = true;
            }

            if (settings_requested) {
                if (!settings_page_visible) {
                    build_settings_page();
                    show_page(g_settings_root);
                    settings_page_visible = true;
                    info_page_visible = false;
                    setup_panel_visible = false;
                }
                if (g_settings_action_seq != last_settings_action_seq) {
                    last_settings_action_seq = g_settings_action_seq;
                    handle_settings_action();
                    settings_requested = g_settings_requested;
                }
                if (settings_requested && is_settings_sync_busy()) {
                    TickType_t deadline = g_settings_sync_deadline_tick;
                    if (deadline != 0 && tick_now >= deadline) {
                        int op = g_settings_sync_op;
                        ESP_LOGW(TAG, "settings manual sync timeout: op=%d", op);
                        if (op == kSettingsSyncNtp) {
                            xEventGroupClearBits(g_app_events, kManualNtpSyncBit);
                            finish_settings_sync(kSettingsSyncNtp, "时间同步超时");
                        } else if (op == kSettingsSyncWeather) {
                            xEventGroupClearBits(g_app_events, kManualWeatherSyncBit);
                            finish_settings_sync(kSettingsSyncWeather, "天气同步超时");
                        } else {
                            g_settings_sync_op = kSettingsSyncNone;
                            g_settings_sync_deadline_tick = 0;
                        }
                    }
                }
                if (settings_requested) {
                    TickType_t last_activity = g_settings_last_activity_tick;
                    if (!is_settings_sync_busy() && last_activity != 0 && tick_now - last_activity >= pdMS_TO_TICKS(kSettingsTimeoutMs)) {
                        ESP_LOGI(TAG, "settings timeout, returning to clock");
                        g_settings_requested = false;
                        settings_requested = false;
                    }
                }
                if (settings_requested) {
                    update_settings_page();
                    lv_refr_now(nullptr);
                    Lvgl_unlock();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
            }

            if (settings_page_visible) {
                show_page(g_clock_root);
                settings_page_visible = false;
                setup_panel_visible = false;
                low_mode_visible = g_low_battery_mode;
                apply_clock_mode_visibility(false);
                status_due = true;
                battery_due = true;
                g_last_ui_second = -1;
                g_last_ui_minute = -1;
                refresh_now = true;
            }

            if (is_system_time_plausible(&local)) {
                int previous_second = g_last_ui_second;
                int previous_minute = g_last_ui_minute;
                update_time_ui(local);
                if (g_last_ui_second != previous_second || g_last_ui_minute != previous_minute) {
                    refresh_now = true;
                }
                bool next_alert_visible = !g_low_battery_mode && g_weather_alert.active && (local.tm_sec % 2 == 0);
                if (next_alert_visible != alert_visible || (next_alert_visible && status_due)) {
                    update_alert_pill(next_alert_visible);
                    alert_visible = next_alert_visible;
                    refresh_now = true;
                }
            } else {
                set_label_text_if_changed(g_date_label, "----/--/-- / 星期-");
                update_alert_pill(false);
                alert_visible = false;
                refresh_now = true;
            }

            if (status_due || battery_due || setup_due || mode_due) {
                EventBits_t bits = xEventGroupGetBits(g_app_events);
                bool setup_active = g_setup_portal_active;
                if (setup_active != setup_panel_visible || mode_due) {
                    apply_clock_mode_visibility(setup_active);
                    setup_panel_visible = setup_active;
                    low_mode_visible = g_low_battery_mode;
                    status_due = true;
                    g_last_ui_second = -1;
                    g_last_ui_minute = -1;
                    g_last_day_progress_filled = -1;
                    g_last_second_progress_filled = -1;
                    update_alert_pill(false);
                    alert_visible = false;
                    refresh_now = true;
                }
                if (setup_active) {
                    update_setup_status_panel();
                }
                char temp[32];
                char humi[32];
                if (g_sensor_ok) {
                    snprintf(temp, sizeof(temp), "温度 %.1f℃", g_temperature);
                    snprintf(humi, sizeof(humi), "湿度 %.1f%%", g_humidity);
                } else {
                    snprintf(temp, sizeof(temp), "温度 --.-℃");
                    snprintf(humi, sizeof(humi), "湿度 --.-%%");
                }

                if (!setup_active && !g_low_battery_mode) {
                    set_label_text_if_changed(g_temp_label, temp);
                    set_label_text_if_changed(g_humi_label, humi);
                    if (bits & kWeatherReadyBit) {
                        char city[48];
                        char temp[24];
                        char humi[24];
                        snprintf(city, sizeof(city), "%s", g_weather.city);
                        snprintf(temp, sizeof(temp), "%s℃", g_weather.temp);
                        snprintf(humi, sizeof(humi), "%s%%", g_weather.humidity);
                        set_label_text_if_changed(g_weather_city_label, city);
                        set_label_text_if_changed(g_weather_info_label, g_weather.text);
                        set_label_text_if_changed(g_weather_temp_label, temp);
                        set_label_text_if_changed(g_weather_humi_label, humi);
                        set_label_text_if_changed(g_weather_icon_label, weather_icon_text(g_weather.icon));
                    } else if (g_have_weather_key) {
                        set_label_text_if_changed(g_weather_city_label, "--");
                        set_label_text_if_changed(g_weather_info_label, (bits & kWifiConnectedBit) ? "天气同步中" : "天气等待");
                        set_label_text_if_changed(g_weather_temp_label, "--℃");
                        set_label_text_if_changed(g_weather_humi_label, "--%");
                        set_label_text_if_changed(g_weather_icon_label, weather_icon_text("999"));
                    } else {
                        set_label_text_if_changed(g_weather_city_label, "--");
                        set_label_text_if_changed(g_weather_info_label, "设置 API Key");
                        set_label_text_if_changed(g_weather_temp_label, "--℃");
                        set_label_text_if_changed(g_weather_humi_label, "--%");
                        set_label_text_if_changed(g_weather_icon_label, weather_icon_text("999"));
                    }
                }
                if (battery_due) {
                    update_battery_icon(g_battery_percent);
                    last_battery_version = g_battery_version;
                }
                if (status_due) {
                    last_status_update = tick_now;
                }
                refresh_now = true;
            }
            if (refresh_now) {
                lv_refr_now(nullptr);
            }
            Lvgl_unlock();
        }
        vTaskDelay(delay_to_next_second());
    }
}

static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    struct FlushRange {
        int x1;
        int x2;
    };
    static FlushRange ranges[kMaxFlushRanges];
    static int range_count = 0;
    static bool force_full_refresh = false;

    int area_x1 = area->x1 < 0 ? 0 : area->x1;
    int area_x2 = area->x2 >= kDisplayWidth ? kDisplayWidth - 1 : area->x2;
    if (area_x1 <= area_x2) {
        area_x1 &= ~1;
        area_x2 |= 1;
        if (area_x2 >= kDisplayWidth) {
            area_x2 = kDisplayWidth - 1;
        }
        if (area_x2 - area_x1 + 1 >= kDisplayPartialMaxWidth) {
            force_full_refresh = true;
        } else {
            bool merged = false;
            for (int i = 0; i < range_count; ++i) {
                if (area_x1 <= ranges[i].x2 + kFlushRangeMergeGap &&
                    area_x2 >= ranges[i].x1 - kFlushRangeMergeGap) {
                    if (area_x1 < ranges[i].x1) ranges[i].x1 = area_x1;
                    if (area_x2 > ranges[i].x2) ranges[i].x2 = area_x2;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                if (range_count < kMaxFlushRanges) {
                    ranges[range_count++] = {area_x1, area_x2};
                } else {
                    force_full_refresh = true;
                }
            }
        }
    }

    uint16_t *buffer = (uint16_t *)color_map;
    constexpr uint16_t kRlcdBlackThreshold = 0xC618;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            if (x >= 0 && x < kDisplayWidth && y >= 0 && y < kDisplayHeight) {
                uint8_t color = (*buffer < kRlcdBlackThreshold) ? ColorBlack : ColorWhite;
                g_display.RLCD_SetPixel(x, y, color);
            }
            buffer++;
        }
    }
    if (lv_disp_flush_is_last(drv)) {
        int covered_width = 0;
        for (int i = 0; i < range_count; ++i) {
            covered_width += ranges[i].x2 - ranges[i].x1 + 1;
        }
        if (force_full_refresh || range_count == 0 || covered_width >= kDisplayPartialMaxWidth) {
            g_display.RLCD_Display();
        } else {
            for (int i = 0; i < range_count; ++i) {
                g_display.RLCD_DisplayXRange(ranges[i].x1, ranges[i].x2);
            }
        }
        range_count = 0;
        force_full_refresh = false;
    }
    lv_disp_flush_ready(drv);
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    g_app_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    init_power_management();

    g_have_wifi_creds = load_saved_config();
    Rtc_Setup(&g_i2c, 0x51);
    setenv("TZ", "CST-8", 1);
    tzset();
    restore_system_time_from_rtc();
    g_shtc3 = new Shtc3Port(g_i2c);
    sample_battery();
    init_wifi();

    g_display.RLCD_Init();
    g_display.RLCD_ColorClear(ColorWhite);
    g_display.RLCD_Display();
    Lvgl_PortInit(kDisplayWidth, kDisplayHeight, flush_callback);
    if (Lvgl_lock(-1)) {
        show_boot_screen();
        Lvgl_unlock();
    }
    g_boot_anim_current_frame = 0;
    g_boot_anim_running = true;
    xTaskCreatePinnedToCore(boot_anim_task, "boot_anim_task", 4096, nullptr, 4, &g_boot_anim_task_handle, 1);
    xTaskCreatePinnedToCore(boot_connectivity_task,
                            "boot_sync",
                            20480,
                            nullptr,
                            4,
                            &g_boot_sync_task_handle,
                            0);
    int waited_ms = 0;
    while (g_boot_sync_task_handle && waited_ms < kBootStartupBudgetMs + 500) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
    update_boot_screen(100, "Ready", "Starting clock");
    g_boot_anim_running = false;
    while (g_boot_anim_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    finish_boot_anim_to_last_frame();
    finish_boot_screen();

    xTaskCreatePinnedToCore(network_sync_task, "network_sync", 20480, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(housekeeping_task, "housekeeping", 5120, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(ui_task, "ui_task", 6144, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(button_task, "button_task", 3072, nullptr, 2, nullptr, 1);
}
