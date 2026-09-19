// 实现设备配网 AP、STA 连接、Wi-Fi 事件和射频启停生命周期。
#include "app_event_group.h"
#include "app_metadata.h"
#include "app_network_config.h"
#include "ota_runtime_state.h"

#include "app_constexpr.h"
#include "app_text_format.h"
#include "network_config.h"
#include "network_credentials_state.h"
#include "network_credentials_state_internal.h"
#include "offline_mode_state.h"
#include "power_services.h"
#include "scoped_semaphore_lock.h"
#include "setup_portal_control.h"
#include "setup_portal_control_internal.h"
#include "wifi_driver_init_policy.h"
#include "wifi_idle_stop_policy.h"
#include "wifi_portal_dns.h"
#include "wifi_portal_http.h"
#include "wifi_portal_state_internal.h"
#include "wifi_radio_services_internal.h"
#include "wifi_radio_state_internal.h"

#include "audio_services.h"
#include "ui_task_notify.h"
#include "xiaozhi_ai.h"

#include <atomic>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static bool apply_station_config(bool reconnect);
static void wifi_event_handler(void *,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data);

namespace {
esp_netif_t *s_sta_netif = nullptr;
esp_netif_t *s_ap_netif = nullptr;
esp_event_handler_instance_t s_wifi_event_handler_instance = nullptr;
esp_event_handler_instance_t s_ip_event_handler_instance = nullptr;
// 射频控制任务与 Wi-Fi 事件回调并发访问，用于抑制主动断开后的自动重连。
std::atomic<bool> s_wifi_stop_requested{false};
std::atomic<bool> s_wifi_stop_when_idle_requested{false};
StaticTaskMutex s_wifi_lifecycle_mutex;
uint32_t s_wifi_power_generation = 0;
StaticTaskMutex s_wifi_failover_mutex;
WifiDriverInitState s_wifi_driver_init_state =
    WifiDriverInitState::kRetryable;
WifiFailoverState s_wifi_failover_state = {};
std::atomic<bool> s_wifi_reconfigure_disconnect_pending{false};
constexpr uint8_t kSetupApChannel = 1;
constexpr uint8_t kSetupApMaxConnections = 4;
constexpr const char *kSetupApSsidFormat = "WeatherClock-%02X%02X";
constexpr const char *kSetupApSsidFallback = "WeatherClock-0000";

enum class StationConnectAttempt {
    Start,
    Reconnect,
};

constexpr bool should_reconnect_running_station(bool enable_setup_portal,
                                                bool station_connected)
{
    return enable_setup_portal || !station_connected;
}

constexpr bool should_reconfigure_running_power_save(bool enable_setup_portal,
                                                     bool xiaozhi_keepalive_active)
{
    return enable_setup_portal || !xiaozhi_keepalive_active;
}

constexpr bool station_config_failure_blocks_start(bool enable_setup_portal)
{
    return !enable_setup_portal;
}

constexpr bool should_attempt_station_reconnect(bool credentials_configured,
                                                bool radio_on,
                                                bool stop_requested,
                                                bool offline_mode)
{
    return credentials_configured && radio_on && !stop_requested &&
           !offline_mode;
}

static_assert(kSetupApChannel > 0, "setup AP channel must be positive");
static_assert(kSetupApMaxConnections > 0, "setup AP max connections must be positive");
static_assert(sizeof(static_cast<wifi_sta_config_t *>(nullptr)->ssid) + 1 ==
                  kNetworkWifiSsidLen,
              "credential SSID capacity must match ESP-IDF STA storage");
static_assert(sizeof(static_cast<wifi_sta_config_t *>(nullptr)->password) + 1 ==
                  kNetworkWifiPasswordLen,
              "credential password capacity must match ESP-IDF STA storage");
static_assert(cstr_length(kSetupApSsidFallback) < kWifiSetupApSsidTextLen,
              "setup AP SSID fallback must fit portal state buffer");
static_assert(!should_reconnect_running_station(false, true),
              "connected STA must be reused outside setup mode");
static_assert(should_reconnect_running_station(false, false),
              "disconnected STA must reconnect outside setup mode");
static_assert(should_reconnect_running_station(true, true),
              "setup mode must apply submitted station credentials");
static_assert(!should_reconfigure_running_power_save(false, true),
              "Xiaozhi keepalive must retain realtime Wi-Fi power policy");
static_assert(should_reconfigure_running_power_save(false, false),
              "ordinary running Wi-Fi may restore modem power save");
static_assert(should_reconfigure_running_power_save(true, true),
              "setup mode must retain its own Wi-Fi power policy");
static_assert(station_config_failure_blocks_start(false),
              "ordinary STA start must fail when station config cannot be applied");
static_assert(!station_config_failure_blocks_start(true),
              "setup AP must remain available when station config cannot be applied");
static_assert(should_attempt_station_reconnect(true, true, false, false),
              "an active station session must reconnect after an unexpected disconnect");
static_assert(!should_attempt_station_reconnect(false, true, false, false),
              "a station without credentials must not reconnect");
static_assert(!should_attempt_station_reconnect(true, false, false, false),
              "a stopped radio must not reconnect");
static_assert(!should_attempt_station_reconnect(true, true, true, false),
              "a deliberate radio stop must suppress reconnect");
static_assert(!should_attempt_station_reconnect(true, true, false, true),
              "offline mode must suppress reconnect");

void clear_wifi_stop_when_idle_request()
{
    s_wifi_stop_when_idle_requested.store(false, std::memory_order_release);
}

void notify_wifi_stop_retry_if_pending()
{
    if (s_wifi_stop_when_idle_requested.load(std::memory_order_acquire) &&
        app_event_group_ready()) {
        app_event_group_set_bits(kNetworkStateChangedBit);
    }
}

enum class WifiRadioStopAttempt {
    kNormal,
    kForced,
    kIdleRetry,
};

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
#define WIFI_STOP_DEFERRED_OWNER_FORMAT \
    "wifi stop deferred for competing network owner: snapshot=%d depth=%d"
#define WIFI_RADIO_OFF_LOG "wifi radio off"
#define WIFI_STA_CONFIG_FAILED_FORMAT "wifi sta config failed: %s"
#define WIFI_STA_CONFIG_PORTAL_FALLBACK_LOG \
    "wifi sta config unavailable; setup portal remains active"
#define WIFI_STA_DISCONNECT_FAILED_FORMAT "wifi station disconnect failed: %s"
#define WIFI_CONNECT_START_FAILED_FORMAT "wifi connect failed to start: %s"
#define WIFI_DISCONNECTED_FORMAT "wifi disconnected, reason=%d"
#define WIFI_RECONNECT_START_FAILED_FORMAT "wifi reconnect failed to start: %s"
#define WIFI_FAILOVER_SWITCH_FORMAT \
    "Wi-Fi slot %c unavailable; trying slot %c"
#define WIFI_FAILOVER_EXHAUSTED_LOG \
    "both Wi-Fi slots unavailable; stopping reconnects for this session"
#define WIFI_FAILOVER_MUTEX_UNAVAILABLE_LOG "Wi-Fi failover mutex unavailable"
#define WIFI_PREFERRED_SLOT_PERSIST_FAILED_LOG \
    "connected backup Wi-Fi could not be promoted"
#define WIFI_GOT_IP_EVENT_MISSING_LOG "got ip event missing data"
#define WIFI_GOT_IP_FORMAT "got ip: " IPSTR
#define WIFI_STA_IP_FORMAT_FAILED_LOG "sta ip format failed"
#define WIFI_CONNECTION_EVENT_GROUP_UNAVAILABLE_LOG "wifi connection event unavailable: app events not initialized"
#define WIFI_WAIT_SKIPPED_EVENT_GROUP_UNAVAILABLE_LOG "wifi wait skipped: app events unavailable"
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
#define WIFI_INIT_RETRY_ABORTED_LOG \
    "wifi initialization retry aborted: rollback incomplete"
#define WIFI_SETUP_AP_CLIENT_COUNT_FORMAT "setup AP clients=%u"
#define WIFI_SETUP_RESULT_STA_DISCONNECT_FAILED_FORMAT \
    "setup result STA disconnect failed: %s"
#define WIFI_SETUP_RESULT_AP_ONLY_FAILED_FORMAT \
    "setup result AP-only mode failed: %s"
#define WIFI_SETUP_RESULT_AP_ONLY_READY_LOG \
    "setup portal returned to AP-only mode for result delivery"
constexpr uint32_t kSetupResultDeliveryRetryMs = 100;
constexpr unsigned kSetupResultDeliveryAttempts = 3;
constexpr uint32_t kWifiInitializationRetryMs = 100;
constexpr unsigned kWifiInitializationAttempts = 3;
constexpr TickType_t kWifiInitializationRetryDelay =
    pdMS_TO_TICKS(kWifiInitializationRetryMs);
constexpr uint32_t kWifiPowerSaveRetryMs = 10;
constexpr unsigned kWifiPowerSaveAttempts = 3;
constexpr TickType_t kWifiPowerSaveRetryDelay =
    pdMS_TO_TICKS(kWifiPowerSaveRetryMs);
constexpr uint32_t kWifiPrimaryAttemptWindowMs = 12000;
constexpr TickType_t kWifiPrimaryAttemptWindowTicks =
    pdMS_TO_TICKS(kWifiPrimaryAttemptWindowMs);
static_assert(kSetupResultDeliveryAttempts > 0,
              "setup result delivery needs at least one attempt");
static_assert(kWifiInitializationAttempts > 1,
              "Wi-Fi initialization must retain a retry opportunity");
static_assert(kWifiInitializationRetryDelay > 0,
              "Wi-Fi initialization retry delay must be positive");
static_assert(kWifiPowerSaveAttempts > 1,
              "Wi-Fi power-save setup must retain a retry opportunity");
static_assert(kWifiPowerSaveRetryDelay > 0,
              "Wi-Fi power-save retry delay must be positive");
static_assert(kWifiPrimaryAttemptWindowTicks > 0,
              "Wi-Fi failover window must be positive");
#define WIFI_INIT_RETRY_FORMAT "wifi initialization retry: attempt=%u/%u"
#define WIFI_INIT_RETRY_EXHAUSTED_LOG "wifi initialization retry exhausted"
#define WIFI_LIFECYCLE_MUTEX_UNAVAILABLE_LOG "wifi lifecycle mutex unavailable"
#define WIFI_SETUP_START_RECOVERY_QUEUE_FAILED_LOG \
    "setup portal startup retry queue unavailable"
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
    if (!app_event_group_ready()) {
        ESP_LOGW(TAG, "%s", WIFI_CONNECTION_EVENT_GROUP_UNAVAILABLE_LOG);
        return;
    }
    if (connected) {
        app_event_group_set_bits(kWifiConnectedBit);
    } else {
        app_event_group_clear_bits(kWifiConnectedBit);
    }
}

void clear_sta_connection_state()
{
    clear_wifi_station_ip();
    set_wifi_connected_event(false);
}

char wifi_slot_label(WifiCredentialSlot slot)
{
    return slot == WifiCredentialSlot::kSlotA ? 'A' : 'B';
}

bool begin_wifi_failover_session()
{
    const WifiCredentialSlot preferred = network_wifi_preferred_slot();
    if (!network_wifi_select_slot(preferred)) {
        return false;
    }
    const bool alternate_configured =
        network_wifi_alternate_slot_configured();
    ScopedSemaphoreLock failover_lock(s_wifi_failover_mutex);
    if (!failover_lock) {
        ESP_LOGW(TAG, "%s", WIFI_FAILOVER_MUTEX_UNAVAILABLE_LOG);
        return false;
    }
    s_wifi_failover_state = wifi_failover_begin(
        preferred, alternate_configured);
    s_wifi_reconfigure_disconnect_pending.store(false,
                                                 std::memory_order_release);
    return true;
}

WifiCredentialSlot wifi_failover_current_slot_snapshot()
{
    ScopedSemaphoreLock failover_lock(s_wifi_failover_mutex);
    return failover_lock ? s_wifi_failover_state.current_slot
                         : network_wifi_current_slot();
}

WifiFailoverAction record_wifi_connection_failure(bool deliberate_disconnect,
                                                  WifiCredentialSlot *slot)
{
    ScopedSemaphoreLock failover_lock(s_wifi_failover_mutex);
    if (!failover_lock) {
        return WifiFailoverAction::kExhausted;
    }
    const WifiCredentialSlot previous = s_wifi_failover_state.current_slot;
    const WifiFailoverAction action = wifi_failover_record_failure(
        &s_wifi_failover_state, deliberate_disconnect);
    if (slot) {
        *slot = s_wifi_failover_state.current_slot;
    }
    if (action == WifiFailoverAction::kSwitchAlternate) {
        ESP_LOGW(TAG,
                 WIFI_FAILOVER_SWITCH_FORMAT,
                 wifi_slot_label(previous),
                 wifi_slot_label(s_wifi_failover_state.current_slot));
    }
    return action;
}

bool force_wifi_failover_switch(WifiCredentialSlot initial_slot)
{
    WifiCredentialSlot next_slot = initial_slot;
    {
        ScopedSemaphoreLock failover_lock(s_wifi_failover_mutex);
        if (!failover_lock) {
            return false;
        }
        if (s_wifi_failover_state.current_slot != initial_slot) {
            return true;
        }
        if (wifi_failover_force_switch(&s_wifi_failover_state) !=
            WifiFailoverAction::kSwitchAlternate) {
            return false;
        }
        next_slot = s_wifi_failover_state.current_slot;
    }
    ESP_LOGW(TAG,
             WIFI_FAILOVER_SWITCH_FORMAT,
             wifi_slot_label(initial_slot),
             wifi_slot_label(next_slot));
    if (!network_wifi_select_slot(next_slot)) {
        return false;
    }
    return apply_station_config(true);
}

void record_wifi_connection_success()
{
    ScopedSemaphoreLock failover_lock(s_wifi_failover_mutex);
    if (failover_lock) {
        wifi_failover_record_connected(&s_wifi_failover_state);
    }
}

void persist_connected_wifi_slot_if_needed()
{
    if (!app_event_group_ready() ||
        (app_event_group_get_bits() & kWifiConnectedBit) == 0) {
        return;
    }
    const WifiCredentialSlot current = network_wifi_current_slot();
    if (current != network_wifi_preferred_slot() &&
        !persist_preferred_wifi_slot(current)) {
        ESP_LOGW(TAG, "%s", WIFI_PREFERRED_SLOT_PERSIST_FAILED_LOG);
    }
}

void format_setup_ap_ssid(uint8_t mac4, uint8_t mac5)
{
    char setup_ap_ssid[kWifiSetupApSsidTextLen] = {};
    int written = snprintf(setup_ap_ssid,
                           sizeof(setup_ap_ssid),
                           kSetupApSsidFormat,
                           mac4,
                           mac5);
    if (app_text::format_failed(written, sizeof(setup_ap_ssid))) {
        strlcpy(setup_ap_ssid, kSetupApSsidFallback, sizeof(setup_ap_ssid));
        ESP_LOGW(TAG, WIFI_SETUP_AP_SSID_FORMAT_FAILED_LOG);
    }
    wifi_setup_ap_ssid_store(setup_ap_ssid);
}

void log_setup_ap_active()
{
    char setup_ap_ssid[kWifiSetupApSsidTextLen] = {};
    (void)wifi_setup_ap_ssid_snapshot(setup_ap_ssid, sizeof(setup_ap_ssid));
    ESP_LOGI(TAG, WIFI_SETUP_AP_ACTIVE_FORMAT, setup_ap_ssid);
}

bool rollback_failed_wifi_initialization(bool wifi_initialized)
{
    bool cleanup_ok = true;
    if (s_ip_event_handler_instance) {
        esp_err_t err = esp_event_handler_instance_unregister(IP_EVENT,
                                                               IP_EVENT_STA_GOT_IP,
                                                               s_ip_event_handler_instance);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, WIFI_INIT_ROLLBACK_FAILED_FORMAT, "ip handler", esp_err_to_name(err));
            cleanup_ok = false;
        } else {
            s_ip_event_handler_instance = nullptr;
        }
    }
    if (s_wifi_event_handler_instance) {
        esp_err_t err = esp_event_handler_instance_unregister(WIFI_EVENT,
                                                               ESP_EVENT_ANY_ID,
                                                               s_wifi_event_handler_instance);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, WIFI_INIT_ROLLBACK_FAILED_FORMAT, "wifi handler", esp_err_to_name(err));
            cleanup_ok = false;
        } else {
            s_wifi_event_handler_instance = nullptr;
        }
    }
    bool driver_released = !wifi_initialized;
    if (wifi_initialized) {
        esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, WIFI_INIT_ROLLBACK_FAILED_FORMAT, "driver", esp_err_to_name(err));
            cleanup_ok = false;
            driver_released = false;
        } else {
            driver_released = true;
        }
    }
    if (driver_released) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = nullptr;
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = nullptr;
    }
    return cleanup_ok;
}

esp_err_t configure_softap()
{
    char setup_ap_ssid[kWifiSetupApSsidTextLen] = {};
    (void)wifi_setup_ap_ssid_snapshot(setup_ap_ssid, sizeof(setup_ap_ssid));
    wifi_config_t ap_config = {};
    strlcpy((char *)ap_config.ap.ssid, setup_ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, kSetupApPassword, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(setup_ap_ssid);
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

bool disconnect_station_and_clear_state()
{
    s_wifi_reconfigure_disconnect_pending.store(true,
                                                 std::memory_order_release);
    const esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        s_wifi_reconfigure_disconnect_pending.store(false,
                                                     std::memory_order_release);
        ESP_LOGW(TAG, WIFI_STA_DISCONNECT_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        s_wifi_reconfigure_disconnect_pending.store(false,
                                                     std::memory_order_release);
    }
    // The disconnect event is asynchronous. Clear the previous IP state now so
    // a connection wait cannot mistake the old AP for the newly saved one.
    clear_sta_connection_state();
    return true;
}

void reconnect_station_after_disconnect_if_allowed()
{
    const bool stop_requested =
        s_wifi_stop_requested.load(std::memory_order_acquire);
    const bool reconfigure_disconnect =
        s_wifi_reconfigure_disconnect_pending.exchange(
            false, std::memory_order_acq_rel);
    WifiCredentialSlot selected_slot = network_wifi_current_slot();
    const WifiFailoverAction action = record_wifi_connection_failure(
        stop_requested || reconfigure_disconnect, &selected_slot);
    if (action == WifiFailoverAction::kIgnore) {
        return;
    }
    if (action == WifiFailoverAction::kExhausted) {
        ESP_LOGW(TAG, "%s", WIFI_FAILOVER_EXHAUSTED_LOG);
        if (!setup_portal_active_load()) {
            request_wifi_radio_stop_when_idle();
        }
        return;
    }
    if (action == WifiFailoverAction::kSwitchAlternate) {
        if (!network_wifi_select_slot(selected_slot) ||
            !apply_station_config(false)) {
            ESP_LOGW(TAG, "%s", WIFI_FAILOVER_EXHAUSTED_LOG);
            if (!setup_portal_active_load()) {
                request_wifi_radio_stop_when_idle();
            }
            return;
        }
    }
    const bool offline_mode = offline_mode_enabled_load();
    if (!should_attempt_station_reconnect(
            network_wifi_credentials_configured(),
            wifi_radio_on_load(),
            stop_requested,
            offline_mode)) {
        return;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK ||
        (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA)) {
        return;
    }

    // A deliberate stop may begin while the mode query is in flight. Recheck
    // immediately before connecting so the stop path retains radio ownership.
    if (s_wifi_stop_requested.load(std::memory_order_acquire) ||
        offline_mode_enabled_load()) {
        return;
    }
    (void)start_station_connection(StationConnectAttempt::Reconnect);
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

bool initialize_wifi_driver_once(bool *retry_safe)
{
    if (!retry_safe) {
        return false;
    }
    *retry_safe = true;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGW(TAG, WIFI_STA_NETIF_CREATE_FAILED_LOG);
        return false;
    }
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGW(TAG, WIFI_AP_NETIF_CREATE_FAILED_LOG);
        *retry_safe = rollback_failed_wifi_initialization(false);
        return false;
    }
    configure_captive_portal_dhcp(s_ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_INIT_FAILED_FORMAT, esp_err_to_name(err));
        *retry_safe = rollback_failed_wifi_initialization(false);
        return false;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_STORAGE_SETUP_FAILED_FORMAT, esp_err_to_name(err));
        *retry_safe = rollback_failed_wifi_initialization(true);
        return false;
    }
    if (!register_wifi_event_handlers()) {
        *retry_safe = rollback_failed_wifi_initialization(true);
        return false;
    }
    if (!configure_initial_wifi_mode()) {
        *retry_safe = rollback_failed_wifi_initialization(true);
        return false;
    }
    return true;
}

bool initialize_wifi_driver_with_retry()
{
    if (wifi_driver_init_ready(s_wifi_driver_init_state)) {
        return true;
    }
    if (!wifi_driver_init_retry_allowed(s_wifi_driver_init_state)) {
        return false;
    }

    for (unsigned attempt = 1;
         attempt <= kWifiInitializationAttempts;
         ++attempt) {
        bool retry_safe = true;
        const bool initialized =
            initialize_wifi_driver_once(&retry_safe);
        s_wifi_driver_init_state =
            wifi_driver_init_state_after_attempt(initialized, retry_safe);
        if (initialized) {
            return true;
        }
        if (!retry_safe) {
            ESP_LOGW(TAG, "%s", WIFI_INIT_RETRY_ABORTED_LOG);
            return false;
        }
        if (attempt < kWifiInitializationAttempts) {
            ESP_LOGW(TAG,
                     WIFI_INIT_RETRY_FORMAT,
                     attempt + 1,
                     kWifiInitializationAttempts);
            vTaskDelay(kWifiInitializationRetryDelay);
        }
    }
    ESP_LOGW(TAG, "%s", WIFI_INIT_RETRY_EXHAUSTED_LOG);
    return false;
}

} // namespace

static bool stop_wifi_radio_internal(WifiRadioStopAttempt attempt);

static bool apply_station_config(bool reconnect)
{
    wifi_config_t sta_config = {};
    if (!network_wifi_credentials_copy(
            reinterpret_cast<char *>(sta_config.sta.ssid),
            sizeof(sta_config.sta.ssid),
            reinterpret_cast<char *>(sta_config.sta.password),
            sizeof(sta_config.sta.password))) {
        return false;
    }
    sta_config.sta.threshold.authmode =
        sta_config.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_STA_CONFIG_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    if (reconnect) {
        if (!disconnect_station_and_clear_state()) {
            return false;
        }
        if (!start_station_connection(StationConnectAttempt::Start)) {
            return false;
        }
    }
    return true;
}

static void configure_wifi_power_save(bool enable_setup_portal)
{
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 0; attempt < kWifiPowerSaveAttempts; ++attempt) {
        err = esp_wifi_set_ps(enable_setup_portal ? WIFI_PS_NONE
                                                  : WIFI_PS_MAX_MODEM);
        if (err == ESP_OK) {
            return;
        }
        if (attempt + 1 < kWifiPowerSaveAttempts) {
            vTaskDelay(kWifiPowerSaveRetryDelay);
        }
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
    if (entering_setup_portal && !stop_http_server()) {
        (void)request_setup_portal_stop();
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
    const bool station_connected =
        app_event_group_ready() &&
        ((app_event_group_get_bits() & kWifiConnectedBit) != 0);
    const bool xiaozhi_keepalive_active = xiaozhi_ai_network_keepalive_active();
    if (!enable_setup_portal) {
        if (!stop_http_server()) {
            (void)request_setup_portal_stop();
            return false;
        }
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (mode_err != ESP_OK) {
            ESP_LOGW(TAG, WIFI_STA_ONLY_MODE_FAILED_FORMAT, esp_err_to_name(mode_err));
            return false;
        }
        if (should_reconfigure_running_power_save(enable_setup_portal,
                                                  xiaozhi_keepalive_active)) {
            configure_wifi_power_save(false);
        }
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
            // Keep the setup portal available even if the driver rejects this
            // best-effort disconnect, but do not publish a false offline state.
            (void)disconnect_station_and_clear_state();
        }
        if (!setup_portal_active_load()) {
            if (!start_http_server()) {
                rollback_running_setup_transition(previous_mode,
                                                  entering_setup_portal);
                return false;
            }
            log_setup_ap_active();
        }
        configure_wifi_power_save(true);
    }
    if (network_wifi_credentials_configured() &&
        should_reconnect_running_station(enable_setup_portal,
                                         station_connected)) {
        if (!begin_wifi_failover_session() ||
            !apply_station_config(true)) {
            if (station_config_failure_blocks_start(enable_setup_portal)) {
                return false;
            }
            ESP_LOGW(TAG, "%s", WIFI_STA_CONFIG_PORTAL_FALLBACK_LOG);
        }
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
        if (!begin_wifi_failover_session() ||
            !apply_station_config(false)) {
            if (station_config_failure_blocks_start(enable_setup_portal)) {
                return false;
            }
            ESP_LOGW(TAG, "%s", WIFI_STA_CONFIG_PORTAL_FALLBACK_LOG);
        }
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_START_FAILED_FORMAT, esp_err_to_name(err));
        // esp_wifi_start() can fail after the driver has begun changing state.
        // Publish conservative ownership and use the common forced-stop path so
        // a partial start cannot be mistaken for an already powered-off radio.
        wifi_radio_on_store(true);
        (void)stop_wifi_radio_internal(WifiRadioStopAttempt::kForced);
        return false;
    }
    configure_wifi_power_save(enable_setup_portal);
    if (enable_setup_portal) {
        if (!start_http_server()) {
            // The driver is already running even though the portal could not
            // start. Publish that ownership before using the common forced-stop
            // path so a stop failure remains observable and retryable.
            wifi_radio_on_store(true);
            (void)stop_wifi_radio_internal(WifiRadioStopAttempt::kForced);
            return false;
        }
        if (entering_setup_portal) {
            request_setup_prompt_once();
        }
        log_setup_ap_active();
    }
    wifi_radio_on_store(true);
    notify_ui_task();
    return true;
}

bool start_wifi_radio(bool enable_setup_portal)
{
    ScopedSemaphoreLock lifecycle_lock(s_wifi_lifecycle_mutex);
    if (!lifecycle_lock) {
        ESP_LOGW(TAG, "%s", WIFI_LIFECYCLE_MUTEX_UNAVAILABLE_LOG);
        return false;
    }
    ++s_wifi_power_generation;
    if (offline_mode_enabled_load() && !enable_setup_portal) {
        ESP_LOGI(TAG, WIFI_START_SKIPPED_OFFLINE_LOG);
        return false;
    }
    if (!initialize_wifi_driver_with_retry()) {
        return false;
    }
    bool entering_setup_portal = enable_setup_portal && !setup_portal_active_load();
    if (entering_setup_portal) {
        wifi_portal_session_reset();
    }
    const bool started =
        wifi_radio_on_load()
            ? configure_running_wifi_radio(enable_setup_portal,
                                           entering_setup_portal)
            : start_stopped_wifi_radio(enable_setup_portal,
                                       entering_setup_portal);
    if (started) {
        // A successful explicit start owns the radio again. Cancel any close
        // retry retained after an earlier driver failure only after the new
        // mode/configuration is fully usable.
        clear_wifi_stop_when_idle_request();
        s_wifi_stop_requested.store(false, std::memory_order_release);
    }
    return started;
}

bool wait_for_wifi_connected(uint32_t timeout_ms, uint32_t cancel_bits)
{
    if (!app_event_group_ready()) {
        ESP_LOGW(TAG, "%s", WIFI_WAIT_SKIPPED_EVENT_GROUP_UNAVAILABLE_LOG);
        return false;
    }
    const EventBits_t cancellation_bits = static_cast<EventBits_t>(cancel_bits);
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    const TickType_t started_at = xTaskGetTickCount();
    const WifiCredentialSlot initial_slot =
        wifi_failover_current_slot_snapshot();
    const bool alternate_configured =
        network_wifi_alternate_slot_configured();
    TickType_t failover_window = timeout_ticks / 2;
    if (failover_window > kWifiPrimaryAttemptWindowTicks) {
        failover_window = kWifiPrimaryAttemptWindowTicks;
    }
    if (failover_window == 0 && timeout_ticks > 0) {
        failover_window = 1;
    }
    bool failover_window_consumed = !alternate_configured;

    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - started_at;
        if (elapsed >= timeout_ticks) {
            return false;
        }
        const TickType_t remaining = timeout_ticks - elapsed;
        const TickType_t wait_ticks =
            !failover_window_consumed && failover_window < remaining
                ? failover_window
                : remaining;
        const EventBits_t bits = app_event_group_wait_bits(
            kWifiConnectedBit | cancellation_bits,
            pdFALSE,
            pdFALSE,
            wait_ticks);
        if ((bits & cancellation_bits) != 0) {
            return false;
        }
        if ((bits & kWifiConnectedBit) != 0) {
            return true;
        }
        if (!failover_window_consumed) {
            failover_window_consumed = true;
            (void)force_wifi_failover_switch(initial_slot);
        }
    }
}

bool prepare_setup_portal_result_delivery()
{
    ScopedSemaphoreLock lifecycle_lock(s_wifi_lifecycle_mutex);
    if (!lifecycle_lock) {
        ESP_LOGW(TAG, "%s", WIFI_LIFECYCLE_MUTEX_UNAVAILABLE_LOG);
        return false;
    }
    if (!wifi_radio_on_load() || !setup_portal_active_load()) {
        return false;
    }

    // APSTA must follow the router channel while credentials are validated.
    // Return to AP-only before publishing the result so the phone can rejoin
    // the original setup channel with its existing DHCP lease.
    persist_connected_wifi_slot_if_needed();
    wifi_portal_ap_channel_transition_begin();
    s_wifi_stop_requested.store(true, std::memory_order_release);
    esp_err_t disconnect_err = esp_wifi_disconnect();
    if (disconnect_err != ESP_OK &&
        disconnect_err != ESP_ERR_WIFI_NOT_CONNECT &&
        disconnect_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG,
                 WIFI_SETUP_RESULT_STA_DISCONNECT_FAILED_FORMAT,
                 esp_err_to_name(disconnect_err));
    }
    esp_err_t mode_err = ESP_FAIL;
    for (unsigned attempt = 0;
         attempt < kSetupResultDeliveryAttempts;
         ++attempt) {
        mode_err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (mode_err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG,
                 WIFI_SETUP_RESULT_AP_ONLY_FAILED_FORMAT,
                 esp_err_to_name(mode_err));
        if (attempt + 1 < kSetupResultDeliveryAttempts) {
            vTaskDelay(pdMS_TO_TICKS(kSetupResultDeliveryRetryMs));
        }
    }
    s_wifi_stop_requested.store(false, std::memory_order_release);
    if (mode_err != ESP_OK) {
        wifi_portal_ap_channel_transition_end();
        return false;
    }

    clear_sta_connection_state();
    notify_ui_task();
    ESP_LOGI(TAG, "%s", WIFI_SETUP_RESULT_AP_ONLY_READY_LOG);
    return true;
}

static bool stop_wifi_radio_internal(WifiRadioStopAttempt attempt)
{
    const bool force_setup_portal = attempt == WifiRadioStopAttempt::kForced;
    const bool explicit_stop_requested =
        attempt != WifiRadioStopAttempt::kNormal;
    if (!wifi_radio_on_load()) {
        if (force_setup_portal && !stop_http_server()) {
            return false;
        }
        clear_wifi_stop_when_idle_request();
        return true;
    }
    int ota_state = ota_runtime_state_load();
    if ((ota_state == kOtaChecking || ota_state == kOtaUpdating) && !force_setup_portal) {
        ESP_LOGI(TAG, WIFI_STOP_SKIPPED_OTA_LOG);
        return false;
    }
    if (xiaozhi_ai_network_keepalive_active() && !force_setup_portal) {
        ESP_LOGI(TAG, WIFI_STOP_SKIPPED_XIAOZHI_LOG);
        return false;
    }
    if (setup_portal_active_load() && !force_setup_portal) {
        return false;
    }
    if (!network_wifi_credentials_configured() && !explicit_stop_requested) {
        return false;
    }
    if (attempt == WifiRadioStopAttempt::kNormal) {
        PowerLockDepthSnapshot lock_depth = {};
        const bool lock_depth_ok =
            get_power_lock_depth_snapshot(&lock_depth);
        if (!wifi_owned_normal_stop_allowed(lock_depth_ok,
                                            lock_depth.network)) {
            ESP_LOGI(TAG,
                     WIFI_STOP_DEFERRED_OWNER_FORMAT,
                     lock_depth_ok,
                     lock_depth.network);
            return false;
        }
    }
    persist_connected_wifi_slot_if_needed();
    // Publish deliberate-stop ownership before stopping the HTTP server.
    // Otherwise a concurrent STA disconnect can still start a reconnect while
    // the portal teardown and esp_wifi_stop() sequence is already in flight.
    s_wifi_stop_requested.store(true, std::memory_order_release);
    const bool portal_stopped = stop_http_server();
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, WIFI_RUNNING_MODE_READ_FAILED_FORMAT, esp_err_to_name(err));
    } else if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        err = esp_wifi_disconnect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, WIFI_DISCONNECT_DURING_STOP_FAILED_FORMAT, esp_err_to_name(err));
        }
    }
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, WIFI_STOP_FAILED_FORMAT, esp_err_to_name(err));
        // Keep deliberate-stop ownership while the retry is pending. Clearing
        // it here lets the asynchronous disconnect event reconnect the STA
        // between failed stop attempts and extends the high-power window.
        s_wifi_stop_when_idle_requested.store(true,
                                              std::memory_order_release);
        if (attempt != WifiRadioStopAttempt::kIdleRetry) {
            notify_wifi_stop_retry_if_pending();
        }
        return false;
    } else {
        wifi_radio_on_store(false);
        clear_wifi_stop_when_idle_request();
        s_wifi_stop_requested.store(false, std::memory_order_release);
        clear_sta_connection_state();
        if (!portal_stopped) {
            // The radio is already off, but a failed httpd_stop() still owns
            // its task and the display DMA guard. Hand that cleanup back to
            // the serialized network task instead of waiting for a later
            // portal start to discover the stale server.
            (void)request_setup_portal_stop();
        }
        notify_ui_task();
        ESP_LOGI(TAG, WIFI_RADIO_OFF_LOG);
        return portal_stopped;
    }
}

void stop_wifi_radio(bool force_setup_portal)
{
    ScopedSemaphoreLock lifecycle_lock(s_wifi_lifecycle_mutex);
    if (!lifecycle_lock) {
        ESP_LOGW(TAG, "%s", WIFI_LIFECYCLE_MUTEX_UNAVAILABLE_LOG);
        if (force_setup_portal && setup_portal_active_load()) {
            (void)request_setup_portal_stop();
        }
        return;
    }
    const bool stopped = stop_wifi_radio_internal(
        force_setup_portal
            ? WifiRadioStopAttempt::kForced
            : WifiRadioStopAttempt::kNormal);
    if (force_setup_portal && !stopped && setup_portal_active_load()) {
        (void)request_setup_portal_stop();
    }
}

WeatherWifiPerformanceGuard::WeatherWifiPerformanceGuard(bool enabled)
{
    if (!enabled) {
        return;
    }
    ScopedSemaphoreLock lock(s_wifi_lifecycle_mutex);
    if (!lock || !wifi_radio_on_load() || setup_portal_active_load() ||
        xiaozhi_ai_network_keepalive_active()) {
        return;
    }
    wifi_ps_type_t previous = WIFI_PS_NONE;
    esp_err_t err = esp_wifi_get_ps(&previous);
    if (err == ESP_OK && previous != WIFI_PS_NONE) {
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err == ESP_OK) {
            previous_mode_ = static_cast<int>(previous);
            generation_ = s_wifi_power_generation;
            changed_ = true;
            ESP_LOGI(TAG, "weather Wi-Fi performance window: power save off");
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "weather Wi-Fi performance request failed: %s", esp_err_to_name(err));
    }
}

WeatherWifiPerformanceGuard::~WeatherWifiPerformanceGuard()
{
    if (!changed_) {
        return;
    }
    ScopedSemaphoreLock lock(s_wifi_lifecycle_mutex);
    // A new start/mode transition owns its power policy; never restore stale state.
    if (!lock || !wifi_radio_on_load() || generation_ != s_wifi_power_generation ||
        setup_portal_active_load() || xiaozhi_ai_network_keepalive_active()) {
        return;
    }
    wifi_ps_type_t current = WIFI_PS_NONE;
    esp_err_t err = esp_wifi_get_ps(&current);
    if (err == ESP_OK && current == WIFI_PS_NONE) {
        err = esp_wifi_set_ps(static_cast<wifi_ps_type_t>(previous_mode_));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "weather Wi-Fi power save restore failed: %s", esp_err_to_name(err));
    }
}

void request_wifi_radio_stop_when_idle()
{
    s_wifi_stop_when_idle_requested.store(true, std::memory_order_release);
    // Only the serialized network task services an ordinary deferred close.
    // Calling the driver here lets Xiaozhi/OTA cleanup race a newly acquired
    // network window between its ownership check and esp_wifi_stop().
    notify_wifi_stop_retry_if_pending();
}

void request_wifi_radio_stop_if_running()
{
    if (wifi_radio_on_load()) {
        request_wifi_radio_stop_when_idle();
    }
}

WifiRadioIdleStopResult service_wifi_radio_stop_when_idle()
{
    if (!s_wifi_stop_when_idle_requested.load(std::memory_order_acquire)) {
        return WifiRadioIdleStopResult::kNoRequest;
    }
    ScopedSemaphoreLock lifecycle_lock(s_wifi_lifecycle_mutex);
    if (!lifecycle_lock) {
        ESP_LOGW(TAG, "%s", WIFI_LIFECYCLE_MUTEX_UNAVAILABLE_LOG);
        return WifiRadioIdleStopResult::kRetryRequired;
    }
    const bool requested =
        s_wifi_stop_when_idle_requested.load(std::memory_order_acquire);
    if (!requested) {
        return WifiRadioIdleStopResult::kNoRequest;
    }
    if (!wifi_radio_on_load()) {
        clear_wifi_stop_when_idle_request();
        return WifiRadioIdleStopResult::kStopped;
    }
    WifiIdleStopPolicyInput policy = {};
    policy.requested = requested;
    policy.radio_on = true;
    policy.setup_portal_active = setup_portal_active_load();
    int ota_state = ota_runtime_state_load();
    policy.ota_active = ota_state == kOtaChecking || ota_state == kOtaUpdating;
    policy.xiaozhi_keepalive_active = xiaozhi_ai_network_keepalive_active();
    policy.network_lock_active = network_awake_lock_active();
    if (!wifi_idle_stop_allowed(policy)) {
        return WifiRadioIdleStopResult::kDeferred;
    }

    // stop_wifi_radio() 只在射频已经关闭或成功关闭时结算请求；失败、
    // 保护状态或所有权变化会让请求保持置位，交给下一次收尾重试。
    return stop_wifi_radio_internal(WifiRadioStopAttempt::kIdleRetry)
               ? WifiRadioIdleStopResult::kStopped
               : WifiRadioIdleStopResult::kRetryRequired;
}

static void wifi_event_handler(void *,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
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
        reconnect_station_after_disconnect_if_allowed();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (!event) {
            ESP_LOGW(TAG, WIFI_GOT_IP_EVENT_MISSING_LOG);
            return;
        }
        ESP_LOGI(TAG, WIFI_GOT_IP_FORMAT, IP2STR(&event->ip_info.ip));
        record_wifi_connection_success();
        format_sta_ip_or_clear(&event->ip_info.ip);
        set_wifi_connected_event(true);
        notify_ui_task();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_portal_ap_channel_transition_end();
        const uint8_t client_count =
            wifi_portal_ap_client_connected(kSetupApMaxConnections);
        ESP_LOGI(TAG,
                 WIFI_SETUP_AP_CLIENT_COUNT_FORMAT,
                 (unsigned)client_count);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const uint8_t client_count = wifi_portal_ap_client_disconnected();
        ESP_LOGI(TAG,
                 WIFI_SETUP_AP_CLIENT_COUNT_FORMAT,
                 (unsigned)client_count);
        if (wifi_portal_should_restart_dhcp()) {
            (void)restart_captive_portal_dhcp(s_ap_netif);
        }
    }
}

void init_wifi()
{
    if (!s_wifi_lifecycle_mutex.handle() &&
        !s_wifi_lifecycle_mutex.init()) {
        ESP_LOGW(TAG, "%s", WIFI_LIFECYCLE_MUTEX_UNAVAILABLE_LOG);
        return;
    }
    if (!s_wifi_failover_mutex.handle() &&
        !s_wifi_failover_mutex.init()) {
        ESP_LOGW(TAG, "%s", WIFI_FAILOVER_MUTEX_UNAVAILABLE_LOG);
        return;
    }
    uint8_t mac[6] = {};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, WIFI_MAC_READ_FAILED_FORMAT, esp_err_to_name(err));
    }
    format_setup_ap_ssid(mac[4], mac[5]);

    bool initialized = false;
    {
        ScopedSemaphoreLock lifecycle_lock(s_wifi_lifecycle_mutex);
        if (!lifecycle_lock) {
            ESP_LOGW(TAG, "%s", WIFI_LIFECYCLE_MUTEX_UNAVAILABLE_LOG);
            return;
        }
        initialized = initialize_wifi_driver_with_retry();
    }
    if (!initialized) {
        return;
    }

    if (!network_all_online_credentials_configured() &&
        !offline_mode_enabled_load()) {
        if (!start_wifi_radio(true) && !request_setup_portal_start()) {
            ESP_LOGW(TAG, "%s", WIFI_SETUP_START_RECOVERY_QUEUE_FAILED_LOG);
        }
    }
}
