// 实现设备配网 AP、STA 连接、Wi-Fi 事件和射频启停生命周期。
#include "network_services.h"

#include "ota_runtime_state.h"

#include "app_constexpr.h"
#include "app_text_format.h"
#include "network_credentials_state.h"
#include "wifi_idle_stop_policy.h"
#include "wifi_portal_dns.h"
#include "wifi_portal_state.h"
#include "wifi_radio_state.h"

#include "audio_services.h"
#include "sensor_services.h"
#include "ui_views.h"
#include "xiaozhi_ai.h"
#include "radio_services.h"

#include <atomic>

namespace {
esp_netif_t *s_sta_netif = nullptr;
esp_netif_t *s_ap_netif = nullptr;
esp_event_handler_instance_t s_wifi_event_handler_instance = nullptr;
esp_event_handler_instance_t s_ip_event_handler_instance = nullptr;
// 射频控制任务与 Wi-Fi 事件回调并发访问，用于抑制主动断开后的自动重连。
std::atomic<bool> s_wifi_stop_requested{false};
std::atomic<bool> s_wifi_stop_when_idle_requested{false};
constexpr uint8_t kSetupApChannel = 1;
constexpr uint8_t kSetupApMaxConnections = 4;
constexpr const char *kSetupApSsidFormat = "WeatherClock-%02X%02X";
constexpr const char *kSetupApSsidFallback = "WeatherClock-0000";

enum class StationConnectAttempt {
    Start,
    Reconnect,
};

static_assert(kSetupApChannel > 0, "setup AP channel must be positive");
static_assert(kSetupApMaxConnections > 0, "setup AP max connections must be positive");
static_assert(cstr_length(kSetupApSsidFallback) < sizeof(g_ap_ssid),
              "setup AP SSID fallback must fit global buffer");
#define WIFI_START_SKIPPED_OFFLINE_LOG "wifi start skipped in offline mode"
#define WIFI_STA_ONLY_MODE_FAILED_FORMAT "wifi sta-only mode failed: %s"
#define WIFI_POWER_SAVE_SETUP_FAILED_FORMAT "wifi power save setup failed: %s"
#define WIFI_APSTA_MODE_FAILED_FORMAT "wifi apsta mode failed: %s"
#define WIFI_RUNNING_MODE_READ_FAILED_FORMAT "wifi running mode read failed: %s"
#define WIFI_SETUP_ROLLBACK_MODE_FAILED_FORMAT "wifi setup rollback mode failed: %s"
#define WIFI_SOFTAP_CONFIG_FAILED_FORMAT "wifi softap config failed: %s"
#define WIFI_SETUP_POWER_SAVE_DISABLE_FAILED_FORMAT "wifi setup power save disable failed: %s"
#define WIFI_SETUP_AP_ACTIVE_FORMAT "setup AP active ssid=%s"
#define WIFI_SET_MODE_FAILED_FORMAT "wifi set mode failed: %s"
#define WIFI_START_FAILED_FORMAT "wifi start failed: %s"
#define WIFI_STOP_SKIPPED_OTA_LOG "wifi stop skipped during OTA"
#define WIFI_STOP_SKIPPED_XIAOZHI_LOG "Wi-Fi stop skipped: Xiaozhi AI page is active"
#define WIFI_DISCONNECT_DURING_STOP_FAILED_FORMAT "wifi disconnect during stop failed: %s"
#define WIFI_STOP_FAILED_FORMAT "wifi stop failed: %s"
#define WIFI_RADIO_OFF_LOG "wifi radio off"
#define WIFI_STA_CONFIG_FAILED_FORMAT "wifi sta config failed: %s"
#define WIFI_CONNECT_START_FAILED_FORMAT "wifi connect failed to start: %s"
#define WIFI_DISCONNECTED_FORMAT "wifi disconnected, reason=%d"
#define WIFI_RECONNECT_START_FAILED_FORMAT "wifi reconnect failed to start: %s"
#define WIFI_GOT_IP_EVENT_MISSING_LOG "got ip event missing data"
#define WIFI_GOT_IP_FORMAT "got ip: " IPSTR
#define WIFI_STA_IP_FORMAT_FAILED_LOG "sta ip format failed"
#define WIFI_CONNECTION_EVENT_GROUP_UNAVAILABLE_LOG "wifi connection event unavailable: app events not initialized"
#define WIFI_MAC_READ_FAILED_FORMAT "wifi mac read failed: %s"
#define WIFI_SETUP_AP_SSID_FORMAT_FAILED_LOG "setup AP ssid format failed"
#define WIFI_STA_NETIF_CREATE_FAILED_LOG "wifi sta netif create failed"
#define WIFI_AP_NETIF_CREATE_FAILED_LOG "wifi ap netif create failed"
#define WIFI_INIT_FAILED_FORMAT "wifi init failed: %s"
#define WIFI_STORAGE_SETUP_FAILED_FORMAT "wifi storage setup failed: %s"
#define WIFI_EVENT_HANDLER_REGISTER_FAILED_FORMAT "wifi event handler register failed: %s"
#define WIFI_IP_EVENT_HANDLER_REGISTER_FAILED_FORMAT "ip event handler register failed: %s"
#define WIFI_INITIAL_MODE_SETUP_FAILED_FORMAT "wifi initial mode setup failed: %s"
#define WIFI_INITIAL_SOFTAP_SETUP_FAILED_FORMAT "wifi initial softap setup failed: %s"
#define WIFI_INIT_ROLLBACK_FAILED_FORMAT "wifi init rollback %s failed: %s"
void format_sta_ip_or_clear(const esp_ip4_addr_t *ip)
{
    if (!ip) {
        clear_wifi_station_ip();
        return;
    }
    char station_ip[kWifiStationIpTextLen] = {};
    int written = snprintf(station_ip, sizeof(station_ip), IPSTR, IP2STR(ip));
    if (app_text::format_failed(written, sizeof(station_ip))) {
        clear_wifi_station_ip();
        ESP_LOGW(TAG, WIFI_STA_IP_FORMAT_FAILED_LOG);
        return;
    }
    wifi_station_ip_store(station_ip);
}

void set_wifi_connected_event(bool connected)
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "%s", WIFI_CONNECTION_EVENT_GROUP_UNAVAILABLE_LOG);
        return;
    }
    if (connected) {
        xEventGroupSetBits(g_app_events, kWifiConnectedBit);
    } else {
        xEventGroupClearBits(g_app_events, kWifiConnectedBit);
    }
}

void clear_sta_connection_state()
{
    clear_wifi_station_ip();
    set_wifi_connected_event(false);
}

void format_setup_ap_ssid(uint8_t mac4, uint8_t mac5)
{
    int written = snprintf(g_ap_ssid, sizeof(g_ap_ssid), kSetupApSsidFormat, mac4, mac5);
    if (app_text::format_failed(written, sizeof(g_ap_ssid))) {
        strlcpy(g_ap_ssid, kSetupApSsidFallback, sizeof(g_ap_ssid));
        ESP_LOGW(TAG, WIFI_SETUP_AP_SSID_FORMAT_FAILED_LOG);
    }
}

void rollback_failed_wifi_initialization(bool wifi_initialized)
{
    if (s_ip_event_handler_instance) {
        esp_err_t err = esp_event_handler_instance_unregister(IP_EVENT,
                                                               IP_EVENT_STA_GOT_IP,
                                                               s_ip_event_handler_instance);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, WIFI_INIT_ROLLBACK_FAILED_FORMAT, "ip handler", esp_err_to_name(err));
        }
        s_ip_event_handler_instance = nullptr;
    }
    if (s_wifi_event_handler_instance) {
        esp_err_t err = esp_event_handler_instance_unregister(WIFI_EVENT,
                                                               ESP_EVENT_ANY_ID,
                                                               s_wifi_event_handler_instance);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, WIFI_INIT_ROLLBACK_FAILED_FORMAT, "wifi handler", esp_err_to_name(err));
        }
        s_wifi_event_handler_instance = nullptr;
    }
    if (wifi_initialized) {
        esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, WIFI_INIT_ROLLBACK_FAILED_FORMAT, "driver", esp_err_to_name(err));
        }
    }
    esp_netif_destroy_default_wifi(s_ap_netif);
    s_ap_netif = nullptr;
    esp_netif_destroy_default_wifi(s_sta_netif);
    s_sta_netif = nullptr;
}

esp_err_t configure_softap()
{
    wifi_config_t ap_config = {};
    strlcpy((char *)ap_config.ap.ssid, g_ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, kSetupApPassword, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(g_ap_ssid);
    ap_config.ap.channel = kSetupApChannel;
    ap_config.ap.max_connection = kSetupApMaxConnections;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

bool configure_runtime_softap()
{
    esp_err_t err = configure_softap();
    if (err == ESP_OK) {
        return true;
    }
    ESP_LOGW(TAG, WIFI_SOFTAP_CONFIG_FAILED_FORMAT, esp_err_to_name(err));
    return false;
}

bool start_station_connection(StationConnectAttempt attempt)
{
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
        return true;
    }
    if (attempt == StationConnectAttempt::Reconnect) {
        ESP_LOGW(TAG, WIFI_RECONNECT_START_FAILED_FORMAT, esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, WIFI_CONNECT_START_FAILED_FORMAT, esp_err_to_name(err));
    }
    return false;
}

bool register_wifi_event_handlers()
{
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        wifi_event_handler,
                                                        nullptr,
                                                        &s_wifi_event_handler_instance);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_EVENT_HANDLER_REGISTER_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler,
                                              nullptr,
                                              &s_ip_event_handler_instance);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_IP_EVENT_HANDLER_REGISTER_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool configure_initial_wifi_mode()
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_INITIAL_MODE_SETUP_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    err = configure_softap();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_INITIAL_SOFTAP_SETUP_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    return true;
}

} // namespace

bool apply_station_config(bool reconnect)
{
    NetworkCredentialsSnapshot credentials = {};
    network_credentials_snapshot(&credentials);
    if (!credentials.wifi_configured) {
        return false;
    }
    wifi_config_t sta_config = {};
    strlcpy((char *)sta_config.sta.ssid,
            credentials.wifi_ssid,
            sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password,
            credentials.wifi_password,
            sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = credentials.wifi_password[0]
                                             ? WIFI_AUTH_WPA2_PSK
                                             : WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_STA_CONFIG_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    if (reconnect) {
        esp_wifi_disconnect();
        if (!start_station_connection(StationConnectAttempt::Start)) {
            return false;
        }
    }
    return true;
}

static void configure_wifi_power_save(bool enable_setup_portal)
{
    esp_err_t err = esp_wifi_set_ps(enable_setup_portal ? WIFI_PS_NONE
                                                        : WIFI_PS_MAX_MODEM);
    if (err == ESP_OK) {
        return;
    }
    if (enable_setup_portal) {
        ESP_LOGW(TAG,
                 WIFI_SETUP_POWER_SAVE_DISABLE_FAILED_FORMAT,
                 esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG,
                 WIFI_POWER_SAVE_SETUP_FAILED_FORMAT,
                 esp_err_to_name(err));
    }
}

static void rollback_running_setup_transition(wifi_mode_t previous_mode,
                                              bool entering_setup_portal)
{
    if (entering_setup_portal) {
        stop_http_server();
    }
    esp_err_t err = esp_wifi_set_mode(previous_mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 WIFI_SETUP_ROLLBACK_MODE_FAILED_FORMAT,
                 esp_err_to_name(err));
    }
}

static bool configure_running_wifi_radio(bool enable_setup_portal,
                                         bool entering_setup_portal)
{
    if (!enable_setup_portal) {
        stop_http_server();
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (mode_err != ESP_OK) {
            ESP_LOGW(TAG, WIFI_STA_ONLY_MODE_FAILED_FORMAT, esp_err_to_name(mode_err));
            return false;
        }
        configure_wifi_power_save(false);
    } else {
        wifi_mode_t previous_mode = WIFI_MODE_STA;
        esp_err_t previous_mode_err = esp_wifi_get_mode(&previous_mode);
        if (previous_mode_err != ESP_OK) {
            ESP_LOGW(TAG,
                     WIFI_RUNNING_MODE_READ_FAILED_FORMAT,
                     esp_err_to_name(previous_mode_err));
            previous_mode = WIFI_MODE_STA;
        }
        if (entering_setup_portal && previous_mode == WIFI_MODE_APSTA) {
            // An inactive portal should not preserve a stale APSTA mode after
            // another failed entry attempt.
            previous_mode = WIFI_MODE_STA;
        }
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (mode_err != ESP_OK) {
            ESP_LOGW(TAG, WIFI_APSTA_MODE_FAILED_FORMAT, esp_err_to_name(mode_err));
            return false;
        }
        if (!configure_runtime_softap()) {
            rollback_running_setup_transition(previous_mode,
                                              entering_setup_portal);
            return false;
        }
        if (!network_wifi_credentials_configured()) {
            (void)esp_wifi_disconnect();
            clear_sta_connection_state();
        }
        if (!setup_portal_active_load()) {
            if (!start_http_server()) {
                rollback_running_setup_transition(previous_mode,
                                                  entering_setup_portal);
                return false;
            }
            ESP_LOGI(TAG, WIFI_SETUP_AP_ACTIVE_FORMAT, g_ap_ssid);
        }
        configure_wifi_power_save(true);
    }
    if (network_wifi_credentials_configured()) {
        (void)apply_station_config(true);
    }
    if (entering_setup_portal) {
        request_setup_prompt_once();
    }
    return true;
}

static bool start_stopped_wifi_radio(bool enable_setup_portal,
                                     bool entering_setup_portal)
{
    s_wifi_stop_requested.store(false, std::memory_order_release);
    esp_err_t err = esp_wifi_set_mode(enable_setup_portal ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_SET_MODE_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    if (enable_setup_portal) {
        if (!configure_runtime_softap()) {
            return false;
        }
    }
    if (network_wifi_credentials_configured()) {
        (void)apply_station_config(false);
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_START_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    configure_wifi_power_save(enable_setup_portal);
    if (enable_setup_portal) {
        if (!start_http_server()) {
            s_wifi_stop_requested.store(true, std::memory_order_release);
            (void)esp_wifi_disconnect();
            (void)esp_wifi_stop();
            return false;
        }
        if (entering_setup_portal) {
            request_setup_prompt_once();
        }
        ESP_LOGI(TAG, WIFI_SETUP_AP_ACTIVE_FORMAT, g_ap_ssid);
    }
    wifi_radio_on_store(true);
    notify_ui_task();
    return true;
}

bool start_wifi_radio(bool enable_setup_portal)
{
    if (g_offline_mode_ui_enabled && !enable_setup_portal) {
        ESP_LOGI(TAG, WIFI_START_SKIPPED_OFFLINE_LOG);
        return false;
    }
    bool entering_setup_portal = enable_setup_portal && !setup_portal_active_load();
    if (wifi_radio_on_load()) {
        return configure_running_wifi_radio(enable_setup_portal,
                                            entering_setup_portal);
    }
    return start_stopped_wifi_radio(enable_setup_portal,
                                    entering_setup_portal);
}

void stop_wifi_radio(bool force_setup_portal)
{
    if (!wifi_radio_on_load()) {
        return;
    }
    int ota_state = ota_runtime_state_load();
    if ((ota_state == kOtaChecking || ota_state == kOtaUpdating) && !force_setup_portal) {
        ESP_LOGI(TAG, WIFI_STOP_SKIPPED_OTA_LOG);
        return;
    }
    if (xiaozhi_ai_network_keepalive_active() && !force_setup_portal) {
        ESP_LOGI(TAG, WIFI_STOP_SKIPPED_XIAOZHI_LOG);
        return;
    }
    if (radio_network_keepalive_active() && !force_setup_portal) {
        ESP_LOGI(TAG, "wifi stop skipped: radio streaming");
        return;
    }
    if (setup_portal_active_load() && !force_setup_portal) {
        return;
    }
    if (!network_wifi_credentials_configured() && !force_setup_portal) {
        return;
    }
    stop_http_server();
    s_wifi_stop_requested.store(true, std::memory_order_release);
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, WIFI_DISCONNECT_DURING_STOP_FAILED_FORMAT, esp_err_to_name(err));
    }
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, WIFI_STOP_FAILED_FORMAT, esp_err_to_name(err));
        s_wifi_stop_requested.store(false, std::memory_order_release);
    } else {
        wifi_radio_on_store(false);
        s_wifi_stop_requested.store(false, std::memory_order_release);
        clear_sta_connection_state();
        notify_ui_task();
        ESP_LOGI(TAG, WIFI_RADIO_OFF_LOG);
    }
}

void request_wifi_radio_stop_when_idle()
{
    s_wifi_stop_when_idle_requested.store(true, std::memory_order_release);
}

void service_wifi_radio_stop_when_idle()
{
    const bool requested = s_wifi_stop_when_idle_requested.load(std::memory_order_acquire);
    if (!requested) {
        return;
    }
    if (!wifi_radio_on_load()) {
        s_wifi_stop_when_idle_requested.store(false, std::memory_order_release);
        return;
    }
    WifiIdleStopPolicyInput policy = {};
    policy.requested = requested;
    policy.radio_on = wifi_radio_on_load();
    policy.setup_portal_active = setup_portal_active_load();
    int ota_state = ota_runtime_state_load();
    policy.ota_active = ota_state == kOtaChecking || ota_state == kOtaUpdating;
    policy.xiaozhi_keepalive_active = xiaozhi_ai_network_keepalive_active();
    policy.network_lock_active = network_awake_lock_active();
    policy.radio_keepalive_active = radio_network_keepalive_active();
    if (!wifi_idle_stop_allowed(policy)) {
        return;
    }

    s_wifi_stop_when_idle_requested.store(false, std::memory_order_release);
    stop_wifi_radio();
    if (wifi_radio_on_load()) {
        // 关闭失败或运行状态在检查后发生变化时保留请求，交给下一次
        // 联网任务收尾重试，不在当前任务内循环抢占网络资源。
        s_wifi_stop_when_idle_requested.store(true, std::memory_order_release);
    }
}

void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START &&
        network_wifi_credentials_configured()) {
        (void)start_station_connection(StationConnectAttempt::Start);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        record_wifi_disconnect_reason(event ? event->reason : -1);
        clear_sta_connection_state();
        ESP_LOGW(TAG, WIFI_DISCONNECTED_FORMAT, event ? event->reason : -1);
        notify_ui_task();
        if (network_wifi_credentials_configured() && wifi_radio_on_load() &&
            !s_wifi_stop_requested.load(std::memory_order_acquire)) {
            (void)start_station_connection(StationConnectAttempt::Reconnect);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (!event) {
            ESP_LOGW(TAG, WIFI_GOT_IP_EVENT_MISSING_LOG);
            return;
        }
        ESP_LOGI(TAG, WIFI_GOT_IP_FORMAT, IP2STR(&event->ip_info.ip));

        // 设置备用DNS（114.114.114.114），解决部分国内域名路由器DNS无法解析的问题
        if (s_sta_netif) {
            esp_netif_dns_info_t backup_dns = {};
            backup_dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("114.114.114.114");
            backup_dns.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &backup_dns);
        }

        format_sta_ip_or_clear(&event->ip_info.ip);
        set_wifi_connected_event(true);
        notify_ui_task();
    }
}

void init_wifi()
{
    uint8_t mac[6] = {};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_MAC_READ_FAILED_FORMAT, esp_err_to_name(err));
    }
    format_setup_ap_ssid(mac[4], mac[5]);

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGW(TAG, WIFI_STA_NETIF_CREATE_FAILED_LOG);
        return;
    }
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGW(TAG, WIFI_AP_NETIF_CREATE_FAILED_LOG);
        rollback_failed_wifi_initialization(false);
        return;
    }
    configure_captive_portal_dhcp(s_ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_INIT_FAILED_FORMAT, esp_err_to_name(err));
        rollback_failed_wifi_initialization(false);
        return;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_STORAGE_SETUP_FAILED_FORMAT, esp_err_to_name(err));
        rollback_failed_wifi_initialization(true);
        return;
    }
    if (!register_wifi_event_handlers()) {
        rollback_failed_wifi_initialization(true);
        return;
    }
    if (!configure_initial_wifi_mode()) {
        rollback_failed_wifi_initialization(true);
        return;
    }

    if (!network_wifi_credentials_configured() && !g_offline_mode_ui_enabled) {
        start_wifi_radio(true);
    }
}
