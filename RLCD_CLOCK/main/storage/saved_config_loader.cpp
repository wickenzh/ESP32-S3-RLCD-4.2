// 在启动阶段读取 NVS，并一次发布联网凭据和普通设备设置运行态。
#include "saved_config_loader_internal.h"

#include "active_work_page_state_internal.h"
#include "app_metadata.h"
#include "chime_runtime_state_internal.h"
#include "chime_settings.h"
#include "device_settings_persistence.h"
#include "manual_weather_city_state_internal.h"
#include "network_chime_storage.h"
#include "network_config_keys.h"
#include "network_config_nvs.h"
#include "network_credentials_state_internal.h"
#include "network_page_storage.h"
#include "network_page_storage_policy.h"
#include "qweather_api_host.h"
#include "weather_provider.h"
#include "network_weather_city_storage.h"
#include "offline_mode_state_internal.h"
#include "ui_gallery_rotation_state_internal.h"
#include "ui_work_page_catalog_internal.h"
#include "xiaozhi_auto_return_state_internal.h"

#include <esp_attr.h>
#include <esp_log.h>

#include <stdint.h>
#include <type_traits>

using network_config_nvs::read_nvs_string;
using network_config_nvs::read_nvs_u8_or_default;
using network_config_nvs::ScopedNvsHandle;
using network_page_storage::read_saved_page_mask;
using network_page_storage::read_saved_page_order;
using network_config_keys::kOfflineModeKey;
using network_config_keys::kQweatherApiHostKey;
using network_config_keys::kWeatherApiKeyKey;
using network_config_keys::kWifiBackupPassKey;
using network_config_keys::kWifiBackupSsidKey;
using network_config_keys::kWifiPassKey;
using network_config_keys::kWifiPreferredSlotKey;
using network_config_keys::kWifiSsidKey;
using network_config_keys::kXiaozhiAutoReturnKey;
using network_config_keys::kGalleryRotationKey;

namespace {
constexpr uint8_t work_page_mask_bit(int page)
{
    return static_cast<uint8_t>(1U << page);
}

constexpr uint8_t kPageMaskV4KnownBits = network_page_storage::kLegacyV4KnownPageMask;
constexpr uint8_t kPageMaskV5KnownBits = network_page_storage::kCurrentKnownPageMask;
constexpr uint8_t kWeatherBoardPageMask = work_page_mask_bit(kWorkPageWeatherBoard);
constexpr uint8_t kFlipClockPageMask = work_page_mask_bit(kWorkPageFlipClock);
constexpr const char *kNvsActionLoadingConfig = "loading config";
constexpr const char *kOfflinePageMaskPersistFailedLog =
    "failed to persist offline-compatible page settings";

struct LoadedSavedConfig {
    esp_err_t ssid_err;
    esp_err_t pass_err;
    esp_err_t backup_ssid_err;
    esp_err_t backup_pass_err;
    esp_err_t key_err;
    esp_err_t host_err;
    char wifi_ssid[kNetworkWifiSsidLen];
    char wifi_password[kNetworkWifiPasswordLen];
    char backup_wifi_ssid[kNetworkWifiSsidLen];
    char backup_wifi_password[kNetworkWifiPasswordLen];
    char weather_api_key[kNetworkWeatherApiKeyLen];
    char weather_api_host[kQweatherApiHostLen];
    uint8_t chime;
    uint8_t all_day;
    uint8_t volume;
    uint8_t sound;
    uint8_t page_mask;
    uint8_t offline;
    uint8_t xiaozhi_auto_return;
    uint8_t gallery_rotation;
    uint8_t preferred_wifi_slot;
    uint8_t weather_provider;
    uint8_t page_order[kWorkPageCount];
    char manual_weather_city[kManualWeatherCityLen];
    bool have_page_order;
};
EXT_RAM_BSS_ATTR LoadedSavedConfig s_loaded_saved_config_workspace;

static_assert(kWorkPageCount <= 8, "work page enabled mask is stored as uint8_t");
static_assert((kPageMaskV4KnownBits & work_page_mask_bit(kWorkPageXiaozhiAI)) == 0,
              "page mask v4 must not include Xiaozhi AI page");
static_assert(kPageMaskV5KnownBits == static_cast<uint8_t>((1U << kWorkPageCount) - 1U),
              "page mask v5 must cover every current work page");
static_assert((kPageMaskV5KnownBits & kWeatherBoardPageMask) == kWeatherBoardPageMask,
              "weather board page must be covered by the current page mask");
static_assert((kPageMaskV5KnownBits & kFlipClockPageMask) == kFlipClockPageMask,
              "flip clock page must be covered by the current page mask");
static_assert(std::is_trivially_default_constructible<LoadedSavedConfig>::value,
              "saved config workspace must not require static construction");
static_assert(std::is_trivially_destructible<LoadedSavedConfig>::value,
              "saved config workspace is cleared without a destructor call");

uint8_t normalize_chime_sound_index(uint8_t sound)
{
    return sound < chime_settings::kSoundCount ? sound : 0;
}

constexpr bool nvs_u8_to_bool(uint8_t value)
{
    return value != 0;
}

bool apply_loaded_page_config(uint8_t page_mask,
                              const uint8_t *page_order,
                              bool have_page_order)
{
    const uint8_t online_mask = normalize_work_page_enabled_mask(page_mask);
    work_page_enabled_mask_store(online_mask);
    if (have_page_order && page_order) {
        work_page_order_replace(page_order, kWorkPageCount);
    } else {
        normalize_work_page_order();
    }
    uint8_t runtime_mask = online_mask;
    if (offline_mode_enabled_load()) {
        runtime_mask = work_page_mask_for_offline_mode(online_mask);
        if (runtime_mask != online_mask) {
            work_page_enabled_mask_store(runtime_mask);
        }
    }
    active_work_page_store(first_enabled_work_page());
    return runtime_mask != online_mask;
}

void clear_loaded_saved_config(LoadedSavedConfig *loaded)
{
    if (!loaded) {
        return;
    }
    volatile uint8_t *bytes =
        reinterpret_cast<volatile uint8_t *>(loaded);
    for (size_t remaining = sizeof(*loaded); remaining > 0; --remaining) {
        *bytes++ = 0;
    }
}

void initialize_loaded_saved_config(LoadedSavedConfig *loaded)
{
    if (!loaded) {
        return;
    }
    clear_loaded_saved_config(loaded);
    loaded->ssid_err = ESP_FAIL;
    loaded->pass_err = ESP_FAIL;
    loaded->backup_ssid_err = ESP_FAIL;
    loaded->backup_pass_err = ESP_FAIL;
    loaded->key_err = ESP_FAIL;
    loaded->host_err = ESP_FAIL;
    loaded->volume = chime_settings::kDefaultVolumePercent;
    loaded->page_mask = kPageMaskV5KnownBits;
    loaded->xiaozhi_auto_return = kDefaultXiaozhiAutoReturnEnabled ? 1 : 0;
    loaded->gallery_rotation = kDefaultGalleryRotationPeriod;
    loaded->preferred_wifi_slot =
        static_cast<uint8_t>(WifiCredentialSlot::kSlotA);
}

void read_saved_config(nvs_handle_t nvs, LoadedSavedConfig *loaded)
{
    if (!loaded) {
        return;
    }
    initialize_loaded_saved_config(loaded);
    loaded->ssid_err =
        read_nvs_string(nvs, kWifiSsidKey, loaded->wifi_ssid, sizeof(loaded->wifi_ssid));
    loaded->pass_err = read_nvs_string(
        nvs, kWifiPassKey, loaded->wifi_password, sizeof(loaded->wifi_password));
    loaded->backup_ssid_err = read_nvs_string(
        nvs,
        kWifiBackupSsidKey,
        loaded->backup_wifi_ssid,
        sizeof(loaded->backup_wifi_ssid));
    loaded->backup_pass_err = read_nvs_string(
        nvs,
        kWifiBackupPassKey,
        loaded->backup_wifi_password,
        sizeof(loaded->backup_wifi_password));
    loaded->key_err = read_nvs_string(
        nvs, kWeatherApiKeyKey, loaded->weather_api_key, sizeof(loaded->weather_api_key));
    loaded->host_err = read_nvs_string(
        nvs,
        kQweatherApiHostKey,
        loaded->weather_api_host,
        sizeof(loaded->weather_api_host));
    network_chime_storage::StoredChimeSettings chime =
        network_chime_storage::read(nvs, chime_settings::kDefaultVolumePercent);
    loaded->chime = chime.enabled;
    loaded->all_day = chime.all_day;
    loaded->volume = chime.volume;
    loaded->sound = chime.sound;
    loaded->page_mask = read_saved_page_mask(nvs);
    loaded->offline = read_nvs_u8_or_default(nvs, kOfflineModeKey, 0);
    loaded->weather_provider = read_nvs_u8_or_default(nvs, network_config_keys::kWeatherProviderKey, 0);
    loaded->xiaozhi_auto_return = read_nvs_u8_or_default(
        nvs, kXiaozhiAutoReturnKey, kDefaultXiaozhiAutoReturnEnabled ? 1 : 0);
    loaded->gallery_rotation = read_nvs_u8_or_default(
        nvs, kGalleryRotationKey, kDefaultGalleryRotationPeriod);
    loaded->preferred_wifi_slot = read_nvs_u8_or_default(
        nvs,
        kWifiPreferredSlotKey,
        static_cast<uint8_t>(WifiCredentialSlot::kSlotA));
    network_weather_city_storage::load_preferred_city(
        nvs, loaded->manual_weather_city, sizeof(loaded->manual_weather_city));
    loaded->have_page_order =
        read_saved_page_order(nvs, loaded->page_order, sizeof(loaded->page_order));
}

bool apply_loaded_config(const LoadedSavedConfig &loaded)
{
    weather_provider_store(loaded.weather_provider == 1 ? WeatherProvider::kOpenMeteo
                                                       : WeatherProvider::kQweather);
    const bool wifi_a_configured = loaded.ssid_err == ESP_OK &&
                                   loaded.pass_err == ESP_OK &&
                                   loaded.wifi_ssid[0] != '\0';
    const bool wifi_b_configured = loaded.backup_ssid_err == ESP_OK &&
                                   loaded.backup_pass_err == ESP_OK &&
                                   loaded.backup_wifi_ssid[0] != '\0';
    WifiCredentialSlot preferred_slot =
        loaded.preferred_wifi_slot ==
                static_cast<uint8_t>(WifiCredentialSlot::kSlotB)
            ? WifiCredentialSlot::kSlotB
            : WifiCredentialSlot::kSlotA;
    if ((preferred_slot == WifiCredentialSlot::kSlotA && !wifi_a_configured) ||
        (preferred_slot == WifiCredentialSlot::kSlotB && !wifi_b_configured)) {
        preferred_slot = wifi_a_configured ? WifiCredentialSlot::kSlotA
                                           : WifiCredentialSlot::kSlotB;
    }
    const bool weather_key_configured = loaded.key_err == ESP_OK &&
                                        loaded.weather_api_key[0] != '\0';
    const bool weather_host_configured = loaded.host_err == ESP_OK &&
                                         loaded.weather_api_host[0] != '\0' &&
                                         qweather_api_host_input_valid(
                                             loaded.weather_api_host);
    network_credentials_store(loaded.wifi_ssid,
                              loaded.wifi_password,
                              loaded.backup_wifi_ssid,
                              loaded.backup_wifi_password,
                              preferred_slot,
                              loaded.weather_api_key,
                              loaded.weather_api_host,
                              weather_key_configured,
                              weather_host_configured);
    manual_weather_city_store(loaded.manual_weather_city);
    ChimeRuntimeSnapshot chime = {
        nvs_u8_to_bool(loaded.chime),
        nvs_u8_to_bool(loaded.all_day),
        chime_settings::normalize_stored_volume(loaded.volume),
        static_cast<uint8_t>(normalize_chime_sound_index(loaded.sound)),
    };
    chime_runtime_snapshot_store(chime);
    offline_mode_enabled_store(nvs_u8_to_bool(loaded.offline));
    xiaozhi_auto_return_enabled_store(nvs_u8_to_bool(loaded.xiaozhi_auto_return));
    gallery_rotation_period_store(loaded.gallery_rotation);
    return apply_loaded_page_config(loaded.page_mask, loaded.page_order, loaded.have_page_order);
}
} // namespace

bool load_saved_config()
{
    LoadedSavedConfig &loaded = s_loaded_saved_config_workspace;
    ScopedNvsHandle nvs;
    esp_err_t open_err = nvs.open(NVS_READONLY, kNvsActionLoadingConfig, false);
    if (open_err != ESP_OK) {
        clear_loaded_saved_config(&loaded);
        return false;
    }
    read_saved_config(nvs.get(), &loaded);
    nvs.close();
    bool offline_page_mask_changed = apply_loaded_config(loaded);
    clear_loaded_saved_config(&loaded);
    if (offline_page_mask_changed &&
        !set_work_page_enabled_mask_setting(work_page_enabled_mask_load())) {
        ESP_LOGW(TAG, "%s", kOfflinePageMaskPersistFailedLog);
    }
    return offline_mode_enabled_load() ||
           network_all_online_credentials_configured();
}
