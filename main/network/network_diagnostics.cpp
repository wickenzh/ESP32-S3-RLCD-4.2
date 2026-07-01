// 执行设置页里的网络诊断流程，逐项显示联网链路状态。
#include "network_services.h"

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
constexpr int kNetworkDiagLocalIpLine = 0;
constexpr int kNetworkDiagPublicIpLine = 1;
constexpr int kNetworkDiagIpLocationLine = 2;
constexpr int kNetworkDiagDnsLine = 3;
constexpr int kNetworkDiagWeatherLine = 4;
constexpr int kNetworkDiagNtpLine = 5;
constexpr int kNetworkDiagSayingLine = 6;
constexpr int kNetworkDiagInternetLine = 7;
constexpr int kNetworkDiagOtaLine = 8;
static_assert(kNetworkDiagLocalIpLine >= 0 && kNetworkDiagLocalIpLine < kNetworkDiagLineCount);
static_assert(kNetworkDiagPublicIpLine >= 0 && kNetworkDiagPublicIpLine < kNetworkDiagLineCount);
static_assert(kNetworkDiagIpLocationLine >= 0 && kNetworkDiagIpLocationLine < kNetworkDiagLineCount);
static_assert(kNetworkDiagDnsLine >= 0 && kNetworkDiagDnsLine < kNetworkDiagLineCount);
static_assert(kNetworkDiagWeatherLine >= 0 && kNetworkDiagWeatherLine < kNetworkDiagLineCount);
static_assert(kNetworkDiagNtpLine >= 0 && kNetworkDiagNtpLine < kNetworkDiagLineCount);
static_assert(kNetworkDiagSayingLine >= 0 && kNetworkDiagSayingLine < kNetworkDiagLineCount);
static_assert(kNetworkDiagInternetLine >= 0 && kNetworkDiagInternetLine < kNetworkDiagLineCount);
static_assert(kNetworkDiagOtaLine >= 0 && kNetworkDiagOtaLine < kNetworkDiagLineCount);

class NetworkDiagResponseBuffer {
public:
    explicit NetworkDiagResponseBuffer(size_t buffer_len)
        : data_((char *)calloc(buffer_len, 1)),
          size_(buffer_len)
    {
        if (!data_) {
            ESP_LOGW(TAG, "network diag response alloc failed len=%u", (unsigned)buffer_len);
        }
    }

    ~NetworkDiagResponseBuffer()
    {
        free(data_);
    }

    NetworkDiagResponseBuffer(const NetworkDiagResponseBuffer &) = delete;
    NetworkDiagResponseBuffer &operator=(const NetworkDiagResponseBuffer &) = delete;

    char *get() const
    {
        return data_;
    }

    size_t size() const
    {
        return size_;
    }

    explicit operator bool() const
    {
        return data_ != nullptr;
    }

private:
    char *data_;
    size_t size_;
};

class NetworkDiagJsonRoot {
public:
    explicit NetworkDiagJsonRoot(char *response)
        : root_(cJSON_Parse(response))
    {
    }

    ~NetworkDiagJsonRoot()
    {
        cJSON_Delete(root_);
    }

    NetworkDiagJsonRoot(const NetworkDiagJsonRoot &) = delete;
    NetworkDiagJsonRoot &operator=(const NetworkDiagJsonRoot &) = delete;

    cJSON *get() const
    {
        return root_;
    }

    explicit operator bool() const
    {
        return root_ != nullptr;
    }

private:
    cJSON *root_;
};

void diag_count(bool ok)
{
    g_network_diag_total = g_network_diag_total + 1;
    if (ok) {
        g_network_diag_passed = g_network_diag_passed + 1;
    }
}

const char *diag_result_text(bool ok)
{
    return ok ? kNetworkDiagStatusOk : kNetworkDiagStatusFailed;
}

bool dns_lookup_ok(const char *host)
{
    if (!host || host[0] == '\0') {
        ESP_LOGW(TAG, "network diag dns invalid host");
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
        ESP_LOGW(TAG, "network diag dns lookup failed host=%s rc=%d", host, rc);
    }
    return rc == 0;
}

bool http_probe_ok(const char *url, size_t buffer_len = kNetworkDiagDefaultProbeBufferSize)
{
    if (!url || url[0] == '\0' || buffer_len == 0) {
        ESP_LOGW(TAG, "network diag http probe invalid arg");
        return false;
    }
    NetworkDiagResponseBuffer response(buffer_len);
    if (!response) {
        return false;
    }
    return http_get_text(url, response.get(), response.size(), nullptr) == ESP_OK;
}

bool find_json_string_recursive(cJSON *node, const char *name, char *out, size_t out_len, int depth = 0)
{
    if (!node || !name || !out || out_len == 0) {
        return false;
    }
    if (depth > kNetworkDiagJsonSearchMaxDepth) {
        return false;
    }
    if (cJSON_IsObject(node)) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(node, name);
        if (cJSON_IsString(item) && item->valuestring) {
            strlcpy(out, item->valuestring, out_len);
            return true;
        }
        cJSON_ArrayForEach(item, node)
        {
            if (find_json_string_recursive(item, name, out, out_len, depth + 1)) {
                return true;
            }
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *item = nullptr;
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
    if (!text || !out || out_len == 0) {
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
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    NetworkDiagResponseBuffer response(kNetworkDiagPublicIpResponseBufferSize);
    if (!response) {
        return false;
    }
    bool ok = false;
    if (http_get_text(kNetworkDiagPublicIpUrl,
                      response.get(),
                      response.size(),
                      nullptr) == ESP_OK) {
        NetworkDiagJsonRoot root(response.get());
        if (root) {
            ok = find_json_string_recursive(root.get(), kNetworkDiagPublicIpJsonKey, out, out_len);
        }
        if (!ok) {
            ok = copy_first_token_if_ip_like(response.get(), out, out_len);
        }
        if (!ok) {
            ESP_LOGW(TAG, "network diag public ip parse failed");
        }
    } else {
        ESP_LOGW(TAG, "network diag public ip http failed");
    }
    return ok;
}
} // namespace

void network_diag_reset()
{
    g_network_diag_state = kNetworkDiagIdle;
    g_network_diag_step = 0;
    g_network_diag_passed = 0;
    g_network_diag_total = 0;
    for (int i = 0; i < kNetworkDiagLineCount; ++i) {
        g_network_diag_lines[i][0] = '\0';
    }
}

void network_diag_begin()
{
    g_network_diag_state = kNetworkDiagRunning;
    g_network_diag_step = 0;
    g_network_diag_passed = 0;
    g_network_diag_total = 0;
    network_diag_set_line(kNetworkDiagLocalIpLine, "本地IP: %s", kNetworkDiagStatusWaiting);
    network_diag_set_line(kNetworkDiagPublicIpLine, "公网IP: %s", kNetworkDiagStatusWaiting);
    network_diag_set_line(kNetworkDiagIpLocationLine, "IP定位: %s", kNetworkDiagStatusWaiting);
    network_diag_set_line(kNetworkDiagDnsLine, "DNS: %s", kNetworkDiagStatusWaiting);
    network_diag_set_line(kNetworkDiagWeatherLine, "天气: %s", kNetworkDiagStatusWaiting);
    network_diag_set_line(kNetworkDiagNtpLine, "NTP: %s", kNetworkDiagStatusWaiting);
    network_diag_set_line(kNetworkDiagSayingLine, "一言: %s", kNetworkDiagStatusWaiting);
    network_diag_set_line(kNetworkDiagInternetLine, "公网: %s", kNetworkDiagStatusWaiting);
    network_diag_set_line(kNetworkDiagOtaLine, "OTA源: %s", kNetworkDiagStatusWaiting);
}

void network_diag_finish()
{
    g_network_diag_state = kNetworkDiagDone;
    g_network_diag_step = kNetworkDiagLineCount;
    notify_ui_task();
}

void network_diag_set_line(int index, const char *fmt, ...)
{
    if (index < 0 || index >= kNetworkDiagLineCount) {
        ESP_LOGW(TAG, "network diag line index invalid: %d", index);
        return;
    }
    if (!fmt) {
        g_network_diag_lines[index][0] = '\0';
        notify_ui_task();
        return;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(g_network_diag_lines[index], kNetworkDiagLineLen, fmt, args);
    va_end(args);
    if (written < 0) {
        g_network_diag_lines[index][0] = '\0';
        ESP_LOGW(TAG, "network diag line format failed index=%d", index);
    } else if (written >= kNetworkDiagLineLen) {
        g_network_diag_lines[index][kNetworkDiagLineLen - 1] = '\0';
        ESP_LOGW(TAG, "network diag line truncated index=%d len=%d", index, written);
    }
    notify_ui_task();
}

void run_network_diagnostics()
{
    network_diag_begin();

    char location[kNetworkDiagLocationTextSize] = {};
    char city[kNetworkDiagCityTextSize] = {};
    char public_ip[kNetworkDiagPublicIpTextSize] = {};
    bool local_ip_ok = g_sta_ip[0] != '\0';
    diag_count(local_ip_ok);
    network_diag_set_line(kNetworkDiagLocalIpLine, "本地IP: %s", local_ip_ok ? g_sta_ip : kNetworkDiagPlaceholder);

    network_diag_set_line(kNetworkDiagPublicIpLine, "公网IP: %s", kNetworkDiagStatusChecking);
    bool public_ip_ok = lookup_public_ip(public_ip, sizeof(public_ip));
    diag_count(public_ip_ok);
    network_diag_set_line(kNetworkDiagPublicIpLine,
                          "公网IP: %s",
                          public_ip_ok ? public_ip : kNetworkDiagStatusFailed);

    network_diag_set_line(kNetworkDiagDnsLine, "DNS: %s", kNetworkDiagStatusChecking);
    bool dns_ok = dns_lookup_ok(kNetworkDiagQweatherDnsHost) && dns_lookup_ok(kNetworkDiagGithubDnsHost);
    network_diag_set_line(kNetworkDiagIpLocationLine, "IP定位: %s", kNetworkDiagStatusChecking);
    bool ip_ok = ip_geolocation_lookup(location, sizeof(location), city, sizeof(city));
    diag_count(ip_ok);
    network_diag_set_line(kNetworkDiagIpLocationLine, "IP定位: %s %s",
                          diag_result_text(ip_ok),
                          city[0] ? city : kNetworkDiagPlaceholder);

    diag_count(dns_ok);
    network_diag_set_line(kNetworkDiagDnsLine, "DNS: %s", diag_result_text(dns_ok));

    bool weather_ok = false;
    if (g_have_weather_key && !g_low_battery_mode) {
        network_diag_set_line(kNetworkDiagWeatherLine, "天气: %s", kNetworkDiagStatusChecking);
        weather_ok = perform_weather_update();
    }
    network_diag_set_line(kNetworkDiagNtpLine, "NTP: %s", kNetworkDiagStatusChecking);
    bool ntp_ok = perform_ntp_sync(kNetworkDiagNtpMaxRetries);
    diag_count(weather_ok);
    diag_count(ntp_ok);
    network_diag_set_line(kNetworkDiagWeatherLine, "天气: %s", diag_result_text(weather_ok));
    network_diag_set_line(kNetworkDiagNtpLine, "NTP: %s", diag_result_text(ntp_ok));

    network_diag_set_line(kNetworkDiagSayingLine, "一言: %s", kNetworkDiagStatusChecking);
    bool saying_ok = !g_low_battery_mode && perform_daily_saying_update();
    network_diag_set_line(kNetworkDiagInternetLine, "公网: %s", kNetworkDiagStatusChecking);
    bool internet_ok = public_ip_ok || http_probe_ok(kNetworkDiagPublicIpUrl, kNetworkDiagWideProbeBufferSize);
    diag_count(saying_ok);
    diag_count(internet_ok);
    network_diag_set_line(kNetworkDiagSayingLine, "一言: %s", diag_result_text(saying_ok));
    network_diag_set_line(kNetworkDiagInternetLine, "公网: %s", diag_result_text(internet_ok));

    network_diag_set_line(kNetworkDiagOtaLine, "OTA源: %s", kNetworkDiagStatusChecking);
    bool ota_ok = http_probe_ok(kOtaManifestUrl, kNetworkDiagWideProbeBufferSize);
    diag_count(ota_ok);
    network_diag_set_line(kNetworkDiagOtaLine, "OTA源: %s", diag_result_text(ota_ok));

    network_diag_finish();
}
