// 负责 Wi-Fi、API Key、页面设置和声音设置的 NVS 配置读写。
#include "network_services.h"

#include "audio_services.h"
#include "sensor_services.h"
#include "ui_views.h"

namespace {
constexpr const char *kWifiNvsNamespace = "wifi";
constexpr const char *kWifiSsidKey = "ssid";
constexpr const char *kWifiPassKey = "pass";
constexpr const char *kWeatherApiKeyKey = "api_key";
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
    esp_err_t ssid_err = nvs_get_str(nvs, kWifiSsidKey, g_wifi_ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(nvs, kWifiPassKey, g_wifi_pass, &pass_len);
    esp_err_t key_err = nvs_get_str(nvs, kWeatherApiKeyKey, g_weather_api_key, &key_len);
    uint8_t chime = 0;
    uint8_t all_day = 0;
    uint8_t volume = 80;
    uint8_t sound = 0;
    uint8_t page_mask = 0x3F;
    uint8_t offline = 0;
    uint8_t page_order[kWorkPageCount] = {};
    size_t page_order_len = sizeof(page_order);
    (void)nvs_get_u8(nvs, kHourlyChimeKey, &chime);
    (void)nvs_get_u8(nvs, kHourlyAllDayKey, &all_day);
    (void)nvs_get_u8(nvs, kChimeVolumeKey, &volume);
    (void)nvs_get_u8(nvs, kChimeSoundKey, &sound);
    if (nvs_get_u8(nvs, kPageMaskV3Key, &page_mask) != ESP_OK) {
        uint8_t v2_page_mask = 0x1F;
        if (nvs_get_u8(nvs, kPageMaskV2Key, &v2_page_mask) == ESP_OK) {
            page_mask = (v2_page_mask & 0x1F) | 0x20;
        } else {
            uint8_t legacy_page_mask = 0x0F;
            if (nvs_get_u8(nvs, kPageMaskV1Key, &legacy_page_mask) == ESP_OK) {
                page_mask = (legacy_page_mask & 0x0F) | 0x10 | 0x20;
            }
        }
    }
    (void)nvs_get_u8(nvs, kOfflineModeKey, &offline);
    esp_err_t order_err = nvs_get_blob(nvs, kPageOrderV3Key, page_order, &page_order_len);
    if (order_err != ESP_OK || page_order_len != sizeof(page_order)) {
        uint8_t v2_order[5] = {};
        size_t v2_len = sizeof(v2_order);
        if (nvs_get_blob(nvs, kPageOrderV2Key, v2_order, &v2_len) == ESP_OK &&
            v2_len == sizeof(v2_order)) {
            int out = 0;
            for (int i = 0; i < 5 && out < kWorkPageCount; ++i) {
                page_order[out++] = v2_order[i];
                if (v2_order[i] == 0 && out < kWorkPageCount) {
                    page_order[out++] = 5;
                }
            }
            while (out < kWorkPageCount) {
                page_order[out++] = 5;
            }
            page_order_len = sizeof(page_order);
            order_err = ESP_OK;
        } else {
            uint8_t legacy_order[4] = {};
            size_t legacy_len = sizeof(legacy_order);
            if (nvs_get_blob(nvs, kPageOrderV1Key, legacy_order, &legacy_len) == ESP_OK &&
                legacy_len == sizeof(legacy_order)) {
                int out = 0;
                for (int i = 0; i < 4 && out < kWorkPageCount; ++i) {
                    page_order[out++] = legacy_order[i];
                    if (legacy_order[i] == 0 && out < kWorkPageCount) {
                        page_order[out++] = 5;
                    }
                }
                if (out < kWorkPageCount) {
                    page_order[out++] = 4;
                }
                if (out < kWorkPageCount) {
                    page_order[out++] = 5;
                }
                page_order_len = sizeof(page_order);
                order_err = ESP_OK;
            }
        }
    }
    nvs_close(nvs);
    g_have_weather_key = key_err == ESP_OK && g_weather_api_key[0] != '\0';
    g_hourly_chime_enabled = chime != 0;
    g_hourly_chime_all_day = all_day != 0;
    g_offline_mode_ui_enabled = offline != 0;
    if (volume != 20 && volume != 40 && volume != 60 && volume != 80 && volume != 100) {
        volume = 80;
    }
    g_chime_volume_percent = volume;
    g_chime_sound_index = sound < kChimeSoundCount ? sound : 0;
    g_work_page_enabled_mask = (page_mask | 0x01) & ((1U << kWorkPageCount) - 1);
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

bool save_config(const char *ssid, const char *pass, const char *api_key)
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
    (void)nvs_erase_key(nvs, kLegacyApiHostKey);
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
    g_have_wifi_creds = true;
    g_have_weather_key = g_weather_api_key[0] != '\0';
    (void)set_offline_mode_enabled(false);
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
    uint8_t mask = (g_work_page_enabled_mask | 0x01) & ((1U << kWorkPageCount) - 1);
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
    if (nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_erase_key(nvs, kWifiSsidKey);
        (void)nvs_erase_key(nvs, kWifiPassKey);
        (void)nvs_erase_key(nvs, kWeatherApiKeyKey);
        (void)nvs_erase_key(nvs, kLegacyApiHostKey);
        (void)nvs_erase_key(nvs, kOfflineModeKey);
        esp_err_t err = nvs_commit(nvs);
        nvs_close(nvs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs clear config failed: %s", esp_err_to_name(err));
            return false;
        }
    } else {
        ESP_LOGW(TAG, "nvs open failed while clearing config");
        return false;
    }
    g_wifi_ssid[0] = '\0';
    g_wifi_pass[0] = '\0';
    g_weather_api_key[0] = '\0';
    g_sta_ip[0] = '\0';
    g_have_wifi_creds = false;
    g_have_weather_key = false;
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
        if (src[si] == '%' &&
            src[si + 1] != '\0' &&
            src[si + 2] != '\0' &&
            isxdigit((unsigned char)src[si + 1]) &&
            isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = {src[si + 1], src[si + 2], 0};
            dst[di++] = (char)strtol(hex, nullptr, 16);
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
    char pattern[16];
    int pattern_len = snprintf(pattern, sizeof(pattern), "%s=", key);
    if (pattern_len < 0 || pattern_len >= (int)sizeof(pattern)) {
        out[0] = '\0';
        return;
    }
    const char *start = strstr(body, pattern);
    if (!start) {
        out[0] = '\0';
        return;
    }
    start += strlen(pattern);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    char encoded[160] = {};
    if (len >= sizeof(encoded)) {
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
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value_fallback(body, "pass", "password", pass, sizeof(pass));
    form_value_fallback(body, "api_key", "weather", api_key, sizeof(api_key));
    trim_ascii(api_key);
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
    ESP_LOGI(TAG, "provisioning saved ssid=%s pass_len=%u api_key=%s len=%u",
             ssid,
             (unsigned)strlen(pass),
             api_key[0] ? "set" : "empty",
             (unsigned)strlen(api_key));
    g_last_wifi_disconnect_reason = 0;
    clear_app_event_bits(kWifiConnectedBit, "provisioning save");
    if (!save_config(ssid, pass, api_key)) {
        return false;
    }
    (void)apply_station_config(true);
    return true;
}
