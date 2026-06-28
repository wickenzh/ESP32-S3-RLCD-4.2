// 执行设置页里的网络诊断流程，逐项显示联网链路状态。
#include "network_services.h"

#include "ui_views.h"

#include "lwip/netdb.h"

namespace {
constexpr size_t kNetworkDiagPublicIpResponseBufferSize = 2048;
constexpr size_t kNetworkDiagDefaultProbeBufferSize = 512;
constexpr size_t kNetworkDiagWideProbeBufferSize = 1024;
constexpr int kNetworkDiagJsonSearchMaxDepth = 8;
constexpr const char *kNetworkDiagPublicIpUrl = "https://uapis.cn/api/v1/network/myip";
constexpr const char *kNetworkDiagQweatherDnsHost = "dev.qweather.com";
constexpr const char *kNetworkDiagGithubDnsHost = "raw.githubusercontent.com";

class NetworkDiagResponseBuffer {
public:
    explicit NetworkDiagResponseBuffer(size_t buffer_len)
        : data_((char *)calloc(buffer_len, 1))
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

    explicit operator bool() const
    {
        return data_ != nullptr;
    }

private:
    char *data_;
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
    return http_get_text(url, response.get(), buffer_len, nullptr) == ESP_OK;
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
                      kNetworkDiagPublicIpResponseBufferSize,
                      nullptr) == ESP_OK) {
        NetworkDiagJsonRoot root(response.get());
        if (root) {
            ok = find_json_string_recursive(root.get(), "ip", out, out_len);
        }
        if (!ok) {
            char *start = response.get();
            while (*start == ' ' || *start == '\r' || *start == '\n' || *start == '\t') {
                ++start;
            }
            if (*start && strchr(start, '.') && strlen(start) < out_len) {
                strlcpy(out, start, out_len);
                ok = true;
            }
        }
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
    network_diag_set_line(0, "本地IP: 等待");
    network_diag_set_line(1, "公网IP: 等待");
    network_diag_set_line(2, "IP定位: 等待");
    network_diag_set_line(3, "DNS: 等待");
    network_diag_set_line(4, "天气: 等待");
    network_diag_set_line(5, "NTP: 等待");
    network_diag_set_line(6, "一言: 等待");
    network_diag_set_line(7, "公网: 等待");
    network_diag_set_line(8, "OTA源: 等待");
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

    char location[32] = {};
    char city[32] = {};
    char public_ip[48] = {};
    bool local_ip_ok = g_sta_ip[0] != '\0';
    diag_count(local_ip_ok);
    network_diag_set_line(0, "本地IP: %s", local_ip_ok ? g_sta_ip : "--");

    network_diag_set_line(1, "公网IP: 检测中");
    bool public_ip_ok = lookup_public_ip(public_ip, sizeof(public_ip));
    diag_count(public_ip_ok);
    network_diag_set_line(1, "公网IP: %s", public_ip_ok ? public_ip : "超时/失败");

    network_diag_set_line(3, "DNS: 检测中");
    bool dns_ok = dns_lookup_ok(kNetworkDiagQweatherDnsHost) && dns_lookup_ok(kNetworkDiagGithubDnsHost);
    network_diag_set_line(2, "IP定位: 检测中");
    bool ip_ok = ip_geolocation_lookup(location, sizeof(location), city, sizeof(city));
    diag_count(ip_ok);
    network_diag_set_line(2, "IP定位: %s %s",
                          ip_ok ? "OK" : "超时/失败",
                          city[0] ? city : "--");

    diag_count(dns_ok);
    network_diag_set_line(3, "DNS: %s", dns_ok ? "OK" : "超时/失败");

    bool weather_ok = false;
    if (g_have_weather_key && !g_low_battery_mode) {
        network_diag_set_line(4, "天气: 检测中");
        weather_ok = perform_weather_update();
    }
    network_diag_set_line(5, "NTP: 检测中");
    bool ntp_ok = perform_ntp_sync(5);
    diag_count(weather_ok);
    diag_count(ntp_ok);
    network_diag_set_line(4, "天气: %s", weather_ok ? "OK" : "超时/失败");
    network_diag_set_line(5, "NTP: %s", ntp_ok ? "OK" : "超时/失败");

    network_diag_set_line(6, "一言: 检测中");
    bool saying_ok = !g_low_battery_mode && perform_daily_saying_update();
    network_diag_set_line(7, "公网: 检测中");
    bool internet_ok = public_ip_ok || http_probe_ok(kNetworkDiagPublicIpUrl, kNetworkDiagWideProbeBufferSize);
    diag_count(saying_ok);
    diag_count(internet_ok);
    network_diag_set_line(6, "一言: %s", saying_ok ? "OK" : "超时/失败");
    network_diag_set_line(7, "公网: %s", internet_ok ? "OK" : "超时/失败");

    network_diag_set_line(8, "OTA源: 检测中");
    bool ota_ok = http_probe_ok(kOtaManifestUrl, kNetworkDiagWideProbeBufferSize);
    diag_count(ota_ok);
    network_diag_set_line(8, "OTA源: %s", ota_ok ? "OK" : "超时/失败");

    network_diag_finish();
}
