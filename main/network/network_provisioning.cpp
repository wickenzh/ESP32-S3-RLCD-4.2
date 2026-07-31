// 处理配网页联网凭据和离线日期时间提交，不拥有 NVS key 细节。
#include "network_services.h"

#include "alarm_services.h"
#include "network_config_internal.h"
#include "network_credentials_state.h"
#include "manual_time_parser.h"
#include "provisioning_form_fields.h"
#include "sensor_services.h"

#include <errno.h>

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
#define PROVISIONING_INVALID_WEATHER_CITY_LOG "provisioning ignored invalid weather city"
#define PROVISIONING_SAVED_FORMAT "provisioning saved ssid=%s pass_len=%u api_key=%s len=%u weather_city=%s city_len=%u"
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
    ProvisioningFormFields fields = {};
    read_provisioning_form_fields(body, &fields);
    if (fields.ssid[0] == '\0') {
        ESP_LOGW(TAG, "%s", PROVISIONING_EMPTY_SSID_LOG);
        return false;
    }
    if (fields.api_key[0] == '\0') {
        (void)network_weather_api_key_snapshot(fields.api_key, sizeof(fields.api_key));
    }
    if (fields.api_key[0] == '\0') {
        ESP_LOGW(TAG, "%s", PROVISIONING_EMPTY_API_KEY_LOG);
        return false;
    }
    if (!is_weather_city_input_valid(fields.weather_city)) {
        ESP_LOGW(TAG, "%s", PROVISIONING_INVALID_WEATHER_CITY_LOG);
        return false;
    }
    ESP_LOGI(TAG, PROVISIONING_SAVED_FORMAT,
             fields.ssid,
             (unsigned)strlen(fields.pass),
             fields.api_key[0] ? "set" : "empty",
             (unsigned)strlen(fields.api_key),
             fields.weather_city[0] ? "set" : "auto",
             (unsigned)strlen(fields.weather_city));
    clear_wifi_last_disconnect_reason();
    clear_config_event_bits(kWifiConnectedBit, kConfigEventReasonProvisioningSave);
    if (!save_config(fields.ssid, fields.pass, fields.api_key, fields.weather_city)) {
        return false;
    }
    (void)apply_station_config(true);
    return true;
}
