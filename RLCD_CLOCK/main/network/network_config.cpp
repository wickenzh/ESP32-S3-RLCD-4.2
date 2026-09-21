// 负责联网、离线和天气城市配置读写，以及恢复出厂运行态重置。
#include "network_config.h"
#include "weather_state_internal.h"
#include "wifi_radio_services.h"
#include "wifi_portal_state_internal.h"

#include "active_work_page_state_internal.h"
#include "app_constexpr.h"
#include "app_event_group.h"
#include "app_metadata.h"
#include "app_text_format.h"
#include "alarm_services.h"
#include "chime_runtime_state_internal.h"
#include "chime_settings.h"
#include "network_config_keys.h"
#include "network_config_nvs.h"
#include "network_config_internal.h"
#include "network_credentials_state_internal.h"
#include "offline_mode_state_internal.h"
#include "manual_weather_city_state_internal.h"
#include "network_factory_reset.h"
#include "network_page_storage.h"
#include "network_runtime_events.h"
#include "network_sync_requests.h"
#include "network_page_storage_policy.h"
#include "network_weather_city_storage.h"
#include "qweather_api_host.h"
#include "ui_work_page_catalog_internal.h"
#include "weather_city_text.h"
#include "xiaozhi_ai.h"
#include "xiaozhi_auto_return_state_internal.h"
#include "ui_gallery_rotation_state_internal.h"

#include <esp_log.h>

using network_config_nvs::commit_nvs_if_changed;
using network_config_nvs::ScopedNvsHandle;
using network_config_nvs::write_changed_nvs_string;
using network_config_nvs::write_changed_nvs_u8;
using network_page_storage::kPageMaskV6Key;
using network_config_keys::kQweatherApiHostKey;
using network_config_keys::kOfflineModeKey;
using network_config_keys::kWeatherApiKeyKey;
using network_config_keys::kWifiBackupPassKey;
using network_config_keys::kWifiBackupSsidKey;
using network_config_keys::kWifiPassKey;
using network_config_keys::kWifiPreferredSlotKey;
using network_config_keys::kWifiSsidKey;

namespace {
constexpr uint8_t kDefaultWorkPageMask = network_page_storage::kCurrentKnownPageMask;
constexpr const char *kConfigEventReasonFactoryReset = "factory reset";
constexpr const char *kNvsActionSavingOfflineMode = "saving offline mode";
constexpr const char *kNvsActionSavingConfig = "saving config";
constexpr const char *kNvsActionSavingWeatherCity = "saving weather city";
constexpr const char *kNvsActionClearingWeatherCity = "clearing weather city";
constexpr const char *kNvsActionClearingConfig = "clearing config";
constexpr const char *kEmptyWifiSsidSaveLog = "skip saving empty wifi ssid";
constexpr const char *kInvalidQweatherApiHostSaveLog =
    "skip saving invalid QWeather API Host";
constexpr const char *kInvalidWeatherCitySaveLog = "skip saving invalid weather city";
constexpr size_t kSavedConfigScratchLen = kQweatherApiHostLen;
#define NVS_SAVE_OFFLINE_MODE_FAILED_FORMAT "nvs save offline mode failed: %s"
#define NVS_SAVE_CONFIG_FAILED_FORMAT "nvs save config failed: %s"
#define NVS_SAVE_WEATHER_CITY_FAILED_FORMAT "nvs save weather city failed: %s"
#define NVS_CLEAR_WEATHER_CITY_FAILED_FORMAT "nvs clear weather city failed: %s"
#define NVS_CLEAR_CONFIG_FAILED_FORMAT "nvs clear config failed: %s"
static_assert(kDefaultWorkPageMask != 0,
              "default work page mask must enable at least one known page");
static_assert(kSavedConfigScratchLen >= kNetworkWifiSsidLen &&
                  kSavedConfigScratchLen >= kNetworkWifiPasswordLen &&
                  kSavedConfigScratchLen >= kNetworkWeatherApiKeyLen,
              "saved config comparison scratch must fit every credential field");

constexpr uint8_t bool_to_nvs_u8(bool value)
{
    return value ? 1 : 0;
}

bool clear_saved_config_nvs()
{
    ScopedNvsHandle nvs;
    esp_err_t open_err = nvs.open(NVS_READWRITE, kNvsActionClearingConfig);
    if (open_err != ESP_OK) {
        return false;
    }
    esp_err_t err = network_factory_reset::erase_saved_config_keys(nvs.get());
    nvs.close();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_CLEAR_CONFIG_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    return true;
}

} // namespace

static void notify_network_runtime_state_changed()
{
    notify_network_sync_runtime_state_changed();
    xiaozhi_ai_notify_network_configuration_changed();
}

bool set_offline_mode_enabled(bool enabled)
{
    ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(NVS_READWRITE, kNvsActionSavingOfflineMode);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t next_value = bool_to_nvs_u8(enabled);
    const uint8_t current_page_mask = work_page_enabled_mask_load();
    uint8_t next_page_mask = enabled
                                 ? work_page_mask_for_offline_mode(current_page_mask)
                                 : current_page_mask;
    bool offline_changed = false;
    bool page_mask_changed = false;
    err = write_changed_nvs_u8(nvs.get(), err, kOfflineModeKey, next_value, &offline_changed);
    if (enabled) {
        err = write_changed_nvs_u8(nvs.get(), err, kPageMaskV6Key, next_page_mask, &page_mask_changed);
    }
    err = commit_nvs_if_changed(nvs.get(), err, offline_changed || page_mask_changed);
    if (!nvs.close_save_ok(err)) {
        ESP_LOGW(TAG, NVS_SAVE_OFFLINE_MODE_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    offline_mode_enabled_store(enabled);
    if (enabled) {
        work_page_enabled_mask_store(next_page_mask);
        normalize_work_page_order();
        ensure_active_work_page_enabled();
        clear_network_request_bits();
        if (!setup_portal_active_load()) {
            stop_wifi_radio(true);
            request_wifi_radio_stop_if_running();
        }
    }
    notify_network_runtime_state_changed();
    return true;
}

bool can_leave_offline_mode_without_setup()
{
    return network_all_online_credentials_configured();
}

bool is_weather_city_input_valid(const char *city)
{
    return weather_city_text::input_valid(city, kManualWeatherCityLen);
}

bool normalize_weather_city_input(const char *city, char *out, size_t out_len)
{
    return weather_city_text::normalize(city, out, out_len);
}

static void copy_trimmed_weather_city(char *out, size_t out_len, const char *city)
{
    if (!normalize_weather_city_input(city, out, out_len) && app_text::output_buffer_available(out, out_len)) {
        out[0] = '\0';
    }
}

static bool finish_manual_weather_city_save(ScopedNvsHandle &nvs,
                                            esp_err_t err,
                                            const char *city,
                                            bool changed)
{
    err = commit_nvs_if_changed(nvs.get(), err, changed);
    nvs.close();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_SAVE_WEATHER_CITY_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    manual_weather_city_store(city);
    weather_provider_store(weather_provider_load());
    invalidate_weather_configuration();
    return true;
}

static void reset_saved_config_runtime_state()
{
    weather_provider_store(WeatherProvider::kQweather);
    network_credentials_clear();
    manual_weather_city_store("");
    invalidate_weather_configuration();
    clear_wifi_station_ip();
    offline_mode_enabled_store(false);
    xiaozhi_auto_return_enabled_store(kDefaultXiaozhiAutoReturnEnabled);
    gallery_rotation_period_store(kDefaultGalleryRotationPeriod);
    chime_runtime_snapshot_store({
        false,
        false,
        static_cast<uint8_t>(chime_settings::kDefaultVolumePercent),
        0,
    });
    work_page_enabled_mask_store(kDefaultWorkPageMask);
    reset_work_page_order();
    active_work_page_store(first_enabled_work_page());
    clear_config_event_bits(kWifiConnectedBit | kWeatherReadyBit, kConfigEventReasonFactoryReset);
    clear_network_request_bits();
    notify_network_runtime_state_changed();
}

static void apply_saved_config_runtime_state(const char *ssid,
                                             const char *pass,
                                             const char *backup_ssid,
                                             const char *backup_pass,
                                             const char *api_key,
                                             const char *api_host,
                                             const char *weather_city)
{
    const char *saved_ssid = cstr_or_empty(ssid);
    const char *saved_password = cstr_or_empty(pass);
    const char *saved_backup_ssid = cstr_or_empty(backup_ssid);
    const char *saved_backup_password = cstr_or_empty(backup_pass);
    const char *saved_api_key = cstr_or_empty(api_key);
    const char *saved_api_host = cstr_or_empty(api_host);
    network_credentials_store(saved_ssid,
                              saved_password,
                              saved_backup_ssid,
                              saved_backup_password,
                              WifiCredentialSlot::kSlotA,
                              saved_api_key,
                              saved_api_host,
                              saved_api_key[0] != '\0',
                              saved_api_host[0] != '\0');
    manual_weather_city_store(weather_city);
}

static esp_err_t write_saved_config_nvs(nvs_handle_t nvs,
                                        const char *ssid,
                                        const char *pass,
                                        const char *backup_ssid,
                                        const char *backup_pass,
                                        const char *api_key,
                                        const char *api_host,
                                        const char *city,
                                        WeatherProvider provider,
                                        bool *changed)
{
    if (changed) {
        *changed = false;
    }
    bool any_changed = false;
    bool item_changed = false;
    char scratch[kSavedConfigScratchLen] = {};
    esp_err_t err = write_changed_nvs_string(nvs,
                                             ESP_OK,
                                             kWifiSsidKey,
                                             ssid,
                                             scratch,
                                             sizeof(scratch),
                                             &item_changed);
    any_changed = any_changed || item_changed;
    if (backup_ssid[0] != '\0') {
        err = write_changed_nvs_string(nvs,
                                       err,
                                       kWifiBackupSsidKey,
                                       backup_ssid,
                                       scratch,
                                       sizeof(scratch),
                                       &item_changed);
        any_changed = any_changed || item_changed;
        err = write_changed_nvs_string(nvs,
                                       err,
                                       kWifiBackupPassKey,
                                       backup_pass,
                                       scratch,
                                       sizeof(scratch),
                                       &item_changed);
        any_changed = any_changed || item_changed;
    } else {
        err = network_config_nvs::write_changed_optional_nvs_string(
            nvs,
            err,
            kWifiBackupSsidKey,
            "",
            scratch,
            sizeof(scratch),
            &item_changed);
        any_changed = any_changed || item_changed;
        err = network_config_nvs::write_changed_optional_nvs_string(
            nvs,
            err,
            kWifiBackupPassKey,
            "",
            scratch,
            sizeof(scratch),
            &item_changed);
        any_changed = any_changed || item_changed;
    }
    err = write_changed_nvs_u8(nvs,
                               err,
                               kWifiPreferredSlotKey,
                               static_cast<uint8_t>(WifiCredentialSlot::kSlotA),
                               &item_changed);
    any_changed = any_changed || item_changed;
    err = write_changed_nvs_string(nvs,
                                   err,
                                   kWifiPassKey,
                                   pass,
                                   scratch,
                                   sizeof(scratch),
                                   &item_changed);
    any_changed = any_changed || item_changed;
    err = write_changed_nvs_string(nvs,
                                   err,
                                   kWeatherApiKeyKey,
                                   api_key,
                                   scratch,
                                   sizeof(scratch),
                                   &item_changed);
    any_changed = any_changed || item_changed;
    err = write_changed_nvs_string(nvs,
                                   err,
                                   kQweatherApiHostKey,
                                   api_host,
                                   scratch,
                                   sizeof(scratch),
                                   &item_changed);
    any_changed = any_changed || item_changed;
    err = network_weather_city_storage::write_provisioned_city(
        nvs, err, city, &item_changed);
    any_changed = any_changed || item_changed;
    // Provisioning credentials are only useful after leaving offline mode. Keep both
    // changes in this transaction so a later NVS write cannot leave them out of sync.
    err = write_changed_nvs_u8(nvs, err, kOfflineModeKey, 0, &item_changed);
    any_changed = any_changed || item_changed;
    err = write_changed_nvs_u8(nvs, err, network_config_keys::kWeatherProviderKey,
                              static_cast<uint8_t>(provider), &item_changed);
    any_changed = any_changed || item_changed;
    if (err == ESP_OK && changed) {
        *changed = any_changed;
    }
    return err;
}

bool save_config(const char *ssid,
                 const char *pass,
                 const char *backup_ssid,
                 const char *backup_pass,
                 const char *api_key,
                 const char *api_host,
                 const char *weather_city,
                 WeatherProvider provider)
{
    if(provider != WeatherProvider::kQweather && provider != WeatherProvider::kOpenMeteo) return false;
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "%s", kEmptyWifiSsidSaveLog);
        return false;
    }
    if (!pass) {
        pass = "";
    }
    if (!backup_ssid) {
        backup_ssid = "";
    }
    if (!backup_pass) {
        backup_pass = "";
    }
    if (!api_key) {
        api_key = "";
    }
    char normalized_api_host[kQweatherApiHostLen] = {};
    if (provider == WeatherProvider::kQweather && !normalize_qweather_api_host(api_host,
                                     normalized_api_host,
                                     sizeof(normalized_api_host))) {
        ESP_LOGW(TAG, "%s", kInvalidQweatherApiHostSaveLog);
        return false;
    }
    if (provider == WeatherProvider::kOpenMeteo && api_host) {
        strlcpy(normalized_api_host, api_host, sizeof(normalized_api_host));
    }
    char city[kManualWeatherCityLen] = {};
    copy_trimmed_weather_city(city, sizeof(city), weather_city);
    if (!is_weather_city_input_valid(city)) {
        ESP_LOGW(TAG, "%s", kInvalidWeatherCitySaveLog);
        return false;
    }
    ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(NVS_READWRITE, kNvsActionSavingConfig);
    if (err != ESP_OK) {
        return false;
    }
    bool changed = false;
    err = write_saved_config_nvs(nvs.get(),
                                 ssid,
                                 pass,
                                 backup_ssid,
                                 backup_pass,
                                 api_key,
                                 normalized_api_host,
                                 city,
                                 provider,
                                 &changed);
    err = commit_nvs_if_changed(nvs.get(), err, changed);
    nvs.close();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_SAVE_CONFIG_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    apply_saved_config_runtime_state(
        ssid,
        pass,
        backup_ssid,
        backup_pass,
        api_key,
        normalized_api_host,
        city);
    weather_provider_store(provider);
    invalidate_weather_configuration();
    offline_mode_enabled_store(false);
    xiaozhi_ai_notify_network_configuration_changed();
    return true;
}

bool persist_preferred_wifi_slot(WifiCredentialSlot slot)
{
    if (!wifi_credential_slot_valid(slot)) {
        return false;
    }
    if (slot == network_wifi_preferred_slot()) {
        return true;
    }
    ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(NVS_READWRITE, kNvsActionSavingConfig);
    if (err != ESP_OK) {
        return false;
    }
    bool changed = false;
    err = write_changed_nvs_u8(nvs.get(),
                               err,
                               kWifiPreferredSlotKey,
                               static_cast<uint8_t>(slot),
                               &changed);
    err = commit_nvs_if_changed(nvs.get(), err, changed);
    nvs.close();
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "failed to persist preferred Wi-Fi slot: %s",
                 esp_err_to_name(err));
        return false;
    }
    network_wifi_preferred_slot_store(slot);
    ESP_LOGI(TAG,
             "preferred Wi-Fi slot updated: %c",
             slot == WifiCredentialSlot::kSlotA ? 'A' : 'B');
    return true;
}

bool save_manual_weather_city(const char *city)
{
    char next[kManualWeatherCityLen] = {};
    copy_trimmed_weather_city(next, sizeof(next), city);
    if (next[0] == '\0') {
        return clear_manual_weather_city();
    }
    if (!is_weather_city_input_valid(next)) {
        ESP_LOGW(TAG, "%s", kInvalidWeatherCitySaveLog);
        return false;
    }
    ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(NVS_READWRITE, kNvsActionSavingWeatherCity);
    if (err != ESP_OK) {
        return false;
    }
    bool changed = false;
    err = network_weather_city_storage::write_manual_city_if_changed(
        nvs.get(), next, &changed);
    return finish_manual_weather_city_save(nvs, err, next, changed);
}

bool clear_manual_weather_city()
{
    char active_city[kManualWeatherCityLen] = {};
    (void)manual_weather_city_snapshot(active_city, sizeof(active_city));
    ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(NVS_READWRITE, kNvsActionClearingWeatherCity);
    if (err != ESP_OK) {
        return false;
    }
    bool changed = false;
    err = network_weather_city_storage::clear_manual_city(
        nvs.get(), active_city, &changed);
    err = commit_nvs_if_changed(nvs.get(), err, changed);
    nvs.close();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_CLEAR_WEATHER_CITY_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    manual_weather_city_store("");
    weather_provider_store(weather_provider_load());
    invalidate_weather_configuration();
    return true;
}

bool clear_saved_config()
{
    // alarm_v1 is a separate NVS namespace. Clear it before the Wi-Fi
    // namespace so an alarm-storage failure leaves credentials intact and the
    // user can retry instead of being stranded between persisted and runtime
    // configuration states.
    if (!alarm_clear_saved_state()) {
        return false;
    }
    if (!clear_saved_config_nvs()) {
        return false;
    }
    reset_saved_config_runtime_state();
    return true;
}
