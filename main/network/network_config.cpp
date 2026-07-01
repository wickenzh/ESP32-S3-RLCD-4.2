// 负责 Wi-Fi、API Key、页面设置和声音设置的 NVS 配置读写。
#include "network_services.h"

#include "sensor_services.h"
#include "ui_views.h"

#include <errno.h>

#define FORM_VALUE_TRUNCATED_LOG_FORMAT "form value truncated for key=%s len=%u cap=%u"

namespace {
constexpr const char *kWifiNvsNamespace = "wifi";
constexpr const char *kWifiSsidKey = "ssid";
constexpr const char *kWifiPassKey = "pass";
constexpr const char *kWeatherApiKeyKey = "api_key";
constexpr const char *kManualWeatherCityKey = "weather_city_v1";
constexpr const char *kLegacyApiHostKey = "api_host";
constexpr const char *kOfflineModeKey = "offline_v1";
constexpr const char *kHourlyChimeKey = "hourly_chime_v2";
constexpr const char *kHourlyAllDayKey = "hour_all_v1";
constexpr const char *kChimeVolumeKey = "chime_vol_v1";
constexpr const char *kChimeSoundKey = "chime_snd_v1";
constexpr const char *kPageMaskV1Key = "page_mask_v1";
constexpr const char *kPageMaskV2Key = "page_mask_v2";
constexpr const char *kPageMaskV3Key = "page_mask_v3";
constexpr const char *kPageOrderV1Key = "page_order_v1";
constexpr const char *kPageOrderV2Key = "page_order_v2";
constexpr const char *kPageOrderV3Key = "page_order_v3";
constexpr size_t kFormEncodedBufferSize = 160;
constexpr size_t kManualTimeFieldSize = 32;
constexpr size_t kSetupSsidFieldSize = 33;
constexpr size_t kSetupPasswordFieldSize = 65;
constexpr size_t kSetupApiKeyFieldSize = 96;
constexpr size_t kSetupWeatherCityFieldSize = kManualWeatherCityLen;
constexpr uint8_t kNvsUnsetU8 = 0xFF;
constexpr uint8_t kDefaultChimeVolumePercent = 80;
constexpr uint8_t kValidChimeVolumePercent[] = {20, 40, 60, 80, 100};
constexpr uint8_t kWeatherClockPageMask = (uint8_t)(1U << kWorkPageWeatherClock);
constexpr uint8_t kLegacyPageMaskV1KnownBits = (uint8_t)((1U << 4) - 1);
constexpr uint8_t kPageMaskV2KnownBits = (uint8_t)((1U << 5) - 1);
constexpr uint8_t kPageMaskV3KnownBits = (uint8_t)((1U << kWorkPageCount) - 1);
constexpr uint8_t kWeatherBoardPageMask = (uint8_t)(1U << kWorkPageWeatherBoard);
constexpr uint8_t kFlipClockPageMask = (uint8_t)(1U << kWorkPageFlipClock);
constexpr size_t kLegacyPageOrderV1Count = 4;
constexpr size_t kPageOrderV2Count = 5;
constexpr int kTmYearOffset = 1900;
constexpr int kTmMonthOffset = 1;
constexpr int kManualTimeMinMonth = 1;
constexpr int kManualTimeMaxMonth = 12;
constexpr int kManualTimeMinDay = 1;
constexpr int kManualTimeMaxDay = 31;
constexpr int kManualTimeMinHour = 0;
constexpr int kManualTimeMaxHour = 23;
constexpr int kManualTimeMinMinute = 0;
constexpr int kManualTimeMaxMinute = 59;
constexpr int kManualTimeMinSecond = 0;
constexpr int kManualTimeMaxSecond = 59;
constexpr int kManualTimeRequiredFieldCount = 5; // year, month, day, hour and minute.
constexpr const char *kManualTimeIsoSecondsFormat = "%d-%d-%dT%d:%d:%d";
constexpr const char *kManualTimeSpaceSecondsFormat = "%d-%d-%d %d:%d:%d";
constexpr const char *kManualTimeSpaceMinutesFormat = "%d-%d-%d %d:%d";
constexpr const char *kConfigEventReasonFallback = "config";
constexpr const char *kConfigEventReasonNetworkRequestReset = "network request reset";
constexpr const char *kConfigEventReasonFactoryReset = "factory reset";
constexpr const char *kConfigEventReasonOfflineManualTime = "offline manual time";
constexpr const char *kConfigEventReasonProvisioningSave = "provisioning save";
constexpr const char *kNvsActionLoadingConfig = "loading config";
constexpr const char *kNvsActionSavingOfflineMode = "saving offline mode";
constexpr const char *kNvsActionSavingConfig = "saving config";
constexpr const char *kNvsActionSavingWeatherCity = "saving weather city";
constexpr const char *kNvsActionClearingWeatherCity = "clearing weather city";
constexpr const char *kNvsActionSavingHourlyReminder = "saving hourly reminder";
constexpr const char *kNvsActionSavingPageSettings = "saving page settings";
constexpr const char *kNvsActionSavingPageOrder = "saving page order";
constexpr const char *kNvsActionClearingConfig = "clearing config";
constexpr const char *kInvalidWeatherCitySaveLog = "skip saving invalid weather city";
#define NVS_SAVE_OFFLINE_MODE_FAILED_FORMAT "nvs save offline mode failed: %s"
#define NVS_ERASE_LEGACY_API_HOST_FAILED_FORMAT "nvs erase legacy api host failed while saving config: %s"
#define NVS_SAVE_CONFIG_FAILED_FORMAT "nvs save config failed: %s"
#define NVS_SAVE_WEATHER_CITY_FAILED_FORMAT "nvs save weather city failed: %s"
#define NVS_CLEAR_WEATHER_CITY_FAILED_FORMAT "nvs clear weather city failed: %s"
#define NVS_SAVE_HOURLY_REMINDER_FAILED_FORMAT "nvs save hourly reminder failed: %s"
#define NVS_SAVE_PAGE_SETTINGS_FAILED_FORMAT "nvs save page settings failed: %s"
#define NVS_SAVE_PAGE_ORDER_FAILED_FORMAT "nvs save page order failed: %s"
#define NVS_ERASE_KEY_CLEARING_CONFIG_FAILED_FORMAT "nvs erase key %s failed while clearing config: %s"
#define NVS_CLEAR_CONFIG_FAILED_FORMAT "nvs clear config failed: %s"
constexpr const char *kFormManualTimeKey = "manual_time";
constexpr const char *kFormManualTimeFallbackKey = "datetime";
constexpr const char *kFormSsidKey = "ssid";
constexpr const char *kFormPasswordKey = "pass";
constexpr const char *kFormPasswordFallbackKey = "password";
constexpr const char *kFormApiKeyKey = "api_key";
constexpr const char *kFormApiKeyFallbackKey = "weather";
constexpr const char *kFormWeatherCityKey = "weather_city";
constexpr const char *kFormWeatherCityFallbackKey = "city";
constexpr const char *kInvalidWeatherCityChars = "&=?#%/\\<>\"'";
constexpr const char *kClearConfigKeys[] = {
    kWifiSsidKey,
    kWifiPassKey,
    kWeatherApiKeyKey,
    kManualWeatherCityKey,
    kLegacyApiHostKey,
    kOfflineModeKey,
};
static_assert(kWorkPageCount <= 8, "work page enabled mask is stored as uint8_t");
static_assert(kLegacyPageOrderV1Count <= kWorkPageCount);
static_assert(kPageOrderV2Count <= kWorkPageCount);

const char *config_event_reason_text(const char *reason)
{
    return reason ? reason : kConfigEventReasonFallback;
}

void clear_app_event_bits(EventBits_t bits, const char *reason)
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "skip clear event bits for %s: event group unavailable", config_event_reason_text(reason));
        return;
    }
    xEventGroupClearBits(g_app_events, bits);
}

void set_app_event_bits(EventBits_t bits, const char *reason)
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "skip set event bits for %s: event group unavailable", config_event_reason_text(reason));
        return;
    }
    xEventGroupSetBits(g_app_events, bits);
}

bool form_field_matches_key(const char *field, size_t field_len, const char *key, size_t key_len)
{
    return field && key &&
           field_len > key_len &&
           field[key_len] == '=' &&
           strncmp(field, key, key_len) == 0;
}

bool find_form_value_range(const char *body, const char *key, const char **value_start, size_t *value_len)
{
    if (!body || !key || key[0] == '\0' || !value_start || !value_len) {
        return false;
    }

    const size_t key_len = strlen(key);
    const char *field = body;
    while (*field) {
        const char *field_end = strchr(field, '&');
        const size_t field_len = field_end ? (size_t)(field_end - field) : strlen(field);
        if (form_field_matches_key(field, field_len, key, key_len)) {
            *value_start = field + key_len + 1;
            *value_len = field_len - key_len - 1;
            return true;
        }
        if (!field_end) {
            break;
        }
        field = field_end + 1;
    }
    return false;
}

int hex_digit_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool decode_url_percent_byte(const char *src, char *out)
{
    if (!src || !out || src[0] != '%' || src[1] == '\0' || src[2] == '\0') {
        return false;
    }
    const int hi = hex_digit_value(src[1]);
    const int lo = hex_digit_value(src[2]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    *out = (char)((hi << 4) | lo);
    return true;
}

bool value_in_range(int value, int min_value, int max_value)
{
    return value >= min_value && value <= max_value;
}

uint8_t normalize_chime_volume(uint8_t volume)
{
    for (uint8_t valid : kValidChimeVolumePercent) {
        if (volume == valid) {
            return volume;
        }
    }
    return kDefaultChimeVolumePercent;
}

esp_err_t erase_nvs_key_if_present(nvs_handle_t nvs, const char *key, bool *erased)
{
    esp_err_t err = nvs_erase_key(nvs, key);
    if (err == ESP_OK) {
        if (erased) {
            *erased = true;
        }
        return ESP_OK;
    }
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t open_wifi_nvs(nvs_open_mode_t mode, nvs_handle_t *nvs, const char *action, bool log_not_found = true)
{
    if (!nvs) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_open(kWifiNvsNamespace, mode, nvs);
    if (err != ESP_OK && (log_not_found || err != ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGW(TAG, "nvs open failed while %s: %s", action ? action : "accessing config", esp_err_to_name(err));
    }
    return err;
}

esp_err_t commit_nvs_if_ok(nvs_handle_t nvs, esp_err_t err)
{
    return err == ESP_OK ? nvs_commit(nvs) : err;
}

esp_err_t set_nvs_str_if_ok(nvs_handle_t nvs, esp_err_t err, const char *key, const char *value)
{
    return err == ESP_OK ? nvs_set_str(nvs, key, value) : err;
}

esp_err_t set_nvs_u8_if_ok(nvs_handle_t nvs, esp_err_t err, const char *key, uint8_t value)
{
    return err == ESP_OK ? nvs_set_u8(nvs, key, value) : err;
}

esp_err_t write_manual_weather_city_key(nvs_handle_t nvs, const char *city)
{
    return city && city[0] != '\0'
               ? nvs_set_str(nvs, kManualWeatherCityKey, city)
               : erase_nvs_key_if_present(nvs, kManualWeatherCityKey, nullptr);
}

uint8_t read_nvs_u8_or_default(nvs_handle_t nvs, const char *key, uint8_t default_value)
{
    uint8_t value = default_value;
    (void)nvs_get_u8(nvs, key, &value);
    return value;
}

esp_err_t read_nvs_string(nvs_handle_t nvs, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    return nvs_get_str(nvs, key, out, &len);
}

bool nvs_u8_matches(nvs_handle_t nvs, const char *key, uint8_t expected)
{
    uint8_t value = kNvsUnsetU8;
    return nvs_get_u8(nvs, key, &value) == ESP_OK && value == expected;
}

void append_work_page_for_migration(uint8_t *order, int *count, uint8_t page)
{
    if (!order || !count || *count >= kWorkPageCount) {
        return;
    }
    order[(*count)++] = page;
}

int migrate_order_with_flip_clock(const uint8_t *source, size_t source_count, uint8_t *dest)
{
    if (!source || !dest) {
        return 0;
    }
    int out = 0;
    for (size_t i = 0; i < source_count && out < kWorkPageCount; ++i) {
        append_work_page_for_migration(dest, &out, source[i]);
        if (source[i] == kWorkPageWeatherClock) {
            append_work_page_for_migration(dest, &out, kWorkPageFlipClock);
        }
    }
    return out;
}
} // namespace

bool load_saved_config()
{
    nvs_handle_t nvs;
    esp_err_t open_err = open_wifi_nvs(NVS_READONLY, &nvs, kNvsActionLoadingConfig, false);
    if (open_err != ESP_OK) {
        return false;
    }
    esp_err_t ssid_err = read_nvs_string(nvs, kWifiSsidKey, g_wifi_ssid, sizeof(g_wifi_ssid));
    esp_err_t pass_err = read_nvs_string(nvs, kWifiPassKey, g_wifi_pass, sizeof(g_wifi_pass));
    esp_err_t key_err = read_nvs_string(nvs, kWeatherApiKeyKey, g_weather_api_key, sizeof(g_weather_api_key));
    esp_err_t city_err = read_nvs_string(nvs, kManualWeatherCityKey, g_manual_weather_city, sizeof(g_manual_weather_city));
    uint8_t page_mask = kPageMaskV3KnownBits;
    uint8_t page_order[kWorkPageCount] = {};
    size_t page_order_len = sizeof(page_order);
    uint8_t chime = read_nvs_u8_or_default(nvs, kHourlyChimeKey, 0);
    uint8_t all_day = read_nvs_u8_or_default(nvs, kHourlyAllDayKey, 0);
    uint8_t volume = read_nvs_u8_or_default(nvs, kChimeVolumeKey, kDefaultChimeVolumePercent);
    uint8_t sound = read_nvs_u8_or_default(nvs, kChimeSoundKey, 0);
    if (nvs_get_u8(nvs, kPageMaskV3Key, &page_mask) != ESP_OK) {
        uint8_t v2_page_mask = kPageMaskV2KnownBits;
        if (nvs_get_u8(nvs, kPageMaskV2Key, &v2_page_mask) == ESP_OK) {
            page_mask = (v2_page_mask & kPageMaskV2KnownBits) | kFlipClockPageMask;
        } else {
            uint8_t legacy_page_mask = kLegacyPageMaskV1KnownBits;
            if (nvs_get_u8(nvs, kPageMaskV1Key, &legacy_page_mask) == ESP_OK) {
                page_mask = (legacy_page_mask & kLegacyPageMaskV1KnownBits) |
                            kWeatherBoardPageMask | kFlipClockPageMask;
            }
        }
    }
    uint8_t offline = read_nvs_u8_or_default(nvs, kOfflineModeKey, 0);
    esp_err_t order_err = nvs_get_blob(nvs, kPageOrderV3Key, page_order, &page_order_len);
    if (order_err != ESP_OK || page_order_len != sizeof(page_order)) {
        uint8_t v2_order[kPageOrderV2Count] = {};
        size_t v2_len = sizeof(v2_order);
        if (nvs_get_blob(nvs, kPageOrderV2Key, v2_order, &v2_len) == ESP_OK &&
            v2_len == sizeof(v2_order)) {
            int out = migrate_order_with_flip_clock(v2_order, kPageOrderV2Count, page_order);
            while (out < kWorkPageCount) {
                append_work_page_for_migration(page_order, &out, kWorkPageFlipClock);
            }
            page_order_len = sizeof(page_order);
            order_err = ESP_OK;
        } else {
            uint8_t legacy_order[kLegacyPageOrderV1Count] = {};
            size_t legacy_len = sizeof(legacy_order);
            if (nvs_get_blob(nvs, kPageOrderV1Key, legacy_order, &legacy_len) == ESP_OK &&
                legacy_len == sizeof(legacy_order)) {
                int out = migrate_order_with_flip_clock(legacy_order, kLegacyPageOrderV1Count, page_order);
                append_work_page_for_migration(page_order, &out, kWorkPageWeatherBoard);
                append_work_page_for_migration(page_order, &out, kWorkPageFlipClock);
                page_order_len = sizeof(page_order);
                order_err = ESP_OK;
            }
        }
    }
    nvs_close(nvs);
    g_have_weather_key = key_err == ESP_OK && g_weather_api_key[0] != '\0';
    if (city_err == ESP_OK) {
        trim_ascii(g_manual_weather_city);
    } else {
        g_manual_weather_city[0] = '\0';
    }
    g_has_manual_weather_city = g_manual_weather_city[0] != '\0';
    g_hourly_chime_enabled = chime != 0;
    g_hourly_chime_all_day = all_day != 0;
    g_offline_mode_ui_enabled = offline != 0;
    g_chime_volume_percent = normalize_chime_volume(volume);
    g_chime_sound_index = sound < kChimeSoundCount ? sound : 0;
    g_work_page_enabled_mask = (page_mask | kWeatherClockPageMask) & kPageMaskV3KnownBits;
    if (order_err == ESP_OK && page_order_len == sizeof(page_order)) {
        memcpy(g_work_page_order, page_order, sizeof(g_work_page_order));
    }
    normalize_work_page_order();
    g_active_work_page = first_enabled_work_page();
    return ssid_err == ESP_OK && pass_err == ESP_OK && g_wifi_ssid[0] != '\0';
}

void clear_network_request_bits()
{
    clear_app_event_bits(kProvisioningSyncBit |
                             kManualNtpSyncBit |
                             kManualWeatherSyncBit |
                             kManualSayingSyncBit |
                             kNetworkDiagBit |
                             kOtaCheckBit |
                             kOtaInstallBit,
                         kConfigEventReasonNetworkRequestReset);
}

bool set_offline_mode_enabled(bool enabled)
{
    nvs_handle_t nvs;
    esp_err_t err = open_wifi_nvs(NVS_READWRITE, &nvs, kNvsActionSavingOfflineMode);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t next_value = enabled ? 1 : 0;
    bool unchanged = nvs_u8_matches(nvs, kOfflineModeKey, next_value);
    if (!unchanged) {
        err = nvs_set_u8(nvs, kOfflineModeKey, next_value);
        err = commit_nvs_if_ok(nvs, err);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_SAVE_OFFLINE_MODE_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    g_offline_mode_ui_enabled = enabled;
    if (enabled) {
        clear_network_request_bits();
        if (!g_setup_portal_active) {
            stop_wifi_radio(true);
        }
    }
    return true;
}

bool can_leave_offline_mode_without_setup()
{
    return g_have_wifi_creds && g_have_weather_key;
}

bool weather_city_char_invalid(unsigned char ch)
{
    return ch < 0x20 || strchr(kInvalidWeatherCityChars, ch) != nullptr;
}

bool is_weather_city_input_valid(const char *city)
{
    if (!city || city[0] == '\0') {
        return true;
    }
    size_t len = strlen(city);
    if (len >= kManualWeatherCityLen) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (weather_city_char_invalid((unsigned char)city[i])) {
            return false;
        }
    }
    return true;
}

void copy_trimmed_weather_city(char *out, size_t out_len, const char *city)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!city) {
        return;
    }
    strlcpy(out, city, out_len);
    trim_ascii(out);
}

void set_manual_weather_city_state(const char *city)
{
    strlcpy(g_manual_weather_city, city ? city : "", sizeof(g_manual_weather_city));
    g_has_manual_weather_city = g_manual_weather_city[0] != '\0';
}

bool save_config(const char *ssid, const char *pass, const char *api_key, const char *weather_city)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "skip saving empty wifi ssid");
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
    nvs_handle_t nvs;
    esp_err_t err = open_wifi_nvs(NVS_READWRITE, &nvs, kNvsActionSavingConfig);
    if (err != ESP_OK) {
        return false;
    }
    err = set_nvs_str_if_ok(nvs, err, kWifiSsidKey, ssid);
    err = set_nvs_str_if_ok(nvs, err, kWifiPassKey, pass);
    err = set_nvs_str_if_ok(nvs, err, kWeatherApiKeyKey, api_key);
    if (err == ESP_OK) {
        err = write_manual_weather_city_key(nvs, city);
    }
    esp_err_t legacy_erase_err = erase_nvs_key_if_present(nvs, kLegacyApiHostKey, nullptr);
    if (legacy_erase_err != ESP_OK) {
        ESP_LOGW(TAG, NVS_ERASE_LEGACY_API_HOST_FAILED_FORMAT,
                 esp_err_to_name(legacy_erase_err));
    }
    err = commit_nvs_if_ok(nvs, err);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_SAVE_CONFIG_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));
    strlcpy(g_wifi_pass, pass, sizeof(g_wifi_pass));
    strlcpy(g_weather_api_key, api_key, sizeof(g_weather_api_key));
    set_manual_weather_city_state(city);
    g_have_wifi_creds = true;
    g_have_weather_key = g_weather_api_key[0] != '\0';
    (void)set_offline_mode_enabled(false);
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
    nvs_handle_t nvs;
    esp_err_t err = open_wifi_nvs(NVS_READWRITE, &nvs, kNvsActionSavingWeatherCity);
    if (err != ESP_OK) {
        return false;
    }
    char old_city[kManualWeatherCityLen] = {};
    size_t old_city_len = sizeof(old_city);
    if (nvs_get_str(nvs, kManualWeatherCityKey, old_city, &old_city_len) == ESP_OK &&
        strcmp(old_city, next) == 0) {
        nvs_close(nvs);
        set_manual_weather_city_state(next);
        return true;
    }
    err = nvs_set_str(nvs, kManualWeatherCityKey, next);
    err = commit_nvs_if_ok(nvs, err);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_SAVE_WEATHER_CITY_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    set_manual_weather_city_state(next);
    return true;
}

bool clear_manual_weather_city()
{
    nvs_handle_t nvs;
    esp_err_t err = open_wifi_nvs(NVS_READWRITE, &nvs, kNvsActionClearingWeatherCity);
    if (err != ESP_OK) {
        return false;
    }
    bool erased = false;
    err = erase_nvs_key_if_present(nvs, kManualWeatherCityKey, &erased);
    if (err == ESP_OK && erased) {
        err = commit_nvs_if_ok(nvs, err);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_CLEAR_WEATHER_CITY_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    set_manual_weather_city_state("");
    return true;
}

bool save_hourly_chime_setting()
{
    nvs_handle_t nvs;
    esp_err_t err = open_wifi_nvs(NVS_READWRITE, &nvs, kNvsActionSavingHourlyReminder);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t next_chime = g_hourly_chime_enabled ? 1 : 0;
    uint8_t next_all_day = g_hourly_chime_all_day ? 1 : 0;
    uint8_t next_volume = (uint8_t)g_chime_volume_percent;
    uint8_t next_sound = (uint8_t)g_chime_sound_index;
    uint8_t old_chime = 0;
    uint8_t old_all_day = 0;
    uint8_t old_volume = 0;
    uint8_t old_sound = 0;
    bool unchanged = nvs_get_u8(nvs, kHourlyChimeKey, &old_chime) == ESP_OK &&
                     nvs_get_u8(nvs, kHourlyAllDayKey, &old_all_day) == ESP_OK &&
                     nvs_get_u8(nvs, kChimeVolumeKey, &old_volume) == ESP_OK &&
                     nvs_get_u8(nvs, kChimeSoundKey, &old_sound) == ESP_OK &&
                     old_chime == next_chime &&
                     old_all_day == next_all_day &&
                     old_volume == next_volume &&
                     old_sound == next_sound;
    if (unchanged) {
        nvs_close(nvs);
        return true;
    }
    err = set_nvs_u8_if_ok(nvs, err, kHourlyChimeKey, next_chime);
    err = set_nvs_u8_if_ok(nvs, err, kHourlyAllDayKey, next_all_day);
    err = set_nvs_u8_if_ok(nvs, err, kChimeVolumeKey, next_volume);
    err = set_nvs_u8_if_ok(nvs, err, kChimeSoundKey, next_sound);
    err = commit_nvs_if_ok(nvs, err);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_SAVE_HOURLY_REMINDER_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool save_work_page_settings()
{
    nvs_handle_t nvs;
    esp_err_t err = open_wifi_nvs(NVS_READWRITE, &nvs, kNvsActionSavingPageSettings);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t mask = (g_work_page_enabled_mask | kWeatherClockPageMask) & kPageMaskV3KnownBits;
    if (nvs_u8_matches(nvs, kPageMaskV3Key, mask)) {
        nvs_close(nvs);
        g_work_page_enabled_mask = mask;
        return true;
    }
    err = nvs_set_u8(nvs, kPageMaskV3Key, mask);
    err = commit_nvs_if_ok(nvs, err);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_SAVE_PAGE_SETTINGS_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    g_work_page_enabled_mask = mask;
    return true;
}

bool save_work_page_order()
{
    normalize_work_page_order();
    nvs_handle_t nvs;
    esp_err_t err = open_wifi_nvs(NVS_READWRITE, &nvs, kNvsActionSavingPageOrder);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t old_order[kWorkPageCount] = {};
    size_t old_len = sizeof(old_order);
    if (nvs_get_blob(nvs, kPageOrderV3Key, old_order, &old_len) == ESP_OK &&
        old_len == sizeof(old_order) &&
        memcmp(old_order, g_work_page_order, sizeof(g_work_page_order)) == 0) {
        nvs_close(nvs);
        return true;
    }
    err = nvs_set_blob(nvs, kPageOrderV3Key, g_work_page_order, sizeof(g_work_page_order));
    err = commit_nvs_if_ok(nvs, err);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, NVS_SAVE_PAGE_ORDER_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool clear_saved_config()
{
    nvs_handle_t nvs;
    esp_err_t open_err = open_wifi_nvs(NVS_READWRITE, &nvs, kNvsActionClearingConfig);
    if (open_err == ESP_OK) {
        bool erased = false;
        esp_err_t err = ESP_OK;
        for (const char *key : kClearConfigKeys) {
            bool key_erased = false;
            err = erase_nvs_key_if_present(nvs, key, &key_erased);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, NVS_ERASE_KEY_CLEARING_CONFIG_FAILED_FORMAT, key, esp_err_to_name(err));
                break;
            }
            erased = erased || key_erased;
        }
        if (err == ESP_OK && erased) {
            err = commit_nvs_if_ok(nvs, err);
        }
        nvs_close(nvs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, NVS_CLEAR_CONFIG_FAILED_FORMAT, esp_err_to_name(err));
            return false;
        }
    } else {
        return false;
    }
    g_wifi_ssid[0] = '\0';
    g_wifi_pass[0] = '\0';
    g_weather_api_key[0] = '\0';
    set_manual_weather_city_state("");
    g_sta_ip[0] = '\0';
    g_have_wifi_creds = false;
    g_have_weather_key = false;
    g_offline_mode_ui_enabled = false;
    clear_app_event_bits(kWifiConnectedBit | kWeatherReadyBit, kConfigEventReasonFactoryReset);
    clear_network_request_bits();
    return true;
}

void url_decode(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; ++si) {
        char decoded = '\0';
        if (decode_url_percent_byte(&src[si], &decoded)) {
            dst[di++] = decoded;
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

void form_value(const char *body, const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!body || !key || key[0] == '\0') {
        out[0] = '\0';
        return;
    }
    const char *start = nullptr;
    size_t len = 0;
    if (!find_form_value_range(body, key, &start, &len)) {
        out[0] = '\0';
        return;
    }
    char encoded[kFormEncodedBufferSize] = {};
    if (len >= sizeof(encoded)) {
        ESP_LOGW(TAG, FORM_VALUE_TRUNCATED_LOG_FORMAT,
                 key,
                 (unsigned)len,
                 (unsigned)sizeof(encoded));
        len = sizeof(encoded) - 1;
    }
    memcpy(encoded, start, len);
    encoded[len] = '\0';
    url_decode(out, out_len, encoded);
}

void form_value_fallback(const char *body, const char *primary_key, const char *fallback_key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    form_value(body, primary_key, out, out_len);
    if (out[0] == '\0' && fallback_key) {
        form_value(body, fallback_key, out, out_len);
    }
}

static bool parse_manual_datetime(const char *text, struct tm *out)
{
    if (!text || !out || text[0] == '\0') {
        return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int parsed = sscanf(text, kManualTimeIsoSecondsFormat, &year, &month, &day, &hour, &minute, &second);
    if (parsed < kManualTimeRequiredFieldCount) {
        second = 0;
        parsed = sscanf(text, kManualTimeSpaceSecondsFormat, &year, &month, &day, &hour, &minute, &second);
        if (parsed < kManualTimeRequiredFieldCount) {
            second = 0;
            parsed = sscanf(text, kManualTimeSpaceMinutesFormat, &year, &month, &day, &hour, &minute);
        }
    }
    if (parsed < kManualTimeRequiredFieldCount ||
        !value_in_range(year, kMinValidYear, kMaxValidYear) ||
        !value_in_range(month, kManualTimeMinMonth, kManualTimeMaxMonth) ||
        !value_in_range(day, kManualTimeMinDay, kManualTimeMaxDay) ||
        !value_in_range(hour, kManualTimeMinHour, kManualTimeMaxHour) ||
        !value_in_range(minute, kManualTimeMinMinute, kManualTimeMaxMinute) ||
        !value_in_range(second, kManualTimeMinSecond, kManualTimeMaxSecond)) {
        return false;
    }
    struct tm local = {};
    local.tm_year = year - kTmYearOffset;
    local.tm_mon = month - kTmMonthOffset;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = second;
    local.tm_isdst = -1;
    time_t epoch = mktime(&local);
    if (epoch <= 0) {
        return false;
    }
    struct tm normalized = {};
    if (!localtime_r(&epoch, &normalized)) {
        ESP_LOGW(TAG, "manual offline time localtime normalization failed");
        return false;
    }
    if (normalized.tm_year != local.tm_year ||
        normalized.tm_mon != local.tm_mon ||
        normalized.tm_mday != local.tm_mday ||
        normalized.tm_hour != local.tm_hour ||
        normalized.tm_min != local.tm_min) {
        return false;
    }
    *out = normalized;
    return true;
}

bool save_offline_datetime_from_body(const char *body)
{
    if (!body) {
        ESP_LOGW(TAG, "offline setup ignored empty request body");
        return false;
    }
    char manual_time[kManualTimeFieldSize] = {};
    form_value_fallback(body, kFormManualTimeKey, kFormManualTimeFallbackKey, manual_time, sizeof(manual_time));
    trim_ascii(manual_time);
    struct tm local = {};
    if (!parse_manual_datetime(manual_time, &local)) {
        ESP_LOGW(TAG, "offline setup ignored invalid manual time");
        return false;
    }
    time_t epoch = mktime(&local);
    if (epoch <= 0) {
        ESP_LOGW(TAG, "set manual offline time skipped: mktime failed");
        return false;
    }
    struct timeval now = {};
    now.tv_sec = epoch;
    if (settimeofday(&now, nullptr) != 0) {
        ESP_LOGW(TAG, "set manual offline time failed errno=%d", errno);
        return false;
    }
    sync_rtc_from_system_time();
    if (!set_offline_mode_enabled(true)) {
        return false;
    }
    set_app_event_bits(kTimeSyncedBit, kConfigEventReasonOfflineManualTime);
    ESP_LOGI(TAG, "offline mode enabled with manual time: %04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + kTmYearOffset,
             local.tm_mon + kTmMonthOffset,
             local.tm_mday,
             local.tm_hour,
             local.tm_min,
             local.tm_sec);
    return true;
}

bool save_credentials_from_body(const char *body)
{
    if (!body) {
        ESP_LOGW(TAG, "provisioning ignored empty request body");
        return false;
    }
    char ssid[kSetupSsidFieldSize] = {};
    char pass[kSetupPasswordFieldSize] = {};
    char api_key[kSetupApiKeyFieldSize] = {};
    char weather_city[kSetupWeatherCityFieldSize] = {};
    form_value(body, kFormSsidKey, ssid, sizeof(ssid));
    form_value_fallback(body, kFormPasswordKey, kFormPasswordFallbackKey, pass, sizeof(pass));
    form_value_fallback(body, kFormApiKeyKey, kFormApiKeyFallbackKey, api_key, sizeof(api_key));
    form_value_fallback(body, kFormWeatherCityKey, kFormWeatherCityFallbackKey, weather_city, sizeof(weather_city));
    trim_ascii(api_key);
    trim_ascii(weather_city);
    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "provisioning ignored empty ssid");
        return false;
    }
    if (api_key[0] == '\0' && g_weather_api_key[0] != '\0') {
        strlcpy(api_key, g_weather_api_key, sizeof(api_key));
    }
    if (api_key[0] == '\0') {
        ESP_LOGW(TAG, "provisioning ignored empty api key for online setup");
        return false;
    }
    if (!is_weather_city_input_valid(weather_city)) {
        ESP_LOGW(TAG, "provisioning ignored invalid weather city");
        return false;
    }
    ESP_LOGI(TAG, "provisioning saved ssid=%s pass_len=%u api_key=%s len=%u weather_city=%s city_len=%u",
             ssid,
             (unsigned)strlen(pass),
             api_key[0] ? "set" : "empty",
             (unsigned)strlen(api_key),
             weather_city[0] ? "set" : "auto",
             (unsigned)strlen(weather_city));
    g_last_wifi_disconnect_reason = 0;
    clear_app_event_bits(kWifiConnectedBit, kConfigEventReasonProvisioningSave);
    if (!save_config(ssid, pass, api_key, weather_city)) {
        return false;
    }
    (void)apply_station_config(true);
    return true;
}
