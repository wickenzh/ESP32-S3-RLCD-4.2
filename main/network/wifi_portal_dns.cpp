// 实现配网强制门户的 DHCP 选项和 UDP DNS 响应任务。
#include "wifi_portal_dns.h"

#include "app_constexpr.h"
#include "app_state.h"
#include "captive_dns_packet.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <atomic>
#include <errno.h>

namespace {
std::atomic<bool> s_captive_dns_task_active{false};
std::atomic<bool> s_captive_dns_stop{false};
constexpr size_t kCaptivePortalUriSize = 64;
char s_captive_portal_uri[kCaptivePortalUriSize] = {};
constexpr uint16_t kCaptiveDnsPort = 53;
constexpr int kCaptiveDnsSocketTimeoutSec = 1;
constexpr int kCaptiveDnsStopWaitAttempts = 15;
constexpr uint32_t kCaptiveDnsStopWaitDelayMs = 100;
constexpr TickType_t kCaptiveDnsStopWaitDelay = pdMS_TO_TICKS(kCaptiveDnsStopWaitDelayMs);
constexpr uint32_t kCaptiveDnsTaskStack = 3072;
constexpr UBaseType_t kCaptiveDnsTaskPriority = 3;
constexpr BaseType_t kCaptiveDnsTaskCore = 0;
constexpr const char *kCaptiveDnsTaskName = "captive_dns";

#define CAPTIVE_DNS_SOCKET_FAILED_LOG "captive dns socket failed"
#define CAPTIVE_DNS_BIND_FAILED_LOG "captive dns bind failed"
#define CAPTIVE_DNS_TIMEOUT_SETUP_FAILED_FORMAT "captive dns timeout setup failed errno=%d"
#define CAPTIVE_DNS_STARTED_LOG "captive dns started"
#define CAPTIVE_DNS_STOPPED_LOG "captive dns stopped"
#define CAPTIVE_DHCPS_STOP_FAILED_FORMAT "dhcps stop before captive setup failed: %s"
#define CAPTIVE_DHCPS_DNS_OPTION_FAILED_FORMAT "dhcps dns option failed: %s"
#define CAPTIVE_AP_DNS_SETUP_FAILED_FORMAT "ap dns setup failed: %s"
#define CAPTIVE_DHCPS_URI_OPTION_FAILED_FORMAT "dhcps captive uri option failed: %s"
#define CAPTIVE_DHCPS_RESTART_FAILED_FORMAT "dhcps restart after captive setup failed: %s"
#define CAPTIVE_DNS_TASK_STILL_STOPPING_LOG "previous captive dns task still stopping"
#define CAPTIVE_DNS_TASK_START_FAILED_LOG "captive dns task start failed"

static_assert(kCaptiveDnsPort > 0, "captive DNS port must be positive");
static_assert(kCaptiveDnsSocketTimeoutSec > 0, "captive DNS socket timeout must be positive");
static_assert(kCaptiveDnsStopWaitAttempts > 0, "captive DNS stop wait attempts must be positive");
static_assert(kCaptiveDnsStopWaitDelayMs > 0, "captive DNS stop wait delay must be positive");
static_assert(kCaptiveDnsStopWaitDelay > 0, "captive DNS stop wait delay must be positive");
static_assert(kCaptiveDnsTaskStack > 0, "captive DNS task stack must be positive");
static_assert(kCaptiveDnsTaskPriority > tskIDLE_PRIORITY, "captive DNS task priority must exceed idle");
static_assert(kCaptiveDnsTaskCore >= 0, "captive DNS task core must be non-negative");
static_assert(kCaptivePortalUriSize > cstr_length(kSetupPortalUrl),
              "mutable captive portal URI must fit setup portal URL and NUL");
class ScopedSocketDescriptor {
public:
    explicit ScopedSocketDescriptor(int descriptor)
        : descriptor_(descriptor)
    {
    }

    ~ScopedSocketDescriptor()
    {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    ScopedSocketDescriptor(const ScopedSocketDescriptor &) = delete;
    ScopedSocketDescriptor &operator=(const ScopedSocketDescriptor &) = delete;

    int get() const
    {
        return descriptor_;
    }

    explicit operator bool() const
    {
        return descriptor_ >= 0;
    }

private:
    int descriptor_ = -1;
};

bool run_captive_dns_server()
{
    ScopedSocketDescriptor sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_IP));
    if (!sock) {
        ESP_LOGW(TAG, CAPTIVE_DNS_SOCKET_FAILED_LOG);
        return false;
    }

    timeval timeout = {};
    timeout.tv_sec = kCaptiveDnsSocketTimeoutSec;
    if (setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGW(TAG, CAPTIVE_DNS_TIMEOUT_SETUP_FAILED_FORMAT, errno);
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kCaptiveDnsPort);
    if (bind(sock.get(), (sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, CAPTIVE_DNS_BIND_FAILED_LOG);
        return false;
    }

    ESP_LOGI(TAG, CAPTIVE_DNS_STARTED_LOG);
    while (!s_captive_dns_stop.load(std::memory_order_acquire)) {
        uint8_t query[kCaptiveDnsPacketSize] = {};
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock.get(), query, sizeof(query), 0, (sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;
        }
        uint8_t response[kCaptiveDnsPacketSize] = {};
        int response_len = build_captive_dns_response(query, len, response, sizeof(response));
        if (response_len > 0) {
            sendto(sock.get(), response, response_len, 0, (sockaddr *)&from, from_len);
        }
    }

    return true;
}

void captive_dns_task(void *)
{
    bool started = run_captive_dns_server();
    s_captive_dns_task_active.store(false, std::memory_order_release);
    if (started) {
        ESP_LOGI(TAG, CAPTIVE_DNS_STOPPED_LOG);
    }
    vTaskDelete(nullptr);
}
} // namespace

void configure_captive_portal_dhcp(esp_netif_t *ap_netif)
{
    if (!ap_netif) {
        return;
    }
    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, CAPTIVE_DHCPS_STOP_FAILED_FORMAT, esp_err_to_name(err));
    }

    uint8_t offer_dns = 1;
    err = esp_netif_dhcps_option(ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns,
                                 sizeof(offer_dns));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, CAPTIVE_DHCPS_DNS_OPTION_FAILED_FORMAT, esp_err_to_name(err));
    }

    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ipaddr_addr(kSetupPortalIp);
    err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, CAPTIVE_AP_DNS_SETUP_FAILED_FORMAT, esp_err_to_name(err));
    }

    strlcpy(s_captive_portal_uri, kSetupPortalUrl, sizeof(s_captive_portal_uri));
    err = esp_netif_dhcps_option(ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_CAPTIVEPORTAL_URI,
                                 s_captive_portal_uri,
                                 strlen(s_captive_portal_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, CAPTIVE_DHCPS_URI_OPTION_FAILED_FORMAT, esp_err_to_name(err));
    }

    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, CAPTIVE_DHCPS_RESTART_FAILED_FORMAT, esp_err_to_name(err));
    }
}

bool start_captive_dns_server()
{
    if (s_captive_dns_task_active.load(std::memory_order_acquire)) {
        if (!s_captive_dns_stop.load(std::memory_order_acquire)) {
            return true;
        }
        for (int i = 0;
             i < kCaptiveDnsStopWaitAttempts &&
             s_captive_dns_task_active.load(std::memory_order_acquire);
             ++i) {
            vTaskDelay(kCaptiveDnsStopWaitDelay);
        }
        if (s_captive_dns_task_active.load(std::memory_order_acquire)) {
            ESP_LOGW(TAG, CAPTIVE_DNS_TASK_STILL_STOPPING_LOG);
            return false;
        }
    }
    s_captive_dns_stop.store(false, std::memory_order_release);
    s_captive_dns_task_active.store(true, std::memory_order_release);
    BaseType_t ok = xTaskCreatePinnedToCore(captive_dns_task,
                                            kCaptiveDnsTaskName,
                                            kCaptiveDnsTaskStack,
                                            nullptr,
                                            kCaptiveDnsTaskPriority,
                                            nullptr,
                                            kCaptiveDnsTaskCore);
    if (ok != pdPASS) {
        s_captive_dns_task_active.store(false, std::memory_order_release);
        ESP_LOGW(TAG, CAPTIVE_DNS_TASK_START_FAILED_LOG);
        return false;
    }
    return true;
}

void stop_captive_dns_server()
{
    if (!s_captive_dns_task_active.load(std::memory_order_acquire)) {
        s_captive_dns_stop.store(false, std::memory_order_release);
        return;
    }
    s_captive_dns_stop.store(true, std::memory_order_release);
}
