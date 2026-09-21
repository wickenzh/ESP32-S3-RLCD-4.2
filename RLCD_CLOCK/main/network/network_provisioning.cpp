// 处理配网页联网凭据和离线日期时间提交，不拥有 NVS key 细节。
#include "network_provisioning.h"

#include "app_event_group.h"
#include "app_metadata.h"
#include "alarm_services.h"
#include "housekeeping_schedule_notify.h"
#include "network_config.h"
#include "network_config_internal.h"
#include "network_credentials_state.h"
#include "network_credentials_state_internal.h"
#include "manual_time_parser.h"
#include "provisioning_form_fields.h"
#include "qweather_api_host.h"
#include "rtc_services.h"
#include "wifi_portal_state_internal.h"

#include <esp_attr.h>
#include <esp_log.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace {
constexpr const char *kConfigEventReasonOfflineManualTime = "offline manual time";
constexpr const char *kConfigEventReasonProvisioningSave = "provisioning save";
#define OFFLINE_SETUP_EMPTY_BODY_LOG "offline setup ignored empty request body"
#define OFFLINE_SETUP_INVALID_MANUAL_TIME_LOG "offline setup ignored invalid manual time"
#define MANUAL_TIME_MKTIME_FAILED_LOG "set manual offline time skipped: mktime failed"
#define MANUAL_TIME_SETTIMEOFDAY_FAILED_FORMAT "set manual offline time failed errno=%d"
#define OFFLINE_MODE_ENABLED_MANUAL_TIME_FORMAT "offline mode enabled with manual time: %04d-%02d-%02d %02d:%02d:%02d"
#define PROVISIONING_EMPTY_BODY_LOG "provisioning ignored empty request body"
#define PROVISIONING_EMPTY_SSID_LOG "provisioning ignored empty ssid"
#define PROVISIONING_EMPTY_API_KEY_LOG "provisioning ignored empty api key for online setup"
#define PROVISIONING_EMPTY_API_HOST_LOG "provisioning ignored empty API Host for online setup"
#define PROVISIONING_INVALID_API_HOST_LOG "provisioning ignored invalid QWeather API Host"
#define PROVISIONING_INVALID_WEATHER_CITY_LOG "provisioning ignored invalid weather city"
#define PROVISIONING_DUPLICATE_WIFI_LOG \
    "provisioning ignored duplicate main and backup Wi-Fi SSID"
#define PROVISIONING_SAVED_FORMAT \
    "provisioning saved main_ssid=%s main_pass_len=%u backup_ssid=%s backup_pass_len=%u api_key=%s len=%u api_host=%s len=%u weather_city=%s city_len=%u"

// Synchronous setup handlers share one HTTP server task, so only one save
// request can own this workspace at a time.
EXT_RAM_BSS_ATTR ProvisioningFormFields s_provisioning_form_fields_workspace;
static_assert(sizeof(s_provisioning_form_fields_workspace) >=
                  kProvisioningSsidFieldSize +
                      kProvisioningPasswordFieldSize +
                      kProvisioningSsidFieldSize +
                      kProvisioningPasswordFieldSize +
                      kProvisioningApiKeyFieldSize +
                      kProvisioningApiHostFieldSize +
                      kProvisioningWeatherCityFieldSize,
              "provisioning form workspace must contain every bounded field");

void clear_provisioning_form_fields_workspace()
{
    volatile uint8_t *bytes =
        reinterpret_cast<volatile uint8_t *>(
            &s_provisioning_form_fields_workspace);
    for (size_t remaining = sizeof(s_provisioning_form_fields_workspace);
         remaining > 0;
         --remaining) {
        *bytes++ = 0;
    }
}

class ProvisioningFormFieldsWorkspaceGuard {
public:
    ProvisioningFormFieldsWorkspaceGuard()
    {
        clear_provisioning_form_fields_workspace();
    }

    ~ProvisioningFormFieldsWorkspaceGuard()
    {
        clear_provisioning_form_fields_workspace();
    }

    ProvisioningFormFieldsWorkspaceGuard(
        const ProvisioningFormFieldsWorkspaceGuard &) = delete;
    ProvisioningFormFieldsWorkspaceGuard &operator=(
        const ProvisioningFormFieldsWorkspaceGuard &) = delete;

    ProvisioningFormFields &fields()
    {
        return s_provisioning_form_fields_workspace;
    }
};

void preserve_saved_wifi_password(WifiCredentialSlot slot,
                                  const char *submitted_ssid,
                                  char *submitted_password,
                                  size_t submitted_password_len)
{
    if (!submitted_ssid || submitted_ssid[0] == '\0' ||
        !submitted_password || submitted_password_len == 0 ||
        submitted_password[0] != '\0') {
        return;
    }
    char saved_ssid[kNetworkWifiSsidLen] = {};
    char saved_password[kNetworkWifiPasswordLen] = {};
    if (network_wifi_credentials_for_slot_copy(slot,
                                               saved_ssid,
                                               sizeof(saved_ssid),
                                               saved_password,
                                               sizeof(saved_password)) &&
        strcmp(saved_ssid, submitted_ssid) == 0) {
        strlcpy(submitted_password,
                saved_password,
                submitted_password_len);
    }
}
} // namespace

bool save_offline_datetime_from_body(const char *body)
{
    if (!body) {
        ESP_LOGW(TAG, "%s", OFFLINE_SETUP_EMPTY_BODY_LOG);
        return false;
    }
    char manual_time[kProvisioningManualTimeFieldSize] = {};
    read_provisioning_manual_time(body, manual_time, sizeof(manual_time));
    struct tm local = {};
    if (!parse_manual_datetime_text(manual_time, &local)) {
        ESP_LOGW(TAG, "%s", OFFLINE_SETUP_INVALID_MANUAL_TIME_LOG);
        return false;
    }
    time_t epoch = mktime(&local);
    if (epoch <= 0) {
        ESP_LOGW(TAG, "%s", MANUAL_TIME_MKTIME_FAILED_LOG);
        return false;
    }
    struct timeval now = {};
    now.tv_sec = epoch;
    if (settimeofday(&now, nullptr) != 0) {
        ESP_LOGW(TAG, MANUAL_TIME_SETTIMEOFDAY_FAILED_FORMAT, errno);
        return false;
    }
    sync_rtc_from_system_time();
    alarm_notify_time_changed();
    notify_housekeeping_schedule_changed();
    if (!set_offline_mode_enabled(true)) {
        return false;
    }
    set_config_event_bits(kTimeSyncedBit, kConfigEventReasonOfflineManualTime);
    ESP_LOGI(TAG, OFFLINE_MODE_ENABLED_MANUAL_TIME_FORMAT,
             local.tm_year + kManualTimeTmYearOffset,
             local.tm_mon + kManualTimeTmMonthOffset,
             local.tm_mday,
             local.tm_hour,
             local.tm_min,
             local.tm_sec);
    return true;
}

bool save_credentials_from_body(const char *body)
{
    if (!body) {
        ESP_LOGW(TAG, "%s", PROVISIONING_EMPTY_BODY_LOG);
        return false;
    }
    ProvisioningFormFieldsWorkspaceGuard workspace;
    ProvisioningFormFields &fields = workspace.fields();
    read_provisioning_form_fields(body, &fields);
    WeatherProvider provider=weather_provider_load();
    if(fields.weather_provider[0] && !parse_weather_provider(fields.weather_provider,&provider))return false;
    if (fields.ssid[0] == '\0') {
        ESP_LOGW(TAG, "%s", PROVISIONING_EMPTY_SSID_LOG);
        return false;
    }
    const WifiCredentialSlot preferred_slot = network_wifi_preferred_slot();
    preserve_saved_wifi_password(preferred_slot,
                                 fields.ssid,
                                 fields.pass,
                                 sizeof(fields.pass));
    preserve_saved_wifi_password(
        wifi_alternate_credential_slot(preferred_slot),
        fields.backup_ssid,
        fields.backup_pass,
        sizeof(fields.backup_pass));
    if (fields.backup_ssid[0] != '\0' &&
        strcmp(fields.ssid, fields.backup_ssid) == 0) {
        ESP_LOGW(TAG, "%s", PROVISIONING_DUPLICATE_WIFI_LOG);
        return false;
    }
    if (provider == WeatherProvider::kOpenMeteo || fields.api_key[0] == '\0') {
        (void)network_weather_api_key_snapshot(fields.api_key, sizeof(fields.api_key));
    }
    if (provider == WeatherProvider::kQweather && fields.api_key[0] == '\0') {
        ESP_LOGW(TAG, "%s", PROVISIONING_EMPTY_API_KEY_LOG);
        return false;
    }
    if (provider == WeatherProvider::kOpenMeteo || fields.api_host[0] == '\0') {
        (void)network_weather_api_host_snapshot(fields.api_host,
                                                sizeof(fields.api_host));
    }
    if (provider == WeatherProvider::kQweather && fields.api_host[0] == '\0') {
        ESP_LOGW(TAG, "%s", PROVISIONING_EMPTY_API_HOST_LOG);
        return false;
    }
    if (provider == WeatherProvider::kQweather && !normalize_qweather_api_host(fields.api_host,
                                     fields.api_host,
                                     sizeof(fields.api_host))) {
        ESP_LOGW(TAG, "%s", PROVISIONING_INVALID_API_HOST_LOG);
        return false;
    }
    if (!is_weather_city_input_valid(fields.weather_city)) {
        ESP_LOGW(TAG, "%s", PROVISIONING_INVALID_WEATHER_CITY_LOG);
        return false;
    }
    ESP_LOGI(TAG, PROVISIONING_SAVED_FORMAT,
             fields.ssid,
             (unsigned)strlen(fields.pass),
             fields.backup_ssid[0] ? fields.backup_ssid : "none",
             (unsigned)strlen(fields.backup_pass),
             fields.api_key[0] ? "set" : "empty",
             (unsigned)strlen(fields.api_key),
             fields.api_host[0] ? "set" : "empty",
             (unsigned)strlen(fields.api_host),
             fields.weather_city[0] ? "set" : "auto",
             (unsigned)strlen(fields.weather_city));
    clear_wifi_last_disconnect_reason();
    clear_config_event_bits(kWifiConnectedBit, kConfigEventReasonProvisioningSave);
    if (!save_config(fields.ssid,
                     fields.pass,
                     fields.backup_ssid,
                     fields.backup_pass,
                     fields.api_key,
                     fields.api_host,
                     fields.weather_city,
                     provider)) {
        return false;
    }
    return true;
}
