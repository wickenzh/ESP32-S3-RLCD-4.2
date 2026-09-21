// 维护恢复出厂需要清除的普通用户配置 key 及条件提交规则。
#include "network_factory_reset.h"

#include "app_constexpr.h"
#include "app_metadata.h"
#include "network_chime_storage.h"
#include "network_config_keys.h"
#include "network_config_nvs.h"
#include "network_page_storage.h"
#include "network_weather_city_storage.h"

#include <esp_log.h>

namespace network_factory_reset {
namespace {
using network_chime_storage::kChimeSoundKey;
using network_chime_storage::kChimeVolumeKey;
using network_chime_storage::kHourlyAllDayKey;
using network_chime_storage::kHourlyChimeKey;
using network_config_keys::kQweatherApiHostKey;
using network_config_keys::kOfflineModeKey;
using network_config_keys::kWeatherApiKeyKey;
using network_config_keys::kWifiBackupPassKey;
using network_config_keys::kWifiBackupSsidKey;
using network_config_keys::kWifiPassKey;
using network_config_keys::kWifiPreferredSlotKey;
using network_config_keys::kWifiSsidKey;
using network_config_keys::kXiaozhiAutoReturnKey;
using network_config_keys::kGalleryRotationKey;
using network_page_storage::kPageMaskV1Key;
using network_page_storage::kPageMaskV2Key;
using network_page_storage::kPageMaskV3Key;
using network_page_storage::kPageMaskV4Key;
using network_page_storage::kPageMaskV5Key;
using network_page_storage::kPageMaskV6Key;
using network_page_storage::kPageOrderV1Key;
using network_page_storage::kPageOrderV2Key;
using network_page_storage::kPageOrderV3Key;
using network_page_storage::kPageOrderV4Key;
using network_page_storage::kPageOrderV5Key;
using network_page_storage::kPageOrderV6Key;
using network_weather_city_storage::kIgnoredAssetWeatherCityKey;
using network_weather_city_storage::kManualWeatherCityKey;

#define NVS_ERASE_KEY_CLEARING_CONFIG_FAILED_FORMAT \
    "nvs erase key %s failed while clearing config: %s"

constexpr const char *kSavedConfigKeys[] = {
    kWifiSsidKey,
    kWifiPassKey,
    kWifiBackupSsidKey,
    kWifiBackupPassKey,
    kWifiPreferredSlotKey,
    kWeatherApiKeyKey,
    kManualWeatherCityKey,
    kIgnoredAssetWeatherCityKey,
    kQweatherApiHostKey,
    network_config_keys::kWeatherProviderKey,
    kOfflineModeKey,
    kHourlyChimeKey,
    kHourlyAllDayKey,
    kChimeVolumeKey,
    kChimeSoundKey,
    kPageMaskV1Key,
    kPageMaskV2Key,
    kPageMaskV3Key,
    kPageMaskV4Key,
    kPageMaskV5Key,
    kPageMaskV6Key,
    kPageOrderV1Key,
    kPageOrderV2Key,
    kPageOrderV3Key,
    kPageOrderV4Key,
    kPageOrderV5Key,
    kPageOrderV6Key,
    kXiaozhiAutoReturnKey,
    kGalleryRotationKey,
};

static_assert(array_count(kSavedConfigKeys) == 29,
              "factory reset key registry count changed; update its host test");
static_assert(cstr_array_nonempty(kSavedConfigKeys),
              "factory reset config keys must be non-empty");
static_assert(cstr_array_unique(kSavedConfigKeys),
              "factory reset config keys must be unique");
static_assert(cstr_array_contains(kSavedConfigKeys, kWifiSsidKey),
              "factory reset must clear Wi-Fi SSID");
static_assert(cstr_array_contains(kSavedConfigKeys, kWifiPassKey),
              "factory reset must clear Wi-Fi password");
static_assert(cstr_array_contains(kSavedConfigKeys, kWifiBackupSsidKey),
              "factory reset must clear backup Wi-Fi SSID");
static_assert(cstr_array_contains(kSavedConfigKeys, kWifiBackupPassKey),
              "factory reset must clear backup Wi-Fi password");
static_assert(cstr_array_contains(kSavedConfigKeys, kWifiPreferredSlotKey),
              "factory reset must clear preferred Wi-Fi slot");
static_assert(cstr_array_contains(kSavedConfigKeys, kWeatherApiKeyKey),
              "factory reset must clear weather API key");
static_assert(cstr_array_contains(kSavedConfigKeys, kManualWeatherCityKey),
              "factory reset must clear manual weather city");
static_assert(cstr_array_contains(kSavedConfigKeys, kIgnoredAssetWeatherCityKey),
              "factory reset must clear ignored asset weather city");
static_assert(cstr_array_contains(kSavedConfigKeys, kQweatherApiHostKey),
              "factory reset must clear QWeather API Host");
static_assert(cstr_array_contains(kSavedConfigKeys, kOfflineModeKey),
              "factory reset must clear offline mode");
static_assert(cstr_array_contains(kSavedConfigKeys, kHourlyChimeKey),
              "factory reset must clear hourly reminder");
static_assert(cstr_array_contains(kSavedConfigKeys, kHourlyAllDayKey),
              "factory reset must clear all-day reminder");
static_assert(cstr_array_contains(kSavedConfigKeys, kChimeVolumeKey),
              "factory reset must clear reminder volume");
static_assert(cstr_array_contains(kSavedConfigKeys, kChimeSoundKey),
              "factory reset must clear reminder sound");
static_assert(cstr_array_contains(kSavedConfigKeys, kPageMaskV5Key),
              "factory reset must clear current page mask");
static_assert(cstr_array_contains(kSavedConfigKeys, kPageOrderV5Key),
              "factory reset must clear current page order");
static_assert(cstr_array_contains(kSavedConfigKeys, kXiaozhiAutoReturnKey),
              "factory reset must clear Xiaozhi auto-return setting");
static_assert(cstr_array_contains(kSavedConfigKeys, kGalleryRotationKey),
              "factory reset must clear gallery rotation setting");
} // namespace

esp_err_t erase_saved_config_keys(nvs_handle_t nvs)
{
    bool erased = false;
    esp_err_t err = ESP_OK;
    for (const char *key : kSavedConfigKeys) {
        bool key_erased = false;
        err = network_config_nvs::erase_nvs_key_if_present(
            nvs, key, &key_erased);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     NVS_ERASE_KEY_CLEARING_CONFIG_FAILED_FORMAT,
                     key,
                     esp_err_to_name(err));
            break;
        }
        erased = erased || key_erased;
    }
    return network_config_nvs::commit_nvs_if_changed(nvs, err, erased);
}

} // namespace network_factory_reset
