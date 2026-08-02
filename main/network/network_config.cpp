// 负责 Wi-Fi、API Key、页面设置和声音设置的 NVS 配置读写。
#include "network_services.h"
#include "wifi_portal_state.h"

#include "app_constexpr.h"
#include "app_text_format.h"
#include "alarm_services.h"
#include "chime_settings.h"
#include "network_chime_storage.h"
#include "network_config_keys.h"
#include "network_config_nvs.h"
#include "network_config_internal.h"
#include "network_credentials_state.h"
#include "manual_weather_city_state.h"
#include "network_factory_reset.h"
#include "network_page_storage.h"
#include "network_runtime_events.h"
#include "network_page_storage_policy.h"
#include "network_weather_city_storage.h"
#include "weather_city_text.h"
#include "xiaozhi_ai.h"

#include "ui_views.h"

using network_config_nvs::commit_nvs_if_changed;
using network_config_nvs::commit_nvs_if_ok;
using network_config_nvs::erase_nvs_key_if_present;
using network_config_nvs::read_nvs_string;
using network_config_nvs::read_nvs_u8_or_default;
using network_config_nvs::ScopedNvsHandle;
using network_config_nvs::set_nvs_str_if_ok;
using network_config_nvs::set_nvs_u8_if_ok;
using network_config_nvs::write_changed_nvs_u8;
using network_page_storage::kPageMaskV5Key;
using network_page_storage::read_saved_page_mask;
using network_page_storage::read_saved_page_order;
using network_page_storage::write_work_page_order_nvs;
using network_config_keys::kLegacyApiHostKey;
using network_config_keys::kOfflineModeKey;
using network_config_keys::kWeatherApiKeyKey;
using network_config_keys::kWifiPassKey;
using network_config_keys::kWifiSsidKey;
using network_config_keys::kXiaozhiAutoReturnKey;

namespace {
constexpr uint8_t work_page_mask_bit(int page)
{
    return static_cast<uint8_t>(1U << page);
}

constexpr uint8_t kPageMaskV4KnownBits = network_page_storage::kLegacyV4KnownPageMask;
constexpr uint8_t kPageMaskV5KnownBits = network_page_storage::kCurrentKnownPageMask;
constexpr uint8_t kDefaultWorkPageMask = kPageMaskV5KnownBits;
constexpr uint8_t kWeatherBoardPageMask = work_page_mask_bit(kWorkPageWeatherBoard);
constexpr uint8_t kRadioPageMask = work_page_mask_bit(kWorkPageRadio);
constexpr EventBits_t kNetworkRequestClearBits = kProvisioningSyncBit |
                                                 kManualNtpSyncBit |
                                                 kManualWeatherSyncBit |
                                                 kManualSayingSyncBit |
                                                 kNetworkDiagBit |
                                                 kOtaCheckBit |
                                                 kOtaInstallBit;
static_assert((kNetworkRequestClearBits & kManualWeatherSyncBit) != 0,
              "network request clear bits must include manual weather sync");
static_assert((kNetworkRequestClearBits & kOtaCheckBit) != 0 &&
                  (kNetworkRequestClearBits & kOtaInstallBit) != 0,
              "network request clear bits must include OTA request bits");
static_assert((kNetworkRequestClearBits & kNetworkStateChangedBit) == 0,
              "network runtime state notification is not a sync request");
constexpr const char *kConfigEventReasonNetworkRequestReset = "network request reset";
constexpr const char *kConfigEventReasonFactoryReset = "factory reset";
constexpr const char *kNvsActionLoadingConfig = "loading config";
constexpr const char *kNvsActionSavingOfflineMode = "saving offline mode";
constexpr const char *kNvsActionSavingConfig = "saving config";
constexpr const char *kNvsActionSavingWeatherCity = "saving weather city";
constexpr const char *kNvsActionClearingWeatherCity = "clearing weather city";
constexpr const char *kNvsActionSavingHourlyReminder = "saving hourly reminder";
constexpr const char *kNvsActionSavingPageSettings = "saving page settings";
constexpr const char *kNvsActionSavingPageOrder = "saving page order";
constexpr const char *kNvsActionSavingXiaozhiAutoReturn = "saving Xiaozhi auto return";
constexpr const char *kNvsActionClearingConfig = "clearing config";
constexpr const char *kEmptyWifiSsidSaveLog = "skip saving empty wifi ssid";
constexpr const char *kInvalidWeatherCitySaveLog = "skip saving invalid weather city";
constexpr const char *kOfflinePageMaskPersistFailedLog = "failed to persist offline-compatible page settings";
#define NVS_SAVE_OFFLINE_MODE_FAILED_FORMAT "nvs save offline mode failed: %s"
#define NVS_ERASE_LEGACY_API_HOST_FAILED_FORMAT "nvs erase legacy api host failed while saving config: %s"
#define NVS_SAVE_CONFIG_FAILED_FORMAT "nvs save config failed: %s"
#define NVS_SAVE_WEATHER_CITY_FAILED_FORMAT "nvs save weather city failed: %s"
#define NVS_CLEAR_WEATHER_CITY_FAILED_FORMAT "nvs clear weather city failed: %s"
#define NVS_SAVE_HOURLY_REMINDER_FAILED_FORMAT "nvs save hourly reminder failed: %s"
#define NVS_SAVE_PAGE_SETTINGS_FAILED_FORMAT "nvs save page settings failed: %s"
#define NVS_SAVE_PAGE_ORDER_FAILED_FORMAT "nvs save page order failed: %s"
#define NVS_SAVE_XIAOZHI_AUTO_RETURN_FAILED_FORMAT "nvs save Xiaozhi auto return failed: %s"
#define NVS_CLEAR_CONFIG_FAILED_FORMAT "nvs clear config failed: %s"
struct LoadedNetworkConfig {
    esp_err_t ssid_err = ESP_FAIL;
    esp_err_t pass_err = ESP_FAIL;
    esp_err_t key_err = ESP_FAIL;
    char wifi_ssid[kNetworkWifiSsidLen] = {};
    char wifi_password[kNetworkWifiPasswordLen] = {};
    char weather_api_key[kNetworkWeatherApiKeyLen] = {};
    uint8_t chime = 0;
    uint8_t all_day = 0;
    uint8_t volume = chime_settings::kDefaultVolumePercent;
    uint8_t sound = 0;
    uint8_t page_mask = kPageMaskV5KnownBits;
    uint8_t offline = 0;
    uint8_t xiaozhi_auto_return = 0;
    uint8_t page_order[kWorkPageCount] = {};
    char manual_weather_city[kManualWeatherCityLen] = {};
    bool have_page_order = false;
};

static_assert(kWorkPageCount <= 8, "work page enabled mask is stored as uint8_t");
static_assert((kPageMaskV4KnownBits & work_page_mask_bit(kWorkPageXiaozhiAI)) == 0,
              "page mask v4 must not include Xiaozhi AI page");
static_assert(kPageMaskV5KnownBits == static_cast<uint8_t>((1U << kWorkPageCount) - 1U),
              "page mask v5 must cover every current work page");
static_assert((kDefaultWorkPageMask & kPageMaskV5KnownBits) == kDefaultWorkPageMask &&
                  kDefaultWorkPageMask != 0,
              "default work page mask must enable at least one known page");
static_assert((kPageMaskV5KnownBits & kWeatherBoardPageMask) == kWeatherBoardPageMask,
              "weather board page must be covered by the current page mask");
static_assert((kPageMaskV5KnownBits & kRadioPageMask) == kRadioPageMask,
              "radio page must be covered by the current page mask");

uint8_t normalize_chime_sound_index(uint8_t sound)
{
    return sound < kChimeSoundCount ? sound : 0;
}

constexpr uint8_t bool_to_nvs_u8(bool value)
{
    return value ? 1 : 0;
}

constexpr bool nvs_u8_to_bool(uint8_t value)
{
    return value != 0;
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

void load_saved_manual_weather_city(nvs_handle_t nvs, char *out, size_t out_len)
{
    network_weather_city_storage::load_preferred_city(nvs, out, out_len);
}

bool apply_loaded_page_config(uint8_t page_mask, const uint8_t *page_order, bool have_page_order)
{
    work_page_enabled_mask_store(normalize_work_page_enabled_mask(page_mask));
    if (have_page_order && page_order) {
        work_page_order_replace(page_order, kWorkPageCount);
    } else {
        normalize_work_page_order();
    }
    uint8_t online_mask = work_page_enabled_mask_load();
    if (g_offline_mode_ui_enabled) {
        work_page_enabled_mask_store(work_page_mask_for_offline_mode(online_mask));
    }
    active_work_page_store(first_enabled_work_page());
    return online_mask != work_page_enabled_mask_load();
}

LoadedNetworkConfig read_loaded_network_config(nvs_handle_t nvs)
{
    LoadedNetworkConfig loaded = {};
    loaded.ssid_err =
        read_nvs_string(nvs, kWifiSsidKey, loaded.wifi_ssid, sizeof(loaded.wifi_ssid));
    loaded.pass_err = read_nvs_string(
        nvs, kWifiPassKey, loaded.wifi_password, sizeof(loaded.wifi_password));
    loaded.key_err = read_nvs_string(
        nvs, kWeatherApiKeyKey, loaded.weather_api_key, sizeof(loaded.weather_api_key));
    network_chime_storage::StoredChimeSettings chime =
        network_chime_storage::read(nvs, chime_settings::kDefaultVolumePercent);
    loaded.chime = chime.enabled;
    loaded.all_day = chime.all_day;
    loaded.volume = chime.volume;
    loaded.sound = chime.sound;
    loaded.page_mask = read_saved_page_mask(nvs);
    loaded.offline = read_nvs_u8_or_default(nvs, kOfflineModeKey, 0);
    loaded.xiaozhi_auto_return = read_nvs_u8_or_default(nvs, kXiaozhiAutoReturnKey, 0);
    load_saved_manual_weather_city(
        nvs, loaded.manual_weather_city, sizeof(loaded.manual_weather_city));
    loaded.have_page_order = read_saved_page_order(nvs, loaded.page_order, sizeof(loaded.page_order));
    return loaded;
}

bool apply_loaded_network_config(const LoadedNetworkConfig &loaded)
{
    const bool wifi_configured = loaded.ssid_err == ESP_OK &&
                                 loaded.pass_err == ESP_OK &&
                                 loaded.wifi_ssid[0] != '\0';
    const bool weather_key_configured = loaded.key_err == ESP_OK &&
                                        loaded.weather_api_key[0] != '\0';
    network_credentials_store(loaded.wifi_ssid,
                              loaded.wifi_password,
                              loaded.weather_api_key,
                              wifi_configured,
                              weather_key_configured);
    manual_weather_city_store(loaded.manual_weather_city);
    g_hourly_chime_enabled = nvs_u8_to_bool(loaded.chime);
    g_hourly_chime_all_day = nvs_u8_to_bool(loaded.all_day);
    g_offline_mode_ui_enabled = nvs_u8_to_bool(loaded.offline);
    g_xiaozhi_auto_return_enabled = nvs_u8_to_bool(loaded.xiaozhi_auto_return);
    g_chime_volume_percent = chime_settings::normalize_stored_volume(loaded.volume);
    g_chime_sound_index = normalize_chime_sound_index(loaded.sound);
    return apply_loaded_page_config(loaded.page_mask, loaded.page_order, loaded.have_page_order);
}
} // namespace

bool load_saved_config()
{
    ScopedNvsHandle nvs;
    esp_err_t open_err = nvs.open(NVS_READONLY, kNvsActionLoadingConfig, false);
    if (open_err != ESP_OK) {
        return false;
    }
    LoadedNetworkConfig loaded = read_loaded_network_config(nvs.get());
    nvs.close();
    bool offline_page_mask_changed = apply_loaded_network_config(loaded);
    if (offline_page_mask_changed && !save_work_page_settings()) {
        ESP_LOGW(TAG, "%s", kOfflinePageMaskPersistFailedLog);
    }
    return network_wifi_credentials_configured();
}

void clear_network_request_bits()
{
    clear_config_event_bits(kNetworkRequestClearBits, kConfigEventReasonNetworkRequestReset);
}

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
        err = write_changed_nvs_u8(nvs.get(), err, kPageMaskV5Key, next_page_mask, &page_mask_changed);
    }
    err = commit_nvs_if_changed(nvs.get(), err, offline_changed || page_mask_changed);
    if (!nvs.close_save_ok(err)) {
        ESP_LOGW(TAG, NVS_SAVE_OFFLINE_MODE_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    g_offline_mode_ui_enabled = enabled;
    if (enabled) {
        work_page_enabled_mask_store(next_page_mask);
        normalize_work_page_order();
        ensure_active_work_page_enabled();
        clear_network_request_bits();
        if (!setup_portal_active_load()) {
            stop_wifi_radio(true);
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
    return true;
}

static void reset_saved_config_runtime_state()
{
    network_credentials_clear();
    manual_weather_city_store("");
    clear_wifi_station_ip();
    g_offline_mode_ui_enabled = false;
    g_xiaozhi_auto_return_enabled = false;
    g_hourly_chime_enabled = false;
    g_hourly_chime_all_day = false;
    g_chime_volume_percent = chime_settings::kDefaultVolumePercent;
    g_chime_sound_index = 0;
    work_page_enabled_mask_store(kDefaultWorkPageMask);
    reset_work_page_order();
    active_work_page_store(first_enabled_work_page());
    clear_config_event_bits(kWifiConnectedBit | kWeatherReadyBit, kConfigEventReasonFactoryReset);
    clear_network_request_bits();
    notify_network_runtime_state_changed();
}

static void apply_saved_config_runtime_state(const char *ssid,
                                             const char *pass,
                                             const char *api_key,
                                             const char *weather_city)
{
    const char *saved_ssid = cstr_or_empty(ssid);
    const char *saved_password = cstr_or_empty(pass);
    const char *saved_api_key = cstr_or_empty(api_key);
    network_credentials_store(saved_ssid,
                              saved_password,
                              saved_api_key,
                              saved_ssid[0] != '\0',
                              saved_api_key[0] != '\0');
    manual_weather_city_store(weather_city);
}

static esp_err_t write_saved_config_nvs(nvs_handle_t nvs,
                                        const char *ssid,
                                        const char *pass,
                                        const char *api_key,
                                        const char *city)
{
    esp_err_t err = set_nvs_str_if_ok(nvs, ESP_OK, kWifiSsidKey, ssid);
    err = set_nvs_str_if_ok(nvs, err, kWifiPassKey, pass);
    err = set_nvs_str_if_ok(nvs, err, kWeatherApiKeyKey, api_key);
    err = network_weather_city_storage::write_provisioned_city(nvs, err, city);
    // Provisioning credentials are only useful after leaving offline mode. Keep both
    // changes in this transaction so a later NVS write cannot leave them out of sync.
    err = set_nvs_u8_if_ok(nvs, err, kOfflineModeKey, 0);
    esp_err_t legacy_erase_err = erase_nvs_key_if_present(nvs, kLegacyApiHostKey, nullptr);
    if (legacy_erase_err != ESP_OK) {
        ESP_LOGW(TAG, NVS_ERASE_LEGACY_API_HOST_FAILED_FORMAT,
                 esp_err_to_name(legacy_erase_err));
    }
    return err;
}

bool save_config(const char *ssid, const char *pass, const char *api_key, const char *weather_city)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "%s", kEmptyWifiSsidSaveLog);
        return false;
    }
    if (!pass) {
        pass = "";
    }
    if (!api_key) {
        api_key = "";
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
    err = write_saved_config_nvs(nvs.get(), ssid, pass, api_key, city);
    err = commit_nvs_if_ok(nvs.get(), err);
    nvs.close();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_SAVE_CONFIG_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    apply_saved_config_runtime_state(ssid, pass, api_key, city);
    g_offline_mode_ui_enabled = false;
    xiaozhi_ai_notify_network_configuration_changed();
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
    return true;
}

bool save_hourly_chime_setting()
{
    ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(NVS_READWRITE, kNvsActionSavingHourlyReminder);
    if (err != ESP_OK) {
        return false;
    }
    network_chime_storage::StoredChimeSettings settings = {};
    settings.enabled = bool_to_nvs_u8(g_hourly_chime_enabled);
    settings.all_day = bool_to_nvs_u8(g_hourly_chime_all_day);
    settings.volume = static_cast<uint8_t>(g_chime_volume_percent);
    settings.sound = static_cast<uint8_t>(g_chime_sound_index);
    bool changed = false;
    err = network_chime_storage::write_if_changed(nvs.get(), err, settings, &changed);
    err = commit_nvs_if_changed(nvs.get(), err, changed);
    if (!nvs.close_save_ok(err)) {
        ESP_LOGW(TAG, NVS_SAVE_HOURLY_REMINDER_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool save_work_page_settings()
{
    ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(NVS_READWRITE, kNvsActionSavingPageSettings);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t mask = normalize_work_page_enabled_mask(work_page_enabled_mask_load());
    bool changed = false;
    err = write_changed_nvs_u8(nvs.get(), err, kPageMaskV5Key, mask, &changed);
    err = commit_nvs_if_changed(nvs.get(), err, changed);
    if (!nvs.close_save_ok(err)) {
        ESP_LOGW(TAG, NVS_SAVE_PAGE_SETTINGS_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    work_page_enabled_mask_store(mask);
    return true;
}

bool save_work_page_order()
{
    normalize_work_page_order();
    uint8_t page_order[kWorkPageCount] = {};
    if (!work_page_order_copy(page_order, sizeof(page_order))) {
        return false;
    }
    ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(NVS_READWRITE, kNvsActionSavingPageOrder);
    if (err != ESP_OK) {
        return false;
    }
    bool changed = false;
    err = write_work_page_order_nvs(nvs.get(),
                                    err,
                                    page_order,
                                    sizeof(page_order),
                                    &changed);
    err = commit_nvs_if_changed(nvs.get(), err, changed);
    if (!nvs.close_save_ok(err)) {
        ESP_LOGW(TAG, NVS_SAVE_PAGE_ORDER_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool save_xiaozhi_auto_return_setting()
{
    ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(NVS_READWRITE, kNvsActionSavingXiaozhiAutoReturn);
    if (err != ESP_OK) {
        return false;
    }
    bool changed = false;
    err = write_changed_nvs_u8(nvs.get(),
                               err,
                               kXiaozhiAutoReturnKey,
                               bool_to_nvs_u8(g_xiaozhi_auto_return_enabled),
                               &changed);
    err = commit_nvs_if_changed(nvs.get(), err, changed);
    if (!nvs.close_save_ok(err)) {
        ESP_LOGW(TAG, NVS_SAVE_XIAOZHI_AUTO_RETURN_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool clear_saved_config()
{
    if (!clear_saved_config_nvs()) {
        return false;
    }
    if (!alarm_clear_saved_state()) {
        return false;
    }
    reset_saved_config_runtime_state();
    return true;
}
