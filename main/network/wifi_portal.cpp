// 实现设备配网 AP、强制门户、Wi-Fi 扫描和网页保存流程。
#include "network_services.h"

#include "audio_services.h"
#include "sensor_services.h"
#include "ui_views.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace {
TaskHandle_t s_captive_dns_task_handle = nullptr;
volatile bool s_captive_dns_stop = false;
esp_netif_t *s_ap_netif = nullptr;
char s_captive_portal_uri[] = "http://192.168.4.1/";
constexpr int kDnsHeaderSize = 12;
constexpr int kCaptiveDnsAnswerSize = 16;
constexpr int kCaptiveDnsPacketSize = 512;
constexpr uint16_t kCaptiveDnsPort = 53;
constexpr uint32_t kCaptiveDnsTtlSeconds = 60;
constexpr int kCaptiveDnsSocketTimeoutSec = 1;
constexpr int kCaptiveDnsStopWaitAttempts = 15;
constexpr TickType_t kCaptiveDnsStopWaitDelay = pdMS_TO_TICKS(100);
constexpr uint32_t kCaptiveDnsTaskStack = 3072;
constexpr UBaseType_t kCaptiveDnsTaskPriority = 3;
constexpr BaseType_t kCaptiveDnsTaskCore = 0;

esp_err_t configure_softap()
{
    wifi_config_t ap_config = {};
    strlcpy((char *)ap_config.ap.ssid, g_ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, kSetupApPassword, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(g_ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

void dns_write_u16(uint8_t *buf, int offset, uint16_t value)
{
    buf[offset] = (uint8_t)(value >> 8);
    buf[offset + 1] = (uint8_t)(value & 0xff);
}

void dns_write_u32(uint8_t *buf, int offset, uint32_t value)
{
    buf[offset] = (uint8_t)(value >> 24);
    buf[offset + 1] = (uint8_t)((value >> 16) & 0xff);
    buf[offset + 2] = (uint8_t)((value >> 8) & 0xff);
    buf[offset + 3] = (uint8_t)(value & 0xff);
}

int build_captive_dns_response(const uint8_t *query, int query_len, uint8_t *response, int response_len)
{
    if (query_len < kDnsHeaderSize || response_len < query_len + kCaptiveDnsAnswerSize) {
        return 0;
    }
    uint16_t qd_count = ((uint16_t)query[4] << 8) | query[5];
    if (qd_count == 0) {
        return 0;
    }

    int pos = kDnsHeaderSize;
    while (pos < query_len) {
        uint8_t label_len = query[pos++];
        if (label_len == 0) {
            break;
        }
        if ((label_len & 0xc0) != 0 || pos + label_len > query_len) {
            return 0;
        }
        pos += label_len;
    }
    if (pos + 4 > query_len) {
        return 0;
    }
    int question_len = pos + 4;
    int answer_len = question_len + kCaptiveDnsAnswerSize;
    if (answer_len > response_len) {
        return 0;
    }

    memcpy(response, query, question_len);
    response[2] = 0x81;
    response[3] = 0x80;
    dns_write_u16(response, 4, 1);
    dns_write_u16(response, 6, 1);
    dns_write_u16(response, 8, 0);
    dns_write_u16(response, 10, 0);

    int out = question_len;
    dns_write_u16(response, out, 0xc00c);
    out += 2;
    dns_write_u16(response, out, 1);
    out += 2;
    dns_write_u16(response, out, 1);
    out += 2;
    dns_write_u32(response, out, kCaptiveDnsTtlSeconds);
    out += 4;
    dns_write_u16(response, out, 4);
    out += 2;
    response[out++] = 192;
    response[out++] = 168;
    response[out++] = 4;
    response[out++] = 1;
    return out;
}

void captive_dns_task(void *)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "captive dns socket failed");
        s_captive_dns_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    timeval timeout = {};
    timeout.tv_sec = kCaptiveDnsSocketTimeoutSec;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kCaptiveDnsPort);
    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "captive dns bind failed");
        close(sock);
        s_captive_dns_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "captive dns started");
    while (!s_captive_dns_stop) {
        uint8_t query[kCaptiveDnsPacketSize] = {};
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, query, sizeof(query), 0, (sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;
        }
        uint8_t response[kCaptiveDnsPacketSize] = {};
        int response_len = build_captive_dns_response(query, len, response, sizeof(response));
        if (response_len > 0) {
            sendto(sock, response, response_len, 0, (sockaddr *)&from, from_len);
        }
    }

    close(sock);
    s_captive_dns_task_handle = nullptr;
    ESP_LOGI(TAG, "captive dns stopped");
    vTaskDelete(nullptr);
}

void configure_captive_portal_dhcp()
{
    if (!s_ap_netif) {
        return;
    }
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps stop before captive setup failed: %s", esp_err_to_name(err));
    }

    uint8_t offer_dns = 1;
    err = esp_netif_dhcps_option(s_ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns,
                                 sizeof(offer_dns));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcps dns option failed: %s", esp_err_to_name(err));
    }

    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ipaddr_addr(kSetupPortalIp);
    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ap dns setup failed: %s", esp_err_to_name(err));
    }

    err = esp_netif_dhcps_option(s_ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_CAPTIVEPORTAL_URI,
                                 s_captive_portal_uri,
                                 strlen(s_captive_portal_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcps captive uri option failed: %s", esp_err_to_name(err));
    }

    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "dhcps restart after captive setup failed: %s", esp_err_to_name(err));
    }
}
} // namespace

void html_append(char *html, size_t html_len, const char *fmt, ...)
{
    if (!html || html_len == 0 || !fmt) {
        return;
    }
    size_t used = strnlen(html, html_len);
    if (used >= html_len - 1) {
        html[html_len - 1] = '\0';
        return;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(html + used, html_len - used, fmt, args);
    va_end(args);
    if (written < 0) {
        html[used] = '\0';
        ESP_LOGW(TAG, "setup html append failed");
    } else if (written >= (int)(html_len - used)) {
        html[html_len - 1] = '\0';
        ESP_LOGW(TAG, "setup html truncated buffer=%u", (unsigned)html_len);
    }
}

void html_escape(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
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

void append_wifi_scan_list(char *html, size_t html_len)
{
    if (!html || html_len == 0) {
        return;
    }
    static constexpr uint16_t kMaxListedApCount = 32;
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
            html_append(html, html_len, "</div></section>");
            return;
        }
        if (ap_count == 0) {
            html_append(html, html_len, "<p class='muted'>No Wi-Fi found.</p>");
            html_append(html, html_len, "</div></section>");
            return;
        }
        uint16_t max_records = ap_count;
        if (max_records > kMaxListedApCount) {
            max_records = kMaxListedApCount;
        }
        wifi_ap_record_t *records = (wifi_ap_record_t *)calloc(max_records, sizeof(wifi_ap_record_t));
        if (records == nullptr) {
            html_append(html, html_len, "<p class='muted'>Not enough memory to list Wi-Fi.</p>");
            html_append(html, html_len, "</div></section>");
            return;
        }
        err = esp_wifi_scan_get_ap_records(&max_records, records);
        if (err != ESP_OK) {
            free(records);
            html_append(html, html_len, "<p class='muted'>Scan failed, refresh this page.</p>");
            html_append(html, html_len, "</div></section>");
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
    }
    html_append(html, html_len, "</div></section>");
}

bool apply_station_config(bool reconnect)
{
    wifi_config_t sta_config = {};
    strlcpy((char *)sta_config.sta.ssid, g_wifi_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, g_wifi_pass, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = g_wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi sta config failed: %s", esp_err_to_name(err));
        return false;
    }
    if (reconnect) {
        esp_wifi_disconnect();
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "wifi connect failed to start: %s", esp_err_to_name(err));
            return false;
        }
    }
    return true;
}

void stop_http_server()
{
    if (g_http_server) {
        httpd_stop(g_http_server);
        g_http_server = nullptr;
    }
    stop_captive_dns_server();
    g_setup_portal_active = false;
}

esp_err_t root_get_handler(httpd_req_t *req)
{
    char safe_ssid[80] = {};
    char safe_weather_city[80] = {};
    html_escape(g_wifi_ssid, safe_ssid, sizeof(safe_ssid));
    html_escape(g_manual_weather_city, safe_weather_city, sizeof(safe_weather_city));
    const size_t html_len = 12288;
    char *html = (char *)calloc(1, html_len);
    if (html == nullptr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Not enough memory.");
    }
    html_append(html, html_len,
                "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
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
                "<body><main class='wrap'><div class='brand'><div><h1>WeatherClock</h1><p class='sub'>Connect Wi-Fi, or set time for offline mode.</p></div><div class='mark'>42</div></div>"
                "<div class='panel'><div class='pill'>Setup AP: %s</div><form method='get' action='/save' accept-charset='UTF-8'>"
                "<label>Wi-Fi SSID</label><input name='ssid' placeholder='Choose or type network name' value='%s' autocomplete='off'>"
                "<label>Password</label><input name='pass' placeholder='Wi-Fi password' type='password' autocomplete='current-password'>"
                "<label>QWeather API Key</label><input name='api_key' placeholder='Leave blank to keep saved key' value='' autocomplete='off'>"
                "<label>Weather City (Optional)</label><input name='weather_city' placeholder='Leave blank for auto location' value='%s' autocomplete='off'>"
                "<label>Offline Date & Time</label><input name='manual_time' type='datetime-local' placeholder='Set time without Wi-Fi'>"
                "<button class='submit' type='submit'>Save and connect</button></form></div>",
                g_ap_ssid, safe_ssid, safe_weather_city);
    append_wifi_scan_list(html, html_len);
    html_append(html, html_len, "</main></body></html>");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, html, strlen(html));
    free(html);
    return err;
}

enum ManualWeatherCityValidationResult {
    kManualWeatherCityValidationOk,
    kManualWeatherCityValidationInvalid,
    kManualWeatherCityValidationDeferred,
};

static ManualWeatherCityValidationResult validate_saved_manual_weather_city()
{
    if (!g_has_manual_weather_city || g_manual_weather_city[0] == '\0') {
        return kManualWeatherCityValidationOk;
    }
    char city_id[24] = {};
    char city_name[32] = {};
    QweatherCityLookupStatus status = qweather_lookup_city_status(g_manual_weather_city,
                                                                  city_id,
                                                                  sizeof(city_id),
                                                                  city_name,
                                                                  sizeof(city_name));
    if (status == kQweatherCityLookupOk) {
        ESP_LOGI(TAG, "manual weather city validated: %s id=%s", city_name, city_id);
        return kManualWeatherCityValidationOk;
    }
    if (status == kQweatherCityLookupNotFound) {
        ESP_LOGW(TAG, "manual weather city validation failed, restoring auto location");
        (void)clear_manual_weather_city();
        return kManualWeatherCityValidationInvalid;
    }
    ESP_LOGW(TAG, "manual weather city validation deferred after network/API error");
    return kManualWeatherCityValidationDeferred;
}

esp_err_t send_save_result_page(httpd_req_t *req, bool saved, bool connected, const char *extra_message)
{
    char safe_ssid[80] = {};
    char safe_city[80] = {};
    char safe_extra[220] = {};
    html_escape(g_wifi_ssid, safe_ssid, sizeof(safe_ssid));
    html_escape(g_has_manual_weather_city ? g_manual_weather_city : "Auto", safe_city, sizeof(safe_city));
    html_escape(extra_message ? extra_message : "", safe_extra, sizeof(safe_extra));
    char html[1700] = {};
    const char *title = saved ? (connected ? "Connected" : "Saved, still connecting") : "Missing setup data";
    const char *body = saved ? (connected ? "The clock has joined your Wi-Fi network." : "The clock saved your settings but did not get an IP yet. Check the password or router signal, then try again.")
                             : "Enter Wi-Fi and QWeather API Key, or set date and time for offline mode.";
    html_append(html, sizeof(html),
                "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>WeatherClock Setup</title><style>"
                "*{box-sizing:border-box}body{margin:0;background:#eef1f5;color:#17202a;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
                ".wrap{max-width:460px;margin:0 auto;padding:28px 16px}.panel{background:#fff;border:1px solid #d3dae2;border-radius:8px;padding:18px;box-shadow:0 8px 24px rgba(23,32,42,.08)}"
                ".state{width:48px;height:48px;border-radius:8px;border:2px solid #17202a;display:grid;place-items:center;font-size:24px;font-weight:900;margin-bottom:14px}"
                "h1{font-size:24px;margin:0 0 8px}p{font-size:15px;line-height:1.45;color:#4d5b68;margin:0 0 14px}.note{border:1px solid #d3dae2;border-radius:6px;padding:10px;margin:0 0 14px;color:#17202a;background:#fbfcfd;font-size:14px}.meta{border-top:1px solid #e1e6eb;padding-top:12px;color:#697784;font-size:13px}"
                "a{display:block;height:46px;line-height:46px;text-align:center;background:#17202a;color:#fff;text-decoration:none;border-radius:6px;font-weight:800;margin-top:16px}"
                "</style></head><body><main class='wrap'><section class='panel'><div class='state'>%s</div><h1>%s</h1><p>%s</p>"
                "%s%s%s<div class='meta'>SSID: %s<br>Weather city: %s<br>Last Wi-Fi reason: %d</div><a href='/'>Back to setup</a></section></main></body></html>",
                connected ? "OK" : "!",
                title,
                body,
                safe_extra[0] ? "<div class='note'>" : "",
                safe_extra,
                safe_extra[0] ? "</div>" : "",
                safe_ssid,
                safe_city,
                g_last_wifi_disconnect_reason);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t send_offline_result_page(httpd_req_t *req, bool saved)
{
    char html[1200] = {};
    html_append(html, sizeof(html),
                "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>WeatherClock Offline</title><style>"
                "*{box-sizing:border-box}body{margin:0;background:#eef1f5;color:#17202a;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
                ".wrap{max-width:460px;margin:0 auto;padding:28px 16px}.panel{background:#fff;border:1px solid #d3dae2;border-radius:8px;padding:18px;box-shadow:0 8px 24px rgba(23,32,42,.08)}"
                ".state{width:48px;height:48px;border-radius:8px;border:2px solid #17202a;display:grid;place-items:center;font-size:24px;font-weight:900;margin-bottom:14px}"
                "h1{font-size:24px;margin:0 0 8px}p{font-size:15px;line-height:1.45;color:#4d5b68;margin:0 0 14px}"
                "a{display:block;height:46px;line-height:46px;text-align:center;background:#17202a;color:#fff;text-decoration:none;border-radius:6px;font-weight:800;margin-top:16px}"
                "</style></head><body><main class='wrap'><section class='panel'><div class='state'>%s</div><h1>%s</h1><p>%s</p><a href='/'>Back to setup</a></section></main></body></html>",
                saved ? "OK" : "!",
                saved ? "Offline mode enabled" : "Invalid date or time",
                saved ? "The clock will use the RTC time and skip all network updates." : "Please enter a valid date and time, or configure Wi-Fi and API Key.");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_setup_save(httpd_req_t *req, const char *body)
{
    char ssid[33] = {};
    form_value(body, "ssid", ssid, sizeof(ssid));
    trim_ascii(ssid);
    if (ssid[0] == '\0') {
        bool offline_saved = save_offline_datetime_from_body(body);
        esp_err_t err = send_offline_result_page(req, offline_saved);
        if (offline_saved) {
            g_settings_requested = false;
            g_network_diag_page_requested = false;
            g_boot_info_requested = false;
            stop_wifi_radio(true);
            notify_ui_task();
        }
        return err;
    }
    bool saved = save_credentials_from_body(body);
    bool connected = saved && wait_for_wifi_connected(12000);
    const char *extra_message = nullptr;
    if (connected && g_has_manual_weather_city) {
        ManualWeatherCityValidationResult city_result = validate_saved_manual_weather_city();
        if (city_result == kManualWeatherCityValidationInvalid) {
            extra_message = "Weather city is not recognized by QWeather. Auto location has been restored.";
        } else if (city_result == kManualWeatherCityValidationDeferred) {
            extra_message = "Weather city was saved, but online validation timed out. The next weather sync will retry.";
        }
    }
    esp_err_t err = send_save_result_page(req, saved, connected, extra_message);
    if (connected) {
        xEventGroupSetBits(g_app_events, kProvisioningSyncBit);
    }
    return err;
}

esp_err_t save_post_handler(httpd_req_t *req)
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
    if (total < req->content_len) {
        ESP_LOGW(TAG,
                 "setup POST body truncated content_len=%d buffer=%u",
                 req->content_len,
                 (unsigned)sizeof(body));
    }

    return handle_setup_save(req, body);
}

esp_err_t save_get_handler(httpd_req_t *req)
{
    char query[640] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Missing query.");
    }
    return handle_setup_save(req, query);
}

esp_err_t empty_asset_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, "", 0);
}

esp_err_t captive_portal_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", kSetupPortalUrl);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "", 0);
}

bool start_captive_dns_server()
{
    if (s_captive_dns_task_handle) {
        if (!s_captive_dns_stop) {
            return true;
        }
        for (int i = 0; i < kCaptiveDnsStopWaitAttempts && s_captive_dns_task_handle; ++i) {
            vTaskDelay(kCaptiveDnsStopWaitDelay);
        }
        if (s_captive_dns_task_handle) {
            ESP_LOGW(TAG, "previous captive dns task still stopping");
            return false;
        }
    }
    s_captive_dns_stop = false;
    BaseType_t ok = xTaskCreatePinnedToCore(captive_dns_task,
                                            "captive_dns",
                                            kCaptiveDnsTaskStack,
                                            nullptr,
                                            kCaptiveDnsTaskPriority,
                                            &s_captive_dns_task_handle,
                                            kCaptiveDnsTaskCore);
    if (ok != pdPASS) {
        s_captive_dns_task_handle = nullptr;
        ESP_LOGW(TAG, "captive dns task start failed");
        return false;
    }
    return true;
}

void stop_captive_dns_server()
{
    if (!s_captive_dns_task_handle) {
        s_captive_dns_stop = false;
        return;
    }
    s_captive_dns_stop = true;
}

bool start_http_server()
{
    if (g_http_server) {
        g_setup_portal_active = true;
        if (!start_captive_dns_server()) {
            ESP_LOGW(TAG, "setup portal running without captive dns");
        }
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&g_http_server, &config);
    if (err != ESP_OK) {
        g_http_server = nullptr;
        g_setup_portal_active = false;
        ESP_LOGW(TAG, "http server start failed: %s", esp_err_to_name(err));
        return false;
    }

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = root_get_handler;
    err = httpd_register_uri_handler(g_http_server, &root);

    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = save_post_handler;
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(g_http_server, &save);
    }

    httpd_uri_t save_get = {};
    save_get.uri = "/save";
    save_get.method = HTTP_GET;
    save_get.handler = save_get_handler;
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(g_http_server, &save_get);
    }

    httpd_uri_t favicon = {};
    favicon.uri = "/favicon.ico";
    favicon.method = HTTP_GET;
    favicon.handler = empty_asset_handler;
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(g_http_server, &favicon);
    }

    httpd_uri_t apple_icon = {};
    apple_icon.uri = "/apple-touch-icon.png";
    apple_icon.method = HTTP_GET;
    apple_icon.handler = empty_asset_handler;
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(g_http_server, &apple_icon);
    }

    httpd_uri_t apple_icon_precomposed = {};
    apple_icon_precomposed.uri = "/apple-touch-icon-precomposed.png";
    apple_icon_precomposed.method = HTTP_GET;
    apple_icon_precomposed.handler = empty_asset_handler;
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(g_http_server, &apple_icon_precomposed);
    }

    httpd_uri_t captive = {};
    captive.uri = "/*";
    captive.method = HTTP_GET;
    captive.handler = captive_portal_handler;
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(g_http_server, &captive);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http uri register failed: %s", esp_err_to_name(err));
        httpd_stop(g_http_server);
        g_http_server = nullptr;
        g_setup_portal_active = false;
        return false;
    }
    if (!start_captive_dns_server()) {
        ESP_LOGW(TAG, "setup portal running without captive dns");
    }
    g_setup_portal_active = true;
    return true;
}

bool start_wifi_radio(bool enable_setup_portal)
{
    if (g_offline_mode_ui_enabled && !enable_setup_portal) {
        ESP_LOGI(TAG, "wifi start skipped in offline mode");
        return false;
    }
    bool entering_setup_portal = enable_setup_portal && !g_setup_portal_active;
    if (g_wifi_radio_on) {
        if (!enable_setup_portal) {
            stop_http_server();
            esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
            if (mode_err != ESP_OK) {
                ESP_LOGW(TAG, "wifi sta-only mode failed: %s", esp_err_to_name(mode_err));
                return false;
            }
            esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
            if (ps_err != ESP_OK) {
                ESP_LOGW(TAG, "wifi power save setup failed: %s", esp_err_to_name(ps_err));
            }
        } else {
            esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
            if (mode_err != ESP_OK) {
                ESP_LOGW(TAG, "wifi apsta mode failed: %s", esp_err_to_name(mode_err));
                return false;
            }
            esp_err_t ap_err = configure_softap();
            if (ap_err != ESP_OK) {
                ESP_LOGW(TAG, "wifi softap config failed: %s", esp_err_to_name(ap_err));
                return false;
            }
            esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
            if (ps_err != ESP_OK) {
                ESP_LOGW(TAG, "wifi setup power save disable failed: %s", esp_err_to_name(ps_err));
            }
            if (!g_have_wifi_creds) {
                (void)esp_wifi_disconnect();
                xEventGroupClearBits(g_app_events, kWifiConnectedBit);
                g_sta_ip[0] = '\0';
            }
        }
        if (enable_setup_portal && !g_setup_portal_active) {
            if (!start_http_server()) {
                return false;
            }
            ESP_LOGI(TAG, "setup AP active ssid=%s", g_ap_ssid);
        }
        if (g_have_wifi_creds) {
            (void)apply_station_config(true);
        }
        if (entering_setup_portal) {
            request_setup_prompt_once();
        }
        return true;
    }

    g_wifi_stop_requested = false;
    esp_err_t err = esp_wifi_set_mode(enable_setup_portal ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi set mode failed: %s", esp_err_to_name(err));
        return false;
    }
    if (enable_setup_portal) {
        err = configure_softap();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi softap config failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    if (g_have_wifi_creds) {
        (void)apply_station_config(false);
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return false;
    }
    esp_err_t ps_err = esp_wifi_set_ps(enable_setup_portal ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi power save setup failed: %s", esp_err_to_name(ps_err));
    }
    if (enable_setup_portal) {
        if (!start_http_server()) {
            g_wifi_stop_requested = true;
            (void)esp_wifi_disconnect();
            (void)esp_wifi_stop();
            return false;
        }
        if (entering_setup_portal) {
            request_setup_prompt_once();
        }
        ESP_LOGI(TAG, "setup AP active ssid=%s", g_ap_ssid);
    }
    g_wifi_radio_on = true;
    return true;
}

void stop_wifi_radio(bool force_setup_portal)
{
    if (!g_wifi_radio_on) {
        return;
    }
    if ((g_ota_state == kOtaChecking || g_ota_state == kOtaUpdating) && !force_setup_portal) {
        ESP_LOGI(TAG, "wifi stop skipped during OTA");
        return;
    }
    if (g_setup_portal_active && !force_setup_portal) {
        return;
    }
    if (!g_have_wifi_creds && !force_setup_portal) {
        return;
    }
    stop_http_server();
    g_wifi_stop_requested = true;
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "wifi disconnect during stop failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "wifi stop failed: %s", esp_err_to_name(err));
        g_wifi_stop_requested = false;
    } else {
        g_wifi_radio_on = false;
        g_wifi_stop_requested = false;
        xEventGroupClearBits(g_app_events, kWifiConnectedBit);
        g_sta_ip[0] = '\0';
        ESP_LOGI(TAG, "wifi radio off");
    }
}

void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data)
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
        if (!event) {
            ESP_LOGW(TAG, "got ip event missing data");
            return;
        }
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(g_sta_ip, sizeof(g_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(g_app_events, kWifiConnectedBit);
    }
}

void init_wifi()
{
    uint8_t mac[6] = {};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi mac read failed: %s", esp_err_to_name(err));
    }
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "WeatherClock-%02X%02X", mac[4], mac[5]);

    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGW(TAG, "wifi sta netif create failed");
        return;
    }
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGW(TAG, "wifi ap netif create failed");
        return;
    }
    configure_captive_portal_dhcp();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi storage setup failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi event handler register failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ip event handler register failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi initial mode setup failed: %s", esp_err_to_name(err));
        return;
    }
    err = configure_softap();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi initial softap setup failed: %s", esp_err_to_name(err));
        return;
    }

    if (!g_have_wifi_creds && !g_offline_mode_ui_enabled) {
        start_wifi_radio(true);
    }
}
