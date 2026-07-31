// 执行设置页里的网络诊断流程，逐项显示联网链路状态。
#include "network_services.h"

#include "app_constexpr.h"
#include "app_text_format.h"
#include "network_credentials_state.h"
#include "network_diagnostics_catalog.h"
#include "network_diagnostics_state.h"
#include "network_json_root.h"
#include "scoped_heap_buffer.h"
#include "ui_views.h"

#include "lwip/netdb.h"

namespace {
constexpr size_t kNetworkDiagPublicIpResponseBufferSize = 2048;
constexpr size_t kNetworkDiagDefaultProbeBufferSize = 512;
constexpr size_t kNetworkDiagWideProbeBufferSize = 1024;
constexpr size_t kNetworkDiagLocationTextSize = 32;
constexpr size_t kNetworkDiagCityTextSize = 32;
constexpr size_t kNetworkDiagPublicIpTextSize = 48;
constexpr int kNetworkDiagJsonSearchMaxDepth = 8;
constexpr int kNetworkDiagNtpMaxRetries = 5;
constexpr const char *kNetworkDiagPublicIpUrl = "https://uapis.cn/api/v1/network/myip";
constexpr const char *kNetworkDiagPublicIpJsonKey = "ip";
constexpr const char *kNetworkDiagQweatherDnsHost = "dev.qweather.com";
constexpr const char *kNetworkDiagGithubDnsHost = "raw.githubusercontent.com";
constexpr const char *kNetworkDiagStatusWaiting = "等待";
constexpr const char *kNetworkDiagStatusChecking = "检测中";
constexpr const char *kNetworkDiagStatusFailed = "超时/失败";
constexpr const char *kNetworkDiagStatusOk = "OK";
constexpr const char *kNetworkDiagPlaceholder = "--";
constexpr const char *kNetworkDiagLocalIpFormat = "本地IP: %s";
constexpr const char *kNetworkDiagPublicIpFormat = "公网IP: %s";
constexpr const char *kNetworkDiagIpLocationFormat = "IP定位: %s";
constexpr const char *kNetworkDiagIpLocationCityFormat = "IP定位: %s %s";
constexpr const char *kNetworkDiagDnsFormat = "DNS: %s";
constexpr const char *kNetworkDiagWeatherFormat = "天气: %s";
constexpr const char *kNetworkDiagNtpFormat = "NTP: %s";
constexpr const char *kNetworkDiagSayingFormat = "一言: %s";
constexpr const char *kNetworkDiagInternetFormat = "公网: %s";
constexpr const char *kNetworkDiagOtaFormat = "OTA源: %s";
constexpr size_t kNetworkDiagIpv4TextMinSize = sizeof("255.255.255.255");
#define NETWORK_DIAG_RESPONSE_ALLOC_FAILED_FORMAT "network diag response alloc failed len=%u"
#define NETWORK_DIAG_DNS_INVALID_HOST_LOG "network diag dns invalid host"
#define NETWORK_DIAG_DNS_LOOKUP_FAILED_FORMAT "network diag dns lookup failed host=%s rc=%d"
#define NETWORK_DIAG_HTTP_PROBE_INVALID_ARG_LOG "network diag http probe invalid arg"
#define NETWORK_DIAG_PUBLIC_IP_PARSE_FAILED_LOG "network diag public ip parse failed"
#define NETWORK_DIAG_PUBLIC_IP_HTTP_FAILED_LOG "network diag public ip http failed"
#define NETWORK_DIAG_LINE_INDEX_INVALID_FORMAT "network diag line index invalid: %d"
#define NETWORK_DIAG_LINE_FORMAT_FAILED_FORMAT "network diag line format failed index=%d"
#define NETWORK_DIAG_LINE_TRUNCATED_FORMAT "network diag line truncated index=%d len=%d"
constexpr const char *const kNetworkDiagTexts[] = {
    kNetworkDiagPublicIpUrl,
    kNetworkDiagPublicIpJsonKey,
    kNetworkDiagQweatherDnsHost,
    kNetworkDiagGithubDnsHost,
    kNetworkDiagStatusWaiting,
    kNetworkDiagStatusChecking,
    kNetworkDiagStatusFailed,
    kNetworkDiagStatusOk,
    kNetworkDiagPlaceholder,
    kNetworkDiagLocalIpFormat,
    kNetworkDiagPublicIpFormat,
    kNetworkDiagIpLocationFormat,
    kNetworkDiagIpLocationCityFormat,
    kNetworkDiagDnsFormat,
    kNetworkDiagWeatherFormat,
    kNetworkDiagNtpFormat,
    kNetworkDiagSayingFormat,
    kNetworkDiagInternetFormat,
    kNetworkDiagOtaFormat,
    NETWORK_DIAG_RESPONSE_ALLOC_FAILED_FORMAT,
    NETWORK_DIAG_DNS_INVALID_HOST_LOG,
    NETWORK_DIAG_DNS_LOOKUP_FAILED_FORMAT,
    NETWORK_DIAG_HTTP_PROBE_INVALID_ARG_LOG,
    NETWORK_DIAG_PUBLIC_IP_PARSE_FAILED_LOG,
    NETWORK_DIAG_PUBLIC_IP_HTTP_FAILED_LOG,
    NETWORK_DIAG_LINE_INDEX_INVALID_FORMAT,
    NETWORK_DIAG_LINE_FORMAT_FAILED_FORMAT,
    NETWORK_DIAG_LINE_TRUNCATED_FORMAT,
};
constexpr int kNetworkDiagLineIndices[] = {
    kNetworkDiagLocalIpLine,
    kNetworkDiagPublicIpLine,
    kNetworkDiagIpLocationLine,
    kNetworkDiagDnsLine,
    kNetworkDiagWeatherLine,
    kNetworkDiagNtpLine,
    kNetworkDiagSayingLine,
    kNetworkDiagInternetLine,
    kNetworkDiagOtaLine,
};

constexpr bool http_probe_args_valid(const char *url, size_t buffer_len)
{
    return cstr_nonempty(url) && buffer_len > 0;
}

void network_diag_clear_line(int index)
{
    network_diag_line_store(index, "");
}

constexpr bool network_diag_lines_in_range()
{
    for (int index : kNetworkDiagLineIndices) {
        if (!network_diag_line_index_valid(index)) {
            return false;
        }
    }
    return true;
}

static_assert(kNetworkDiagDefaultProbeBufferSize > 0, "network diag default probe buffer must be nonzero");
static_assert(kNetworkDiagWideProbeBufferSize >= kNetworkDiagDefaultProbeBufferSize,
              "network diag wide probe buffer must cover default probe buffer");
static_assert(kNetworkDiagPublicIpResponseBufferSize >= kNetworkDiagWideProbeBufferSize,
              "public IP response buffer must cover wide probe responses");
static_assert(kNetworkDiagLocationTextSize > 1, "network diag location text buffer must fit text and NUL");
static_assert(kNetworkDiagCityTextSize > 1, "network diag city text buffer must fit text and NUL");
static_assert(kNetworkDiagPublicIpTextSize > 1, "network diag public IP text buffer must fit text and NUL");
static_assert(kNetworkDiagPublicIpTextSize >= kNetworkDiagIpv4TextMinSize,
              "network diag public IP text buffer must fit IPv4 text");
static_assert(kNetworkDiagJsonSearchMaxDepth > 0, "network diag JSON search depth must be positive");
static_assert(kNetworkDiagNtpMaxRetries > 0, "network diag NTP retry count must be positive");
static_assert(array_count(kNetworkDiagTexts) > 0,
              "network diagnostic text guard must cover fixed texts and logs");
static_assert(cstr_array_nonempty(kNetworkDiagTexts), "network diagnostic fixed texts and logs must be non-empty");
static_assert(array_count(kNetworkDiagLineIndices) == kNetworkDiagLineCount,
              "network diag line index table must cover every UI row");
static_assert(network_diag_lines_in_range(), "network diag line indices must fit line table");
static_assert(kNetworkDiagOtaLine == kNetworkDiagLineCount - 1,
              "network diag OTA line must remain the final diagnostic row");
static_assert(kNetworkDiagLocalIpLine < kNetworkDiagPublicIpLine &&
                  kNetworkDiagPublicIpLine < kNetworkDiagIpLocationLine &&
                  kNetworkDiagIpLocationLine < kNetworkDiagDnsLine &&
                  kNetworkDiagDnsLine < kNetworkDiagWeatherLine &&
                  kNetworkDiagWeatherLine < kNetworkDiagNtpLine &&
                  kNetworkDiagNtpLine < kNetworkDiagSayingLine &&
                  kNetworkDiagSayingLine < kNetworkDiagInternetLine &&
                  kNetworkDiagInternetLine < kNetworkDiagOtaLine,
              "network diag line order must match UI initialization and execution order");

struct NetworkDiagLineFormat {
    int index;
    const char *format;
};

constexpr NetworkDiagLineFormat kNetworkDiagInitialLines[] = {
    {kNetworkDiagLocalIpLine, kNetworkDiagLocalIpFormat},
    {kNetworkDiagPublicIpLine, kNetworkDiagPublicIpFormat},
    {kNetworkDiagIpLocationLine, kNetworkDiagIpLocationFormat},
    {kNetworkDiagDnsLine, kNetworkDiagDnsFormat},
    {kNetworkDiagWeatherLine, kNetworkDiagWeatherFormat},
    {kNetworkDiagNtpLine, kNetworkDiagNtpFormat},
    {kNetworkDiagSayingLine, kNetworkDiagSayingFormat},
    {kNetworkDiagInternetLine, kNetworkDiagInternetFormat},
    {kNetworkDiagOtaLine, kNetworkDiagOtaFormat},
};

static_assert(array_count(kNetworkDiagInitialLines) == kNetworkDiagLineCount,
              "network diag initial line table must cover every UI row");
static_assert(kNetworkDiagInitialLines[0].index == kNetworkDiagLocalIpLine,
              "network diag initial table must follow visible row order");
static_assert(kNetworkDiagInitialLines[array_count(kNetworkDiagInitialLines) - 1].index == kNetworkDiagOtaLine,
              "network diag initial table must end with OTA row");

const char *diag_result_text(bool ok)
{
    return ok ? kNetworkDiagStatusOk : kNetworkDiagStatusFailed;
}

bool dns_lookup_ok(const char *host)
{
    if (!host || host[0] == '\0') {
        ESP_LOGW(TAG, "%s", NETWORK_DIAG_DNS_INVALID_HOST_LOG);
        return false;
    }
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    int rc = getaddrinfo(host, nullptr, &hints, &result);
    if (result) {
        freeaddrinfo(result);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, NETWORK_DIAG_DNS_LOOKUP_FAILED_FORMAT, host, rc);
    }
    return rc == 0;
}

bool http_probe_ok(const char *url, size_t buffer_len = kNetworkDiagDefaultProbeBufferSize)
{
    if (!http_probe_args_valid(url, buffer_len)) {
        ESP_LOGW(TAG, "%s", NETWORK_DIAG_HTTP_PROBE_INVALID_ARG_LOG);
        return false;
    }
    ScopedHeapBuffer<char> response(buffer_len, HeapBufferInit::kZeroed);
    if (!response) {
        ESP_LOGW(TAG, NETWORK_DIAG_RESPONSE_ALLOC_FAILED_FORMAT, (unsigned)buffer_len);
        return false;
    }
    return http_get_text(url, response.get(), response.size(), nullptr) == ESP_OK;
}

bool copy_json_string_value(const cJSON *item, char *out, size_t out_len)
{
    if (!cJSON_IsString(item) || !item->valuestring || !app_text::output_buffer_available(out, out_len)) {
        return false;
    }
    strlcpy(out, item->valuestring, out_len);
    return true;
}

bool find_json_string_recursive(const cJSON *node,
                                const char *name,
                                char *out,
                                size_t out_len,
                                int depth = 0)
{
    if (!node || !name || !app_text::output_buffer_available(out, out_len)) {
        return false;
    }
    if (depth > kNetworkDiagJsonSearchMaxDepth) {
        return false;
    }
    if (cJSON_IsObject(node)) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(node, name);
        if (copy_json_string_value(item, out, out_len)) {
            return true;
        }
        cJSON_ArrayForEach(item, node)
        {
            if (find_json_string_recursive(item, name, out, out_len, depth + 1)) {
                return true;
            }
        }
    } else if (cJSON_IsArray(node)) {
        const cJSON *item = nullptr;
        cJSON_ArrayForEach(item, node)
        {
            if (find_json_string_recursive(item, name, out, out_len, depth + 1)) {
                return true;
            }
        }
    }
    return false;
}

bool network_diag_token_space(char ch)
{
    return ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t';
}

bool copy_first_token_if_ip_like(const char *text, char *out, size_t out_len)
{
    if (!text || !app_text::output_buffer_available(out, out_len)) {
        return false;
    }
    const char *start = text;
    while (network_diag_token_space(*start)) {
        ++start;
    }
    if (!*start || !strchr(start, '.')) {
        return false;
    }
    size_t len = 0;
    while (start[len] && !network_diag_token_space(start[len])) {
        ++len;
    }
    if (len == 0 || len >= out_len) {
        return false;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

bool lookup_public_ip(char *out, size_t out_len)
{
    if (!app_text::output_buffer_available(out, out_len)) {
        return false;
    }
    out[0] = '\0';
    ScopedHeapBuffer<char> response(kNetworkDiagPublicIpResponseBufferSize,
                                    HeapBufferInit::kZeroed);
    if (!response) {
        ESP_LOGW(TAG,
                 NETWORK_DIAG_RESPONSE_ALLOC_FAILED_FORMAT,
                 (unsigned)kNetworkDiagPublicIpResponseBufferSize);
        return false;
    }
    bool ok = false;
    if (http_get_text(kNetworkDiagPublicIpUrl,
                      response.get(),
                      response.size(),
                      nullptr) == ESP_OK) {
        NetworkJsonRoot root(response.get());
        if (root) {
            ok = find_json_string_recursive(root.get(), kNetworkDiagPublicIpJsonKey, out, out_len);
        }
        if (!ok) {
            ok = copy_first_token_if_ip_like(response.get(), out, out_len);
        }
        if (!ok) {
            ESP_LOGW(TAG, "%s", NETWORK_DIAG_PUBLIC_IP_PARSE_FAILED_LOG);
        }
    } else {
        ESP_LOGW(TAG, "%s", NETWORK_DIAG_PUBLIC_IP_HTTP_FAILED_LOG);
    }
    return ok;
}
} // namespace

void network_diag_reset()
{
    network_diag_state_clear(kNetworkDiagIdle);
}

void network_diag_begin()
{
    network_diag_state_store(kNetworkDiagRunning);
    for (const auto &line : kNetworkDiagInitialLines) {
        network_diag_set_line(line.index, line.format, kNetworkDiagStatusWaiting);
    }
}

void network_diag_finish()
{
    network_diag_state_store(kNetworkDiagDone);
    notify_ui_task();
}

void network_diag_set_line(int index, const char *fmt, ...)
{
    if (!network_diag_line_index_valid(index)) {
        ESP_LOGW(TAG, NETWORK_DIAG_LINE_INDEX_INVALID_FORMAT, index);
        return;
    }
    if (!fmt) {
        network_diag_clear_line(index);
        notify_ui_task();
        return;
    }
    char line[kNetworkDiagLineLen] = {};
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (written < 0) {
        line[0] = '\0';
        ESP_LOGW(TAG, NETWORK_DIAG_LINE_FORMAT_FAILED_FORMAT, index);
    } else if (written >= kNetworkDiagLineLen) {
        line[kNetworkDiagLineLen - 1] = '\0';
        ESP_LOGW(TAG, NETWORK_DIAG_LINE_TRUNCATED_FORMAT, index, written);
    }
    network_diag_line_store(index, line);
    notify_ui_task();
}

namespace {
void network_diag_set_result_line(int index, const char *fmt, bool ok)
{
    network_diag_set_line(index, fmt, diag_result_text(ok));
}

void network_diag_set_checking_line(int index, const char *fmt)
{
    network_diag_set_line(index, fmt, kNetworkDiagStatusChecking);
}

void network_diag_record_result_line(int index, const char *fmt, bool ok)
{
    network_diag_set_result_line(index, fmt, ok);
}

void network_diag_record_text_line(int index, const char *fmt, bool ok, const char *success_text, const char *failed_text)
{
    network_diag_set_line(index, fmt, ok ? success_text : failed_text);
}
} // namespace

void run_network_diagnostic_checks()
{
    char location[kNetworkDiagLocationTextSize] = {};
    char city[kNetworkDiagCityTextSize] = {};
    char public_ip[kNetworkDiagPublicIpTextSize] = {};
    char local_ip[kWifiStationIpTextLen] = {};
    bool local_ip_ok = wifi_station_ip_snapshot(local_ip, sizeof(local_ip));
    network_diag_record_text_line(kNetworkDiagLocalIpLine,
                                  kNetworkDiagLocalIpFormat,
                                  local_ip_ok,
                                  local_ip,
                                  kNetworkDiagPlaceholder);

    network_diag_set_checking_line(kNetworkDiagPublicIpLine, kNetworkDiagPublicIpFormat);
    bool public_ip_ok = lookup_public_ip(public_ip, sizeof(public_ip));
    network_diag_record_text_line(kNetworkDiagPublicIpLine,
                                  kNetworkDiagPublicIpFormat,
                                  public_ip_ok,
                                  public_ip,
                                  kNetworkDiagStatusFailed);

    network_diag_set_checking_line(kNetworkDiagDnsLine, kNetworkDiagDnsFormat);
    bool dns_ok = dns_lookup_ok(kNetworkDiagQweatherDnsHost) && dns_lookup_ok(kNetworkDiagGithubDnsHost);
    network_diag_set_checking_line(kNetworkDiagIpLocationLine, kNetworkDiagIpLocationFormat);
    bool ip_ok = ip_geolocation_lookup(location, sizeof(location), city, sizeof(city));
    network_diag_set_line(kNetworkDiagIpLocationLine, kNetworkDiagIpLocationCityFormat,
                          diag_result_text(ip_ok),
                          city[0] ? city : kNetworkDiagPlaceholder);

    network_diag_record_result_line(kNetworkDiagDnsLine, kNetworkDiagDnsFormat, dns_ok);

    bool weather_ok = false;
    if (network_weather_api_key_configured() && !battery_low_mode_load()) {
        network_diag_set_checking_line(kNetworkDiagWeatherLine, kNetworkDiagWeatherFormat);
        weather_ok = perform_weather_update() == WeatherUpdateResult::kSuccess;
    }
    network_diag_set_checking_line(kNetworkDiagNtpLine, kNetworkDiagNtpFormat);
    bool ntp_ok = perform_ntp_sync(kNetworkDiagNtpMaxRetries);
    network_diag_record_result_line(kNetworkDiagWeatherLine, kNetworkDiagWeatherFormat, weather_ok);
    network_diag_record_result_line(kNetworkDiagNtpLine, kNetworkDiagNtpFormat, ntp_ok);

    network_diag_set_checking_line(kNetworkDiagSayingLine, kNetworkDiagSayingFormat);
    bool saying_ok = !battery_low_mode_load() && perform_daily_saying_update();
    network_diag_set_checking_line(kNetworkDiagInternetLine, kNetworkDiagInternetFormat);
    bool internet_ok = public_ip_ok || http_probe_ok(kNetworkDiagPublicIpUrl, kNetworkDiagWideProbeBufferSize);
    network_diag_record_result_line(kNetworkDiagSayingLine, kNetworkDiagSayingFormat, saying_ok);
    network_diag_record_result_line(kNetworkDiagInternetLine, kNetworkDiagInternetFormat, internet_ok);

    network_diag_set_checking_line(kNetworkDiagOtaLine, kNetworkDiagOtaFormat);
    bool ota_ok = http_probe_ok(kOtaManifestUrl, kNetworkDiagWideProbeBufferSize);
    network_diag_record_result_line(kNetworkDiagOtaLine, kNetworkDiagOtaFormat, ota_ok);
}
