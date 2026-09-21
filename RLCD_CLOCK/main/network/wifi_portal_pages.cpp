// 生成配网页 HTML、Wi-Fi 扫描列表和保存结果页面。
#include "wifi_portal_pages.h"

#include "app_metadata.h"
#include "app_network_config.h"
#include "app_text_format.h"
#include "checked_size.h"
#include "manual_weather_city_state.h"
#include "network_credentials_state.h"
#include "weather_provider.h"
#include "scoped_heap_buffer.h"
#include "wifi_portal_html_text.h"
#include "wifi_portal_ui_assets.h"
#include "wifi_portal_state_internal.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint16_t kMaxListedApCount = 32;
constexpr size_t kPortalSubmitSsidFieldSize = 33;
constexpr size_t kPortalWeatherCityNameSize = 32;
constexpr size_t kPortalLongestHtmlEntitySize = sizeof("&quot;") - 1;
constexpr size_t kPortalEscapedSsidSize =
    (kPortalSubmitSsidFieldSize - 1) * kPortalLongestHtmlEntitySize + 1;
constexpr size_t kPortalEscapedCitySize =
    (kPortalWeatherCityNameSize - 1) * kPortalLongestHtmlEntitySize + 1;
constexpr size_t kPortalSaveExtraTextSize = 220;
constexpr size_t kPortalRootHtmlSize = 24576;
constexpr size_t kPortalSaveResultHtmlSize = 12288;
constexpr size_t kPortalOfflineResultHtmlSize = 10240;
constexpr const char *kPortalSectionCloseHtml = "</div></div></section>";
constexpr const char *kPortalHtmlContentType = "text/html; charset=utf-8";
constexpr const char *kPortalHttpStatusInternalError = "500 Internal Server Error";
constexpr const char *kPortalHttpStatusFound = "302 Found";
constexpr const char *kPortalHeaderLocation = "Location";
constexpr const char *kPortalHeaderCacheControl = "Cache-Control";
constexpr const char *kPortalHeaderConnection = "Connection";
constexpr const char *kPortalCacheNoStore = "no-store";
constexpr const char *kPortalConnectionClose = "close";
constexpr const char *kPortalErrorNotEnoughMemory = "设备内存不足，请稍后重试。";
constexpr const char *kPortalSaveConnectedTitle = "网络连接成功";
constexpr const char *kPortalSaveValidatingTitle = "正在验证网络配置";
constexpr const char *kPortalSaveMissingTitle = "配置信息不完整";
constexpr const char *kPortalSaveWifiFailedTitle = "Wi-Fi 连接失败";
constexpr const char *kPortalSaveWeatherApiFailedTitle = "天气 API 验证失败";
constexpr const char *kPortalSaveWeatherCityInvalidTitle = "天气城市无效";
constexpr const char *kPortalSaveConnectedBody = "天气时钟已连接到 Wi-Fi 网络。";
constexpr const char *kPortalSaveValidatingBody =
    "设备正在连接 Wi-Fi，并验证所选天气服务和天气城市，请稍候。";
constexpr const char *kPortalSaveMissingBody =
    "在线模式请填写 Wi-Fi；和风天气需要 API Key 和专属 API Host，Open-Meteo 无需密钥。离线可仅设置日期时间。";
constexpr const char *kPortalSaveWifiFailedBody =
    "设备未能连接主 Wi-Fi 或备用 Wi-Fi。请检查密码、信号和路由器状态后重新填写。";
constexpr const char *kPortalSaveWeatherApiFailedBody =
    "Wi-Fi 已连接，但所选天气服务验证失败。请检查网络、城市；使用和风天气时还需检查 Key 和 Host。";
constexpr const char *kPortalSaveWeatherCityInvalidBody =
    "Wi-Fi 与 API 密钥可用，但和风天气无法识别该城市。请修改城市，或留空使用自动定位。";
constexpr const char *kPortalOfflineSavedTitle = "离线模式已开启";
constexpr const char *kPortalOfflineInvalidTitle = "日期或时间无效";
constexpr const char *kPortalOfflineSavedBody = "天气时钟将使用 RTC 时间，并停止所有网络更新。";
constexpr const char *kPortalOfflineInvalidBody = "请输入有效日期和时间，或者填写 Wi-Fi、和风天气 API 密钥与账号专属 API Host。";
constexpr const char *kPortalWifiScanBusyMessage = "Wi-Fi 正在扫描，请稍后刷新页面。";
constexpr const char *kPortalWifiScanFailedMessage = "Wi-Fi 扫描失败，请刷新页面重试。";
constexpr const char *kPortalWifiScanEmptyMessage = "没有发现可用的 Wi-Fi 网络。";
constexpr const char *kPortalWifiScanNoMemoryMessage = "设备内存不足，暂时无法显示 Wi-Fi 列表。";
constexpr const char *kPortalHtmlHeadPrefix =
    "<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
#define PORTAL_HTML_APPEND_FAILED_LOG "setup html append failed"
#define PORTAL_HTML_TRUNCATED_FORMAT "setup html truncated buffer=%u"

static_assert(kMaxListedApCount > 0, "listed AP count must be positive");
static_assert(kPortalEscapedSsidSize > kPortalSubmitSsidFieldSize,
              "escaped SSID buffer must exceed submitted SSID field size");
static_assert(kPortalEscapedCitySize > kPortalWeatherCityNameSize,
              "escaped city buffer must exceed weather city name buffer");
static_assert(kPortalWeatherCityNameSize == kManualWeatherCityLen,
              "portal city escape capacity must follow the shared city contract");
static_assert(kPortalRootHtmlSize > kPortalSaveResultHtmlSize,
              "portal root HTML buffer must exceed save result buffer");
static_assert(kPortalRootHtmlSize > kPortalOfflineResultHtmlSize,
              "portal root HTML buffer must exceed offline result buffer");
static_assert(kPortalSaveExtraTextSize > 1, "portal save extra text buffer must fit text and NUL");

struct PortalPageTextWorkspace {
    char wifi_ssid[kNetworkWifiSsidLen];
    char backup_wifi_ssid[kNetworkWifiSsidLen];
    char safe_ssid[kPortalEscapedSsidSize];
    char safe_backup_ssid[kPortalEscapedSsidSize];
    char safe_weather_city[kPortalEscapedCitySize];
    char safe_extra[kPortalSaveExtraTextSize];
    char weather_city[kManualWeatherCityLen];
    char setup_ap_ssid[kWifiSetupApSsidTextLen];
};

// Synchronous URI handlers share the single HTTP server task, so page rendering
// can borrow one private workspace without adding allocation failure paths.
EXT_RAM_BSS_ATTR PortalPageTextWorkspace s_portal_page_text_workspace;
static_assert(sizeof(s_portal_page_text_workspace.safe_extra) ==
                  kPortalSaveExtraTextSize,
              "portal page text workspace must preserve extra-message capacity");

PortalPageTextWorkspace &reset_portal_page_text_workspace()
{
    memset(&s_portal_page_text_workspace,
           0,
           sizeof(s_portal_page_text_workspace));
    return s_portal_page_text_workspace;
}

void set_portal_common_response_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, kPortalHeaderCacheControl, kPortalCacheNoStore);
    httpd_resp_set_hdr(req, kPortalHeaderConnection, kPortalConnectionClose);
}

esp_err_t send_portal_html(httpd_req_t *req, const char *html)
{
    if (!req || !html) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_resp_set_type(req, kPortalHtmlContentType);
    set_portal_common_response_headers(req);
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t send_portal_empty_response(httpd_req_t *req)
{
    if (!req) {
        return ESP_ERR_INVALID_ARG;
    }
    set_portal_common_response_headers(req);
    return httpd_resp_send(req, "", 0);
}

const char *portal_save_result_title(WifiPortalSaveResult result)
{
    switch (result) {
    case WifiPortalSaveResult::kSuccess:
        return kPortalSaveConnectedTitle;
    case WifiPortalSaveResult::kValidating:
        return kPortalSaveValidatingTitle;
    case WifiPortalSaveResult::kWifiConnectionFailed:
        return kPortalSaveWifiFailedTitle;
    case WifiPortalSaveResult::kWeatherApiFailed:
        return kPortalSaveWeatherApiFailedTitle;
    case WifiPortalSaveResult::kWeatherCityInvalid:
        return kPortalSaveWeatherCityInvalidTitle;
    case WifiPortalSaveResult::kNone:
    case WifiPortalSaveResult::kInvalidInput:
    default:
        return kPortalSaveMissingTitle;
    }
}

const char *portal_save_result_body(WifiPortalSaveResult result)
{
    switch (result) {
    case WifiPortalSaveResult::kSuccess:
        return kPortalSaveConnectedBody;
    case WifiPortalSaveResult::kValidating:
        return kPortalSaveValidatingBody;
    case WifiPortalSaveResult::kWifiConnectionFailed:
        return kPortalSaveWifiFailedBody;
    case WifiPortalSaveResult::kWeatherApiFailed:
        return kPortalSaveWeatherApiFailedBody;
    case WifiPortalSaveResult::kWeatherCityInvalid:
        return kPortalSaveWeatherCityInvalidBody;
    case WifiPortalSaveResult::kNone:
    case WifiPortalSaveResult::kInvalidInput:
    default:
        return kPortalSaveMissingBody;
    }
}

bool portal_save_result_is_visible(WifiPortalSaveResult result)
{
    return result != WifiPortalSaveResult::kNone;
}

void append_wifi_scan_message(char *html, size_t html_len, const char *message)
{
    html_append(html, html_len, "<p class='muted'>%s</p>", message ? message : "");
}

void append_wifi_scan_message_and_close(char *html, size_t html_len, const char *message)
{
    append_wifi_scan_message(html, html_len, message);
    html_append(html, html_len, kPortalSectionCloseHtml);
}
} // namespace

void html_append(char *html, size_t html_len, const char *fmt, ...)
{
    if (!app_text::output_buffer_available(html, html_len) || !fmt) {
        return;
    }
    size_t used = strnlen(html, html_len);
    if (used >= html_len - 1) {
        html[html_len - 1] = '\0';
        return;
    }
    size_t remaining = html_len - used;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(html + used, remaining, fmt, args);
    va_end(args);
    if (written < 0) {
        html[used] = '\0';
        ESP_LOGW(TAG, PORTAL_HTML_APPEND_FAILED_LOG);
    } else if (written >= (int)remaining) {
        html[html_len - 1] = '\0';
        ESP_LOGW(TAG, PORTAL_HTML_TRUNCATED_FORMAT, (unsigned)html_len);
    }
}

esp_err_t send_portal_text_status(httpd_req_t *req, const char *status, const char *text)
{
    if (!req || !status || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_resp_set_status(req, status);
    set_portal_common_response_headers(req);
    return httpd_resp_sendstr(req, text);
}

esp_err_t send_portal_empty_status(httpd_req_t *req, const char *status)
{
    if (!req || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_resp_set_status(req, status);
    return send_portal_empty_response(req);
}

esp_err_t redirect_to_setup_portal(httpd_req_t *req)
{
    if (!req) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_resp_set_status(req, kPortalHttpStatusFound);
    httpd_resp_set_hdr(req, kPortalHeaderLocation, kSetupPortalUrl);
    httpd_resp_set_hdr(req, kPortalHeaderCacheControl, kPortalCacheNoStore);
    return send_portal_empty_response(req);
}

void append_wifi_scan_list(char *html, size_t html_len)
{
    if (!app_text::output_buffer_available(html, html_len)) {
        return;
    }
    html_append(html,
                html_len,
                "<section class='wifi-section portal-panel'><div class='portal-panel-body'><div class='section-title'><span>附近的 Wi-Fi（点击填入主 Wi-Fi）</span><a href='/'>重新扫描</a></div><div class='wifi-list'>");
    wifi_scan_config_t scan_config = {};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        append_wifi_scan_message(html, html_len, kPortalWifiScanBusyMessage);
    } else {
        uint16_t ap_count = 0;
        err = esp_wifi_scan_get_ap_num(&ap_count);
        if (err != ESP_OK) {
            append_wifi_scan_message_and_close(html, html_len, kPortalWifiScanFailedMessage);
            return;
        }
        if (ap_count == 0) {
            append_wifi_scan_message_and_close(html, html_len, kPortalWifiScanEmptyMessage);
            return;
        }
        uint16_t max_records = ap_count;
        if (max_records > kMaxListedApCount) {
            max_records = kMaxListedApCount;
        }
        size_t records_bytes = 0;
        if (!app_memory::checked_size_multiply(max_records,
                                               sizeof(wifi_ap_record_t),
                                               &records_bytes)) {
            append_wifi_scan_message_and_close(html, html_len, kPortalWifiScanNoMemoryMessage);
            return;
        }
        ScopedHeapBuffer<uint8_t> records_storage(records_bytes,
                                                  HeapBufferInit::kZeroed,
                                                  HeapBufferStorage::kPsramPreferred);
        if (!records_storage) {
            append_wifi_scan_message_and_close(html, html_len, kPortalWifiScanNoMemoryMessage);
            return;
        }
        wifi_ap_record_t *records =
            reinterpret_cast<wifi_ap_record_t *>(records_storage.data());
        uint16_t record_count = max_records;
        err = esp_wifi_scan_get_ap_records(&record_count, records);
        if (err != ESP_OK) {
            append_wifi_scan_message_and_close(html, html_len, kPortalWifiScanFailedMessage);
            return;
        }
        if (record_count > max_records) {
            record_count = max_records;
        }
        if (record_count == 0) {
            append_wifi_scan_message(html, html_len, kPortalWifiScanEmptyMessage);
        }
        for (uint16_t i = 0; i < record_count; ++i) {
            if (records[i].ssid[0] == '\0') {
                continue;
            }
            char ssid[kPortalEscapedSsidSize] = {};
            wifi_portal_html::escape_text(
                reinterpret_cast<const char *>(records[i].ssid),
                ssid,
                sizeof(ssid));
            html_append(html, html_len,
                        "<button type='button' class='wifi' data-ssid=\"%s\" onclick=\"pick(this.dataset.ssid)\"><span>%s</span><b>%d dBm</b></button>",
                        ssid, ssid, records[i].rssi);
        }
    }
    html_append(html, html_len, kPortalSectionCloseHtml);
}

esp_err_t root_get_handler(httpd_req_t *req)
{
    PortalPageTextWorkspace &text = reset_portal_page_text_workspace();
    (void)manual_weather_city_snapshot(text.weather_city,
                                       sizeof(text.weather_city));
    (void)network_wifi_ssid_snapshot(text.wifi_ssid,
                                     sizeof(text.wifi_ssid));
    (void)network_wifi_alternate_ssid_snapshot(
        text.backup_wifi_ssid, sizeof(text.backup_wifi_ssid));
    (void)wifi_setup_ap_ssid_snapshot(text.setup_ap_ssid,
                                      sizeof(text.setup_ap_ssid));
    wifi_portal_html::escape_text(
        text.wifi_ssid, text.safe_ssid, sizeof(text.safe_ssid));
    wifi_portal_html::escape_text(text.backup_wifi_ssid,
                                  text.safe_backup_ssid,
                                  sizeof(text.safe_backup_ssid));
    wifi_portal_html::escape_text(text.weather_city,
                                  text.safe_weather_city,
                                  sizeof(text.safe_weather_city));
    const WifiPortalSaveSnapshot save =
        wifi_portal_save_snapshot_load();
    const WifiPortalSaveResult save_result = save.result;
    const bool show_feedback = portal_save_result_is_visible(save_result);
    const char *feedback_open = save_result == WifiPortalSaveResult::kValidating
                                    ? "<div class='feedback pending' role='status'><strong>"
                                    : (save_result == WifiPortalSaveResult::kSuccess
                                           ? "<div class='feedback success' role='status'><strong>"
                                           : "<div class='feedback' role='alert'><strong>");
    ScopedHeapBuffer<char> html(kPortalRootHtmlSize,
                                HeapBufferInit::kZeroed,
                                HeapBufferStorage::kPsramRequired);
    if (!html) {
        return send_portal_text_status(req, kPortalHttpStatusInternalError, kPortalErrorNotEnoughMemory);
    }
    html_append(html.data(), html.size(),
                "%s"
                "<title>天气时钟配网</title><style>%s</style><script>%s</script></head>"
                "<body><main class='portal-shell'><header class='portal-header'><div class='brand-lockup'><div class='brand-mark'>42</div><div class='brand-copy'><h1>天气时钟</h1><p>网络、天气与离线时间设置</p></div></div>"
                "<div class='ap-meta'><span>设备热点</span><strong>%s</strong></div></header>"
                "<section class='portal-form-shell'>%s%s%s",
                kPortalHtmlHeadPrefix,
                wifi_portal_ui::kCommonCss,
                wifi_portal_ui::kCommonScript,
                text.setup_ap_ssid,
                show_feedback ? feedback_open : "",
                show_feedback ? portal_save_result_title(save_result) : "",
                show_feedback ? "</strong>" : "");
    if (show_feedback) {
        html_append(html.data(), html.size(), "%s</div>", portal_save_result_body(save_result));
    }
    html_append(html.data(), html.size(),
                wifi_portal_ui::kFormHtml,
                text.safe_ssid,
                text.safe_backup_ssid,
                text.safe_weather_city);
    if(weather_provider_load()==WeatherProvider::kOpenMeteo) {
        html_append(html.data(),html.size(),"<script>document.getElementById('open-meteo').checked=true;selectWeatherProvider();</script>");
    }
    html_append(html.data(), html.size(), "</section>");
    append_wifi_scan_list(html.data(), html.size());
    html_append(html.data(), html.size(), "</main></body></html>");
    esp_err_t err = send_portal_html(req, html.data());
    if (err == ESP_OK && save_result == WifiPortalSaveResult::kSuccess) {
        (void)wifi_portal_mark_save_feedback_seen(save);
    }
    return err;
}

esp_err_t send_save_result_page(httpd_req_t *req,
                                WifiPortalSaveResult result,
                                const char *extra_message)
{
    PortalPageTextWorkspace &text = reset_portal_page_text_workspace();
    const bool have_weather_city = manual_weather_city_snapshot(
        text.weather_city, sizeof(text.weather_city));
    (void)network_wifi_ssid_snapshot(text.wifi_ssid,
                                     sizeof(text.wifi_ssid));
    (void)network_wifi_alternate_ssid_snapshot(
        text.backup_wifi_ssid, sizeof(text.backup_wifi_ssid));
    wifi_portal_html::escape_text(text.wifi_ssid,
                                  text.safe_ssid,
                                  sizeof(text.safe_ssid));
    wifi_portal_html::escape_text(text.backup_wifi_ssid,
                                  text.safe_backup_ssid,
                                  sizeof(text.safe_backup_ssid));
    wifi_portal_html::escape_text(
        have_weather_city ? text.weather_city : "自动定位",
        text.safe_weather_city,
        sizeof(text.safe_weather_city));
    wifi_portal_html::escape_text(extra_message ? extra_message : "",
                                  text.safe_extra,
                                  sizeof(text.safe_extra));
    ScopedHeapBuffer<char> html(kPortalSaveResultHtmlSize,
                                HeapBufferInit::kZeroed,
                                HeapBufferStorage::kPsramRequired);
    if (!html) {
        return send_portal_text_status(req, kPortalHttpStatusInternalError, kPortalErrorNotEnoughMemory);
    }
    const char *title = portal_save_result_title(result);
    const char *body = portal_save_result_body(result);
    const char *poll_script = result == WifiPortalSaveResult::kValidating
                                  ? "<script>function poll(){fetch('/status',{cache:'no-store'}).then(function(r){if(r.status===200){document.getElementById('save-state').textContent='已连接';document.getElementById('save-title').textContent='网络连接成功';document.getElementById('save-body').textContent='验证通过，设备即将进入工作状态。';return;}if(r.status===409){location.replace('/');return;}setTimeout(poll,1000);}).catch(function(){setTimeout(poll,1200);});}setTimeout(poll,800);</script>"
                                  : "";
    const char *state_text = result == WifiPortalSaveResult::kSuccess
                                 ? "已连接"
                                 : (result == WifiPortalSaveResult::kValidating
                                        ? "验证中"
                                        : "失败");
    const int disconnect_reason = wifi_last_disconnect_reason();
    html_append(html.data(), html.size(),
                "%s"
                "<title>天气时钟配网结果</title><style>%s</style>%s</head><body><main class='result-shell'><section class='portal-panel result-panel'><div id='save-state' class='result-state'>%s</div><h1 id='save-title'>%s</h1><p id='save-body'>%s</p>"
                "%s%s%s<div class='meta'>主 Wi-Fi：%s<br>备用 Wi-Fi：%s<br>API Host：已保存<br>天气城市：%s<br>最近一次 Wi-Fi 断开原因：%d</div><a class='primary-link' href='/'>返回配网页</a></section></main></body></html>",
                kPortalHtmlHeadPrefix,
                wifi_portal_ui::kCommonCss,
                poll_script,
                state_text,
                title,
                body,
                text.safe_extra[0] ? "<div class='note'>" : "",
                text.safe_extra,
                text.safe_extra[0] ? "</div>" : "",
                text.safe_ssid,
                text.safe_backup_ssid[0] ? text.safe_backup_ssid : "未配置",
                text.safe_weather_city,
                disconnect_reason);
    return send_portal_html(req, html.data());
}

esp_err_t send_offline_result_page(httpd_req_t *req, bool saved)
{
    ScopedHeapBuffer<char> html(kPortalOfflineResultHtmlSize,
                                HeapBufferInit::kZeroed,
                                HeapBufferStorage::kPsramRequired);
    if (!html) {
        return send_portal_text_status(req, kPortalHttpStatusInternalError, kPortalErrorNotEnoughMemory);
    }
    html_append(html.data(), html.size(),
                "%s"
                "<title>天气时钟离线模式</title><style>%s</style></head><body><main class='result-shell'><section class='portal-panel result-panel'><div class='result-state'>%s</div><h1>%s</h1><p>%s</p><a class='primary-link' href='/'>返回配网页</a></section></main></body></html>",
                kPortalHtmlHeadPrefix,
                wifi_portal_ui::kCommonCss,
                saved ? "已开启" : "提示",
                saved ? kPortalOfflineSavedTitle : kPortalOfflineInvalidTitle,
                saved ? kPortalOfflineSavedBody : kPortalOfflineInvalidBody);
    return send_portal_html(req, html.data());
}
