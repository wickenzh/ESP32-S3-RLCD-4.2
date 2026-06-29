// 负责 Wi-Fi、API Key、页面设置和声音设置的 NVS 配置读写。
#include "network_services.h"

#include "sensor_services.h"
#include "ui_views.h"

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
static_assert(kWorkPageCount <= 8, "work page enabled mask is stored as uint8_t");
static_assert(kLegacyPageOrderV1Count <= kWorkPageCount);
static_assert(kPageOrderV2Count <= kWorkPageCount);

void clear_app_event_bits(EventBits_t bits, const char *reason)
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "skip clear event bits for %s: event group unavailable", reason ? reason : "config");
        return;
    }
    xEventGroupClearBits(g_app_events, bits);
}

void set_app_event_bits(EventBits_t bits, const char *reason)
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "skip set event bits for %s: event group unavailable", reason ? reason : "config");
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
    if (nvs_open(kWifiNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t ssid_len = sizeof(g_wifi_ssid);
    size_t pass_len = sizeof(g_wifi_pass);
    size_t key_len = sizeof(g_weather_api_key);
    size_t city_len = sizeof(g_manual_weather_city);
    esp_err_t ssid_err = nvs_get_str(nvs, kWifiSsidKey, g_wifi_ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(nvs, kWifiPassKey, g_wifi_pass, &pass_len);
    esp_err_t key_err = nvs_get_str(nvs, kWeatherApiKeyKey, g_weather_api_key, &key_len);
    esp_err_t city_err = nvs_get_str(nvs, kManualWeatherCityKey, g_manual_weather_city, &city_len);
    uint8_t chime = 0;
    uint8_t all_day = 0;
    uint8_t volume = kDefaultChimeVolumePercent;
    uint8_t sound = 0;
    uint8_t page_mask = kPageMaskV3KnownBits;
    uint8_t offline = 0;
    uint8_t page_order[kWorkPageCount] = {};
    size_t page_order_len = sizeof(page_order);
    (void)nvs_get_u8(nvs, kHourlyChimeKey, &chime);
    (void)nvs_get_u8(nvs, kHourlyAllDayKey, &all_day);
    (void)nvs_get_u8(nvs, kChimeVolumeKey, &volume);
    (void)nvs_get_u8(nvs, kChimeSoundKey, &sound);
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
    (void)nvs_get_u8(nvs, kOfflineModeKey, &offline);
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
                         "network request reset");
}

bool set_offline_mode_enabled(bool enabled)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed while saving offline mode: %s", esp_err_to_name(err));
        return false;
    }
    uint8_t old_value = 0xff;
    uint8_t next_value = enabled ? 1 : 0;
    bool unchanged = nvs_get_u8(nvs, kOfflineModeKey, &old_value) == ESP_OK && old_value == next_value;
    if (!unchanged) {
        err = nvs_set_u8(nvs, kOfflineModeKey, next_value);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs save offline mode failed: %s", esp_err_to_name(err));
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
        unsigned char ch = (unsigned char)city[i];
        if (ch < 0x20 ||
            ch == '&' || ch == '=' || ch == '?' || ch == '#' || ch == '%' ||
            ch == '/' || ch == '\\' || ch == '<' || ch == '>' || ch == '"' || ch == '\'') {
            return false;
        }
    }
    return true;
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
    if (weather_city) {
        strlcpy(city, weather_city, sizeof(city));
        trim_ascii(city);
    }
    if (!is_weather_city_input_valid(city)) {
        ESP_LOGW(TAG, "skip saving invalid weather city");
        return false;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed while saving config: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_str(nvs, kWifiSsidKey, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, kWifiPassKey, pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, kWeatherApiKeyKey, api_key);
    }
    if (err == ESP_OK) {
        if (city[0] != '\0') {
            err = nvs_set_str(nvs, kManualWeatherCityKey, city);
        } else {
            esp_err_t erase_err = nvs_erase_key(nvs, kManualWeatherCityKey);
            if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
                err = erase_err;
            }
        }
    }
    esp_err_t legacy_erase_err = erase_nvs_key_if_present(nvs, kLegacyApiHostKey, nullptr);
    if (legacy_erase_err != ESP_OK) {
        ESP_LOGW(TAG, "nvs erase legacy api host failed while saving config: %s",
                 esp_err_to_name(legacy_erase_err));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs save config failed: %s", esp_err_to_name(err));
        return false;
    }
    strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));
    strlcpy(g_wifi_pass, pass, sizeof(g_wifi_pass));
    strlcpy(g_weather_api_key, api_key, sizeof(g_weather_api_key));
    strlcpy(g_manual_weather_city, city, sizeof(g_manual_weather_city));
    g_have_wifi_creds = true;
    g_have_weather_key = g_weather_api_key[0] != '\0';
    g_has_manual_weather_city = g_manual_weather_city[0] != '\0';
    (void)set_offline_mode_enabled(false);
    return true;
}

bool save_manual_weather_city(const char *city)
{
    char next[kManualWeatherCityLen] = {};
    if (city) {
        strlcpy(next, city, sizeof(next));
        trim_ascii(next);
    }
    if (next[0] == '\0') {
        return clear_manual_weather_city();
    }
    if (!is_weather_city_input_valid(next)) {
        ESP_LOGW(TAG, "skip saving invalid weather city");
        return false;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed while saving weather city: %s", esp_err_to_name(err));
        return false;
    }
    char old_city[kManualWeatherCityLen] = {};
    size_t old_city_len = sizeof(old_city);
    if (nvs_get_str(nvs, kManualWeatherCityKey, old_city, &old_city_len) == ESP_OK &&
        strcmp(old_city, next) == 0) {
        nvs_close(nvs);
        strlcpy(g_manual_weather_city, next, sizeof(g_manual_weather_city));
        g_has_manual_weather_city = true;
        return true;
    }
    err = nvs_set_str(nvs, kManualWeatherCityKey, next);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs save weather city failed: %s", esp_err_to_name(err));
        return false;
    }
    strlcpy(g_manual_weather_city, next, sizeof(g_manual_weather_city));
    g_has_manual_weather_city = true;
    return true;
}

bool clear_manual_weather_city()
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed while clearing weather city: %s", esp_err_to_name(err));
        return false;
    }
    bool erased = false;
    err = erase_nvs_key_if_present(nvs, kManualWeatherCityKey, &erased);
    if (err == ESP_OK && erased) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs clear weather city failed: %s", esp_err_to_name(err));
        return false;
    }
    g_manual_weather_city[0] = '\0';
    g_has_manual_weather_city = false;
    return true;
}

bool save_hourly_chime_setting()
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed while saving hourly reminder: %s", esp_err_to_name(err));
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
    err = nvs_set_u8(nvs, kHourlyChimeKey, next_chime);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, kHourlyAllDayKey, next_all_day);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, kChimeVolumeKey, next_volume);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, kChimeSoundKey, next_sound);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs save hourly reminder failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool save_work_page_settings()
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed while saving page settings: %s", esp_err_to_name(err));
        return false;
    }
    uint8_t mask = (g_work_page_enabled_mask | kWeatherClockPageMask) & kPageMaskV3KnownBits;
    uint8_t old_mask = 0;
    if (nvs_get_u8(nvs, kPageMaskV3Key, &old_mask) == ESP_OK && old_mask == mask) {
        nvs_close(nvs);
        g_work_page_enabled_mask = mask;
        return true;
    }
    err = nvs_set_u8(nvs, kPageMaskV3Key, mask);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs save page settings failed: %s", esp_err_to_name(err));
        return false;
    }
    g_work_page_enabled_mask = mask;
    return true;
}

bool save_work_page_order()
{
    normalize_work_page_order();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed while saving page order: %s", esp_err_to_name(err));
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
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs save page order failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool clear_saved_config()
{
    nvs_handle_t nvs;
    esp_err_t open_err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs);
    if (open_err == ESP_OK) {
        bool erased = false;
        esp_err_t err = ESP_OK;
        const char *keys[] = {
            kWifiSsidKey,
            kWifiPassKey,
            kWeatherApiKeyKey,
            kManualWeatherCityKey,
            kLegacyApiHostKey,
            kOfflineModeKey,
        };
        for (const char *key : keys) {
            bool key_erased = false;
            err = erase_nvs_key_if_present(nvs, key, &key_erased);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "nvs erase key %s failed while clearing config: %s", key, esp_err_to_name(err));
                break;
            }
            erased = erased || key_erased;
        }
        if (err == ESP_OK && erased) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs clear config failed: %s", esp_err_to_name(err));
            return false;
        }
    } else {
        ESP_LOGW(TAG, "nvs open failed while clearing config: %s", esp_err_to_name(open_err));
        return false;
    }
    g_wifi_ssid[0] = '\0';
    g_wifi_pass[0] = '\0';
    g_weather_api_key[0] = '\0';
    g_manual_weather_city[0] = '\0';
    g_sta_ip[0] = '\0';
    g_have_wifi_creds = false;
    g_have_weather_key = false;
    g_has_manual_weather_city = false;
    g_offline_mode_ui_enabled = false;
    clear_app_event_bits(kWifiConnectedBit | kWeatherReadyBit, "factory reset");
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
        ESP_LOGW(TAG, "form value truncated for key=%s len=%u cap=%u",
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
    int parsed = sscanf(text, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    if (parsed < 5) {
        second = 0;
        parsed = sscanf(text, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
        if (parsed < 5) {
            second = 0;
            parsed = sscanf(text, "%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute);
        }
    }
    if (parsed < 5 ||
        year < kMinValidYear || year > kMaxValidYear ||
        month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return false;
    }
    struct tm local = {};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
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
    localtime_r(&epoch, &normalized);
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
    char manual_time[32] = {};
    form_value_fallback(body, "manual_time", "datetime", manual_time, sizeof(manual_time));
    trim_ascii(manual_time);
    struct tm local = {};
    if (!parse_manual_datetime(manual_time, &local)) {
        ESP_LOGW(TAG, "offline setup ignored invalid manual time");
        return false;
    }
    time_t epoch = mktime(&local);
    struct timeval now = {};
    now.tv_sec = epoch;
    if (settimeofday(&now, nullptr) != 0) {
        ESP_LOGW(TAG, "set manual offline time failed");
        return false;
    }
    sync_rtc_from_system_time();
    if (!set_offline_mode_enabled(true)) {
        return false;
    }
    set_app_event_bits(kTimeSyncedBit, "offline manual time");
    ESP_LOGI(TAG, "offline mode enabled with manual time: %04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900,
             local.tm_mon + 1,
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
    char ssid[33] = {};
    char pass[65] = {};
    char api_key[96] = {};
    char weather_city[kManualWeatherCityLen] = {};
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value_fallback(body, "pass", "password", pass, sizeof(pass));
    form_value_fallback(body, "api_key", "weather", api_key, sizeof(api_key));
    form_value_fallback(body, "weather_city", "city", weather_city, sizeof(weather_city));
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
    clear_app_event_bits(kWifiConnectedBit, "provisioning save");
    if (!save_config(ssid, pass, api_key, weather_city)) {
        return false;
    }
    (void)apply_station_config(true);
    return true;
}
