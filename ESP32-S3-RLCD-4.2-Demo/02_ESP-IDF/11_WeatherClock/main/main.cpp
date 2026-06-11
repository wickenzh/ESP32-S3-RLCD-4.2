#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
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

#include "display_bsp.h"
#include "i2c_bsp.h"
#include "i2c_equipment.h"
#include "lvgl_bsp.h"

static const char *TAG = "WeatherClock";

static constexpr int kDisplayWidth = 400;
static constexpr int kDisplayHeight = 300;
static constexpr int kWifiConnectedBit = BIT0;
static constexpr int kTimeSyncedBit = BIT1;

static DisplayPort g_display(12, 11, 5, 40, 41, kDisplayWidth, kDisplayHeight);
static I2cMasterBus g_i2c(14, 13, 0);
static Shtc3Port *g_shtc3 = nullptr;
static EventGroupHandle_t g_app_events;
static httpd_handle_t g_http_server = nullptr;

static char g_wifi_ssid[33] = {};
static char g_wifi_pass[65] = {};
static char g_ap_ssid[33] = {};
static bool g_have_wifi_creds = false;
static bool g_ntp_started = false;
static float g_temperature = 0.0f;
static float g_humidity = 0.0f;
static bool g_sensor_ok = false;

struct SegDigit {
    lv_obj_t *seg[7] = {};
};

static lv_obj_t *g_date_label;
static lv_obj_t *g_week_label;
static lv_obj_t *g_temp_label;
static lv_obj_t *g_humi_label;
static lv_obj_t *g_wifi_label;
static lv_obj_t *g_sync_label;
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
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 1, LV_PART_MAIN);
    return label;
}

static void build_clock_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    g_date_label = make_label(screen, 20, 18, 190, 28, "----/--/--");
    g_week_label = make_label(screen, 270, 18, 110, 28, "---");
    g_temp_label = make_label(screen, 20, 240, 150, 28, "TEMP --.-C");
    g_humi_label = make_label(screen, 200, 240, 150, 28, "HUMI --.-%");
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
    lv_obj_t *bottom_line = make_bar(screen, 18, 220, 364, 4);
    set_obj_black(top_line, true);
    set_obj_black(bottom_line, true);
}

static bool load_wifi_credentials()
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t ssid_len = sizeof(g_wifi_ssid);
    size_t pass_len = sizeof(g_wifi_pass);
    esp_err_t ssid_err = nvs_get_str(nvs, "ssid", g_wifi_ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(nvs, "pass", g_wifi_pass, &pass_len);
    nvs_close(nvs);
    return ssid_err == ESP_OK && pass_err == ESP_OK && g_wifi_ssid[0] != '\0';
}

static void save_wifi_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("wifi", NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "pass", pass));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));
    strlcpy(g_wifi_pass, pass, sizeof(g_wifi_pass));
    g_have_wifi_creds = true;
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
    char encoded[96] = {};
    if (len >= sizeof(encoded)) {
        len = sizeof(encoded) - 1;
    }
    memcpy(encoded, start, len);
    encoded[len] = '\0';
    url_decode(out, out_len, encoded);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WeatherClock Wi-Fi</title></head><body style='font-family:sans-serif;max-width:420px;margin:32px auto'>"
        "<h2>WeatherClock Wi-Fi</h2><form method='post' action='/save'>"
        "<p><input name='ssid' placeholder='Wi-Fi SSID' style='width:100%;font-size:18px'></p>"
        "<p><input name='pass' placeholder='Password' type='password' style='width:100%;font-size:18px'></p>"
        "<p><button style='font-size:18px'>Save and connect</button></p></form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[192] = {};
    int total = 0;
    while (total < req->content_len && total < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        total += ret;
    }
    body[total] = '\0';

    char ssid[33] = {};
    char pass[65] = {};
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "pass", pass, sizeof(pass));
    if (ssid[0] != '\0') {
        save_wifi_credentials(ssid, pass);
        wifi_config_t sta_config = {};
        strlcpy((char *)sta_config.sta.ssid, g_wifi_ssid, sizeof(sta_config.sta.ssid));
        strlcpy((char *)sta_config.sta.password, g_wifi_pass, sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        if (g_wifi_pass[0] == '\0') {
            sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        esp_wifi_connect();
    }
    const char *reply = "Saved. The clock is connecting now. You can close this page.";
    return httpd_resp_send(req, reply, HTTPD_RESP_USE_STRLEN);
}

static void start_http_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
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
}

static void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && g_have_wifi_creds) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_app_events, kWifiConnectedBit | kTimeSyncedBit);
        if (g_have_wifi_creds) {
            esp_wifi_connect();
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
        wifi_config_t sta_config = {};
        strlcpy((char *)sta_config.sta.ssid, g_wifi_ssid, sizeof(sta_config.sta.ssid));
        strlcpy((char *)sta_config.sta.password, g_wifi_pass, sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = g_wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    start_http_server();
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

static void ntp_task(void *)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    restore_system_time_from_rtc();

    for (;;) {
        xEventGroupWaitBits(g_app_events, kWifiConnectedBit, pdFALSE, pdTRUE, portMAX_DELAY);
        if (!g_ntp_started) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_setservername(1, "ntp.aliyun.com");
            esp_sntp_setservername(2, "time.windows.com");
            esp_sntp_init();
            g_ntp_started = true;
        }

        for (int retry = 0; retry < 20; ++retry) {
            time_t now;
            time(&now);
            struct tm local = {};
            localtime_r(&now, &local);
            if (local.tm_year + 1900 >= 2024) {
                sync_rtc_from_system_time();
                xEventGroupSetBits(g_app_events, kTimeSyncedBit);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(60 * 60 * 1000));
    }
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

static void update_time_ui(const struct tm &local)
{
    int values[6] = {
        local.tm_hour / 10,
        local.tm_hour % 10,
        local.tm_min / 10,
        local.tm_min % 10,
        local.tm_sec / 10,
        local.tm_sec % 10,
    };
    for (int i = 0; i < 6; ++i) {
        set_digit(g_digits[i], values[i]);
    }
    bool colon_on = (local.tm_sec % 2) == 0;
    set_colon(g_colon1, colon_on);
    set_colon(g_colon2, colon_on);

    char date[32];
    snprintf(date, sizeof(date), "%04d/%02d/%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    lv_label_set_text(g_date_label, date);

    static const char *week_days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    lv_label_set_text(g_week_label, week_days[local.tm_wday]);
}

static void ui_task(void *)
{
    for (;;) {
        time_t now;
        time(&now);
        struct tm local = {};
        localtime_r(&now, &local);

        EventBits_t bits = xEventGroupGetBits(g_app_events);
        char temp[32];
        char humi[32];
        if (g_sensor_ok) {
            snprintf(temp, sizeof(temp), "TEMP %.1fC", g_temperature);
            snprintf(humi, sizeof(humi), "HUMI %.1f%%", g_humidity);
        } else {
            snprintf(temp, sizeof(temp), "TEMP --.-C");
            snprintf(humi, sizeof(humi), "HUMI --.-%%");
        }

        char wifi[48];
        if (bits & kWifiConnectedBit) {
            snprintf(wifi, sizeof(wifi), "STA OK  AP %s", g_ap_ssid);
        } else if (g_have_wifi_creds) {
            snprintf(wifi, sizeof(wifi), "STA WAIT  AP %s", g_ap_ssid);
        } else {
            snprintf(wifi, sizeof(wifi), "SETUP AP %s", g_ap_ssid);
        }

        if (Lvgl_lock(200)) {
            update_time_ui(local);
            lv_label_set_text(g_temp_label, temp);
            lv_label_set_text(g_humi_label, humi);
            lv_label_set_text(g_wifi_label, wifi);
            lv_label_set_text(g_sync_label, (bits & kTimeSyncedBit) ? "NTP OK" : "NTP WAIT");
            Lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
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

    g_have_wifi_creds = load_wifi_credentials();
    Rtc_Setup(&g_i2c, 0x51);
    g_shtc3 = new Shtc3Port(g_i2c);
    init_wifi();

    g_display.RLCD_Init();
    Lvgl_PortInit(kDisplayWidth, kDisplayHeight, flush_callback);
    if (Lvgl_lock(-1)) {
        build_clock_ui();
        Lvgl_unlock();
    }

    xTaskCreatePinnedToCore(ntp_task, "ntp_task", 4096, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(sensor_task, "sensor_task", 4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(ui_task, "ui_task", 6144, nullptr, 3, nullptr, 1);
}
