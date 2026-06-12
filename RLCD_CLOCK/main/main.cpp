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
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
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

LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_font_16);

static const char *TAG = "WeatherClock";

static constexpr int kDisplayWidth = 400;
static constexpr int kDisplayHeight = 300;
static constexpr int kWifiConnectedBit = BIT0;
static constexpr int kTimeSyncedBit = BIT1;
static constexpr int kWeatherReadyBit = BIT2;

static DisplayPort g_display(12, 11, 5, 40, 41, kDisplayWidth, kDisplayHeight);
static I2cMasterBus g_i2c(14, 13, 0);
static Shtc3Port *g_shtc3 = nullptr;
static i2c_master_dev_handle_t g_battery_dev = nullptr;
static EventGroupHandle_t g_app_events;
static httpd_handle_t g_http_server = nullptr;

static char g_wifi_ssid[33] = {};
static char g_wifi_pass[65] = {};
static char g_weather_api_key[96] = {};
static char g_ap_ssid[33] = {};
static bool g_have_wifi_creds = false;
static bool g_have_weather_key = false;
static bool g_ntp_started = false;
static bool g_wifi_radio_on = false;
static bool g_wifi_stop_requested = false;
static bool g_setup_portal_active = false;
static int g_last_wifi_disconnect_reason = 0;
static float g_temperature = 0.0f;
static float g_humidity = 0.0f;
static bool g_sensor_ok = false;
static int g_battery_percent = -1;

struct WeatherData {
    char city[32] = {};
    char text[32] = {};
    char icon[8] = {};
    char temp[8] = {};
    char humidity[8] = {};
};

static WeatherData g_weather;

struct SegDigit {
    lv_obj_t *seg[7] = {};
};

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

static void apply_station_config(bool reconnect);
static bool wait_for_wifi_connected(uint32_t timeout_ms);

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
    if (value < 0 || value > 9) {
        for (int i = 0; i < 7; ++i) {
            set_obj_black(digit.seg[i], false);
        }
        return;
    }
    for (int i = 0; i < 7; ++i) {
        set_obj_black(digit.seg[i], map[value][i]);
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
    lv_obj_set_style_text_font(label, &zh_font_16, LV_PART_MAIN);
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
    static int last_percent = -999;
    if (percent == last_percent) {
        return;
    }
    last_percent = percent;

    int filled = 0;
    if (percent >= 0) {
        if (percent > 100) {
            percent = 100;
        }
        filled = percent;
    }
    int width = (filled * 40 + 99) / 100;
    if (filled > 0 && width < 2) {
        width = 2;
    }
    lv_obj_set_width(g_battery_fill, width);
}

static void build_clock_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    g_date_label = make_label(screen, 20, 18, 190, 28, "----/--/--");
    g_week_label = make_label(screen, 242, 18, 62, 28, "---");
    build_battery_icon(screen);
    g_temp_label = make_label(screen, 20, 232, 150, 24, "本地 --.-℃");
    g_humi_label = make_label(screen, 200, 232, 150, 24, "湿度 --.-%");
    g_weather_city_label = make_label(screen, 20, 202, 120, 22, "城市 --");
    g_weather_info_label = make_label(screen, 150, 202, 190, 22, "天气等待");
    g_weather_icon_label = make_label(screen, 344, 186, 42, 44, "");
    lv_obj_set_style_text_font(g_weather_icon_label, &qweather_icons_36, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_weather_icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_wifi_label = make_label(screen, 20, 270, 220, 24, "AP 192.168.4.1");
    g_sync_label = make_label(screen, 265, 270, 120, 24, "NTP WAIT");

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
    nvs_close(nvs);
    g_have_weather_key = key_err == ESP_OK && g_weather_api_key[0] != '\0';
    return ssid_err == ESP_OK && pass_err == ESP_OK && g_wifi_ssid[0] != '\0';
}

static void save_config(const char *ssid, const char *pass, const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("wifi", NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "pass", pass));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "api_key", api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));
    strlcpy(g_wifi_pass, pass, sizeof(g_wifi_pass));
    strlcpy(g_weather_api_key, api_key, sizeof(g_weather_api_key));
    g_have_wifi_creds = true;
    g_have_weather_key = g_weather_api_key[0] != '\0';
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
    char body[384] = {};
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
    return send_save_result_page(req, saved, connected);
}

static esp_err_t save_get_handler(httpd_req_t *req)
{
    char query[384] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Missing query.");
    }
    bool saved = save_credentials_from_body(query);
    bool connected = saved && wait_for_wifi_connected(12000);
    return send_save_result_page(req, saved, connected);
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
    g_wifi_radio_on = true;
    if (enable_setup_portal) {
        start_http_server();
    }
}

static void stop_wifi_radio()
{
    if (!g_wifi_radio_on || !g_have_wifi_creds) {
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
    strlcpy((char *)ap_config.ap.password, "12345678", sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(g_ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (g_have_wifi_creds) {
        apply_station_config(false);
    }

    start_wifi_radio(!g_have_wifi_creds);
}

static void restore_system_time_from_rtc()
{
    rtcTimeStruct_t rtc_time = {};
    Rtc_GetTime(&rtc_time);
    if (rtc_time.year < 2024 || rtc_time.year > 2099) {
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
    struct timeval now = {};
    now.tv_sec = epoch;
    settimeofday(&now, nullptr);
}

static void sync_rtc_from_system_time()
{
    time_t now;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    Rtc_SetTime(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

static void init_battery_gauge()
{
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = 0x55;
    dev_cfg.scl_speed_hz = 400000;
    esp_err_t err = i2c_master_bus_add_device(g_i2c.Get_I2cBusHandle(), &dev_cfg, &g_battery_dev);
    if (err != ESP_OK) {
        g_battery_dev = nullptr;
        ESP_LOGW(TAG, "battery gauge init failed: %s", esp_err_to_name(err));
    }
}

static bool read_battery_percent(int *percent)
{
    if (!g_battery_dev) {
        return false;
    }
    uint8_t reg = 0x2C;
    uint8_t data[2] = {};
    esp_err_t err = (esp_err_t)g_i2c.i2c_master_write_read_dev(g_battery_dev, &reg, 1, data, sizeof(data));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery soc read failed: %s", esp_err_to_name(err));
        return false;
    }
    int soc = data[0] | (data[1] << 8);
    if (soc < 0 || soc > 100) {
        ESP_LOGW(TAG, "battery soc out of range: %d", soc);
        return false;
    }
    *percent = soc;
    return true;
}

static void battery_task(void *)
{
    for (;;) {
        int percent = -1;
        if (read_battery_percent(&percent)) {
            g_battery_percent = percent;
        } else {
            g_battery_percent = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

static bool perform_ntp_sync()
{
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

    for (int retry = 0; retry < 30; ++retry) {
        time_t now;
        time(&now);
        struct tm local = {};
        localtime_r(&now, &local);
        if (local.tm_year + 1900 >= 2024) {
            sync_rtc_from_system_time();
            xEventGroupSetBits(g_app_events, kTimeSyncedBit);
            ESP_LOGI(TAG, "ntp synced");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "ntp sync timeout");
    return false;
}

static void sensor_task(void *)
{
    for (;;) {
        float temp = 0.0f;
        float humi = 0.0f;
        g_sensor_ok = g_shtc3 && g_shtc3->Shtc3_ReadTempHumi(&temp, &humi) == 0;
        if (g_sensor_ok) {
            g_temperature = temp;
            g_humidity = humi;
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
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
    size_t room = buffer->cap - buffer->len - 1;
    size_t copy_len = evt->data_len < room ? evt->data_len : room;
    if (copy_len > 0) {
        memcpy(buffer->data + buffer->len, evt->data, copy_len);
        buffer->len += copy_len;
        buffer->data[buffer->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_get_text(const char *url, char *out, size_t out_len)
{
    out[0] = '\0';
    HttpBuffer buffer = {out, 0, out_len};
    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.user_data = &buffer;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Accept", "application/json,text/plain,*/*");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "http get failed status=%d err=%s", status, esp_err_to_name(err));
        return err == ESP_OK ? ESP_FAIL : err;
    }
    return ESP_OK;
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
    char response[1024] = {};
    if (http_get_text("http://ip-api.com/json/?fields=status,message,lat,lon,city&lang=zh-CN", response, sizeof(response)) != ESP_OK) {
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
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
    return ok;
}

static bool qweather_lookup_city(const char *location, char *city_id, size_t city_id_len, char *city_name, size_t city_name_len)
{
    char encoded_location[128] = {};
    if (!url_encode_component(location, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "qweather city location too long");
        return false;
    }

    char url[384];
    snprintf(url, sizeof(url),
             "https://geoapi.qweather.com/v2/city/lookup?location=%s&number=1&lang=zh&key=%s",
             encoded_location, g_weather_api_key);
    char response[3072] = {};
    if (http_get_text(url, response, sizeof(response)) != ESP_OK) {
        ESP_LOGW(TAG, "qweather city lookup http failed");
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather city", response);
        return false;
    }
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *locations = cJSON_GetObjectItem(root, "location");
    cJSON *first = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : nullptr;
    if (cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 && first) {
        ok = json_copy_string(first, "id", city_id, city_id_len) &&
             json_copy_string(first, "name", city_name, city_name_len);
    } else {
        ESP_LOGW(TAG, "qweather city lookup failed code=%s",
                 cJSON_IsString(code) ? code->valuestring : "missing");
    }
    cJSON_Delete(root);
    return ok;
}

static bool qweather_fetch_now(const char *city_id, WeatherData *weather)
{
    char encoded_location[128] = {};
    if (!url_encode_component(city_id, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "qweather now location too long");
        return false;
    }

    char url[384];
    snprintf(url, sizeof(url),
             "https://devapi.qweather.com/v7/weather/now?location=%s&lang=zh&unit=m&key=%s",
             encoded_location, g_weather_api_key);
    char response[3072] = {};
    if (http_get_text(url, response, sizeof(response)) != ESP_OK) {
        ESP_LOGW(TAG, "qweather now http failed");
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather now", response);
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
    return ok;
}

static bool perform_weather_update()
{
    if (!g_have_weather_key) {
        xEventGroupClearBits(g_app_events, kWeatherReadyBit);
        return false;
    }

    char location[32] = {};
    char city_id[24] = {};
    char ip_city[32] = {};
    WeatherData next = {};
    if (ip_geolocation_lookup(location, sizeof(location), ip_city, sizeof(ip_city))) {
        trim_ascii(location);
        bool have_city_id = qweather_lookup_city(location, city_id, sizeof(city_id), next.city, sizeof(next.city));
        if (!have_city_id && ip_city[0] != '\0') {
            ESP_LOGW(TAG, "retry qweather city lookup by ip city: %s", ip_city);
            have_city_id = qweather_lookup_city(ip_city, city_id, sizeof(city_id), next.city, sizeof(next.city));
        }
        if (!have_city_id) {
            strlcpy(city_id, location, sizeof(city_id));
            strlcpy(next.city, ip_city[0] ? ip_city : location, sizeof(next.city));
            ESP_LOGW(TAG, "using ip coordinates for weather now: %s", city_id);
        }
        if (qweather_fetch_now(city_id, &next)) {
            g_weather = next;
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
    time_t now;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local_out) {
        *local_out = local;
    }
    return local.tm_year + 1900 >= 2024;
}

static void network_sync_task(void *)
{
    bool boot_ntp_due = true;
    time_t next_weather_at = 0;
    time_t next_ntp_retry_at = 0;
    int last_midnight_ntp_yday = -1;

    for (;;) {
        if (!g_have_wifi_creds) {
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
        bool weather_due = g_have_weather_key && (next_weather_at == 0 || now >= next_weather_at);
        bool ntp_due = (boot_ntp_due || midnight_ntp_due) && now >= next_ntp_retry_at;

        if (!ntp_due && !weather_due) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        ESP_LOGI(TAG, "wifi radio on for sync: ntp=%d weather=%d", ntp_due, weather_due);
        start_wifi_radio(false);
        if (wait_for_wifi_connected(45000)) {
            if (ntp_due) {
                if (perform_ntp_sync()) {
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
                bool weather_ok = perform_weather_update();
                time(&next_weather_at);
                next_weather_at += weather_ok ? 30 * 60 : 5 * 60;
            }
        } else {
            ESP_LOGW(TAG, "wifi connect timeout during sync window");
            if (ntp_due) {
                time(&next_ntp_retry_at);
                next_ntp_retry_at += 5 * 60;
            }
            if (weather_due) {
                time(&next_weather_at);
                next_weather_at += 5 * 60;
            }
        }
        stop_wifi_radio();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void update_time_ui(const struct tm &local)
{
    static int last_values[6] = {-1, -1, -1, -1, -1, -1};
    static int last_colon_on = -1;

    int values[6] = {
        local.tm_hour / 10,
        local.tm_hour % 10,
        local.tm_min / 10,
        local.tm_min % 10,
        local.tm_sec / 10,
        local.tm_sec % 10,
    };
    for (int i = 0; i < 6; ++i) {
        if (values[i] != last_values[i]) {
            set_digit(g_digits[i], values[i]);
            last_values[i] = values[i];
        }
    }
    bool colon_on = (local.tm_sec % 2) == 0;
    if ((int)colon_on != last_colon_on) {
        set_colon(g_colon1, colon_on);
        set_colon(g_colon2, colon_on);
        last_colon_on = colon_on;
    }

    char date[32];
    snprintf(date, sizeof(date), "%04d/%02d/%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    set_label_text_if_changed(g_date_label, date);

    static const char *week_days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    set_label_text_if_changed(g_week_label, week_days[local.tm_wday]);
}

static void ui_task(void *)
{
    TickType_t last_status_update = xTaskGetTickCount() - pdMS_TO_TICKS(1000);

    for (;;) {
        time_t now;
        time(&now);
        struct tm local = {};
        localtime_r(&now, &local);

        TickType_t tick_now = xTaskGetTickCount();
        bool status_due = tick_now - last_status_update >= pdMS_TO_TICKS(1000);

        if (Lvgl_lock(20)) {
            update_time_ui(local);

            if (status_due) {
                EventBits_t bits = xEventGroupGetBits(g_app_events);
                char temp[32];
                char humi[32];
                if (g_sensor_ok) {
                    snprintf(temp, sizeof(temp), "本地 %.1f℃", g_temperature);
                    snprintf(humi, sizeof(humi), "湿度 %.1f%%", g_humidity);
                } else {
                    snprintf(temp, sizeof(temp), "本地 --.-℃");
                    snprintf(humi, sizeof(humi), "湿度 --.-%%");
                }

                char wifi[48];
                if (bits & kWifiConnectedBit) {
                    snprintf(wifi, sizeof(wifi), g_setup_portal_active ? "STA OK  AP %s" : "STA OK", g_ap_ssid);
                } else if (g_have_wifi_creds && !g_wifi_radio_on) {
                    snprintf(wifi, sizeof(wifi), "WIFI OFF  AP OFF");
                } else if (g_have_wifi_creds) {
                    snprintf(wifi, sizeof(wifi), g_setup_portal_active ? "STA WAIT  AP %s" : "STA WAIT", g_ap_ssid);
                } else {
                    snprintf(wifi, sizeof(wifi), "SETUP AP %s", g_ap_ssid);
                }

                set_label_text_if_changed(g_temp_label, temp);
                set_label_text_if_changed(g_humi_label, humi);
                if (bits & kWeatherReadyBit) {
                    char city[48];
                    char weather[80];
                    snprintf(city, sizeof(city), "城市 %s", g_weather.city);
                    snprintf(weather, sizeof(weather), "%s %s℃ %s%%", g_weather.text, g_weather.temp, g_weather.humidity);
                    set_label_text_if_changed(g_weather_city_label, city);
                    set_label_text_if_changed(g_weather_info_label, weather);
                    set_label_text_if_changed(g_weather_icon_label, weather_icon_text(g_weather.icon));
                } else if (g_have_weather_key) {
                    set_label_text_if_changed(g_weather_city_label, "城市 --");
                    set_label_text_if_changed(g_weather_info_label, (bits & kWifiConnectedBit) ? "天气同步中" : "天气等待");
                    set_label_text_if_changed(g_weather_icon_label, weather_icon_text("999"));
                } else {
                    set_label_text_if_changed(g_weather_city_label, "城市 --");
                    set_label_text_if_changed(g_weather_info_label, "设置 API Key");
                    set_label_text_if_changed(g_weather_icon_label, weather_icon_text("999"));
                }
                update_battery_icon(g_battery_percent);
                set_label_text_if_changed(g_wifi_label, wifi);
                set_label_text_if_changed(g_sync_label, (bits & kTimeSyncedBit) ? "NTP OK" : "NTP WAIT");
                last_status_update = tick_now;
            }
            Lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint16_t *buffer = (uint16_t *)color_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            g_display.RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    g_display.RLCD_Display();
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

    g_have_wifi_creds = load_saved_config();
    Rtc_Setup(&g_i2c, 0x51);
    setenv("TZ", "CST-8", 1);
    tzset();
    restore_system_time_from_rtc();
    g_shtc3 = new Shtc3Port(g_i2c);
    init_battery_gauge();
    init_wifi();

    g_display.RLCD_Init();
    Lvgl_PortInit(kDisplayWidth, kDisplayHeight, flush_callback);
    if (Lvgl_lock(-1)) {
        build_clock_ui();
        Lvgl_unlock();
    }

    xTaskCreatePinnedToCore(network_sync_task, "network_sync", 7168, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(sensor_task, "sensor_task", 4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(battery_task, "battery_task", 3072, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(ui_task, "ui_task", 6144, nullptr, 3, nullptr, 1);
}
