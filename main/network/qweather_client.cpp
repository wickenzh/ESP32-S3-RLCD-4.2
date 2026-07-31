// 对接 IP 定位、QWeather 城市查询、实时天气和天气预警接口。
#include "network_services.h"
#include "app_constexpr.h"
#include "app_text_format.h"
#include "network_credentials_state.h"
#include "qweather_alert_parser.h"
#include "qweather_alert_text.h"
#include "qweather_city_parser.h"
#include "qweather_current_parser.h"
#include "qweather_forecast_parser.h"
#include "qweather_response.h"
#include "qweather_url.h"

namespace {
constexpr size_t kQweatherCityResponseBufferSize = 8192;
constexpr size_t kQweatherNowResponseBufferSize = 8192;
constexpr size_t kQweatherAlertResponseBufferSize = 16384;
constexpr size_t kQweatherDailyResponseBufferSize = 24576;
constexpr size_t kQweatherAirResponseBufferSize = 8192;
constexpr size_t kQweatherApiUrlSize = 512;
constexpr size_t kQweatherAlertUrlSize = 256;
static_assert(kQweatherCityResponseBufferSize > 1, "QWeather city response buffer must fit text and NUL");
static_assert(kQweatherNowResponseBufferSize > 1, "QWeather now response buffer must fit text and NUL");
static_assert(kQweatherAlertResponseBufferSize > kQweatherNowResponseBufferSize,
              "QWeather alert response buffer should remain larger than now response buffer");
static_assert(kQweatherDailyResponseBufferSize > kQweatherAlertResponseBufferSize,
              "QWeather daily response buffer should remain the largest weather response buffer");
static_assert(kQweatherAirResponseBufferSize > 1, "QWeather air response buffer must fit text and NUL");
static_assert(kQweatherEncodedLocationSize > kManualWeatherCityLen,
              "encoded weather location buffer must fit manual city text");
static_assert(kQweatherApiUrlSize > kQweatherEncodedLocationSize,
              "general QWeather API URL buffer must fit encoded location text");
static_assert(kQweatherApiUrlSize > kQweatherAlertUrlSize,
              "general QWeather API URL buffer must stay larger than alert URL buffer");
constexpr int kQweatherDaily3DayEndpointDays = 3;
constexpr int kQweatherDaily7DayEndpointDays = 7;
static_assert(kQweatherDaily7DayEndpointDays > kQweatherDaily3DayEndpointDays,
              "QWeather 7-day endpoint must cover more days than 3-day endpoint");
static_assert(kWeatherForecastDays <= kQweatherDaily7DayEndpointDays,
              "stored forecast day count must fit the preferred QWeather endpoint");
constexpr const char *kQweatherStageCity = "city";
constexpr const char *kQweatherStageAlert = "alert";
constexpr const char *kQweatherStageNow = "now";
constexpr const char *kQweatherStageDaily = "daily";
constexpr const char *kQweatherStageAir = "air";
constexpr const char *kQweatherPreviewCityLabel = "qweather city";
constexpr const char *kQweatherPreviewAlertLabel = "qweather alert";
constexpr const char *kQweatherPreviewNowLabel = "qweather now";
constexpr const char *kQweatherPreviewDailyLabel = "qweather daily";
constexpr const char *kQweatherPreviewAirLabel = "qweather air";
constexpr const char *kQweatherUnknownStage = "unknown";
constexpr const char *kQweatherJsonLocationField = "location";
constexpr const char *kQweatherJsonNowField = "now";
constexpr const char *kQweatherAlertJsonAlertsField = "alerts";
constexpr const char *kQweatherDailyJsonDailyField = "daily";
#define QWEATHER_URL_INVALID_ARG_FORMAT "qweather url invalid arg stage=%s"
#define QWEATHER_URL_TOO_LONG_FORMAT "qweather %s url too long"
constexpr const char *kQweatherCityInvalidArgLog = "qweather city invalid arg";
constexpr const char *kQweatherCityLocationTooLongLog = "qweather city location too long";
constexpr const char *kQweatherCityHttpFailedLog = "qweather city lookup http failed";
#define QWEATHER_CITY_LOOKUP_FORMAT "qweather city lookup: %s via %s"
#define QWEATHER_CITY_RESOLVED_FORMAT "qweather city resolved: %s id=%s"
#define QWEATHER_CITY_LOOKUP_FAILED_FORMAT "qweather city lookup failed code=%s"
constexpr const char *kQweatherAlertInvalidArgLog = "qweather alert invalid arg";
constexpr const char *kQweatherAlertHttpFailedLog = "qweather alert http failed";
constexpr const char *kQweatherAlertTitleFormatFailedLog = "qweather alert title format failed";
#define QWEATHER_ALERT_LOOKUP_FORMAT "qweather alert lookup: %s,%s via %s"
constexpr const char *kQweatherNowInvalidArgLog = "qweather now invalid arg";
constexpr const char *kQweatherNowLocationTooLongLog = "qweather now location too long";
constexpr const char *kQweatherNowHttpFailedLog = "qweather now http failed";
#define QWEATHER_NOW_LOOKUP_FORMAT "qweather now lookup: %s via %s"
#define QWEATHER_NOW_FAILED_FORMAT "qweather now failed code=%s"
constexpr const char *kQweatherDailyInvalidArgLog = "qweather daily invalid arg";
constexpr const char *kQweatherDailyLocationTooLongLog = "qweather daily location too long";
#define QWEATHER_DAILY_LOOKUP_FORMAT "qweather daily lookup: %s %dd via %s"
#define QWEATHER_DAILY_HTTP_FAILED_FORMAT "qweather daily http failed err=%s"
#define QWEATHER_DAILY_FAILED_FORMAT "qweather daily failed code=%s"
constexpr const char *kQweatherAirInvalidArgLog = "qweather air invalid arg";
constexpr const char *kQweatherAirLocationTooLongLog = "qweather air location too long";
#define QWEATHER_AIR_LOOKUP_FORMAT "qweather air lookup: %s via %s"
#define QWEATHER_AIR_HTTP_FAILED_FORMAT "qweather air http failed err=%s"
#define QWEATHER_AIR_FAILED_FORMAT "qweather air failed code=%s"
void log_qweather_fixed_warning(const char *message);

bool qweather_url_ready(QweatherUrlStatus status,
                        const char *stage,
                        const char *location_warning = nullptr)
{
    if (status == kQweatherUrlOk) {
        return true;
    }
    if (status == kQweatherUrlLocationTooLong) {
        log_qweather_fixed_warning(location_warning);
    } else if (status == kQweatherUrlInvalidArgument) {
        ESP_LOGW(TAG, QWEATHER_URL_INVALID_ARG_FORMAT, qweather_stage_text(stage));
    } else {
        ESP_LOGW(TAG, QWEATHER_URL_TOO_LONG_FORMAT, qweather_stage_text(stage));
    }
    return false;
}

void log_qweather_fixed_warning(const char *message)
{
    ESP_LOGW(TAG, "%s", cstr_nonempty(message) ? message : kQweatherUnknownStage);
}

esp_err_t qweather_http_get_text(const char *url, char *response, size_t response_len)
{
    char api_key[kNetworkWeatherApiKeyLen] = {};
    if (!network_weather_api_key_snapshot(api_key, sizeof(api_key))) {
        return ESP_ERR_INVALID_STATE;
    }
    return http_get_text(url, response, response_len, api_key);
}

} // namespace

QweatherCityLookupStatus qweather_lookup_city_status(const char *location,
                                                      char *city_id,
                                                      size_t city_id_len,
                                                      char *city_name,
                                                      size_t city_name_len,
                                                      char *lat_out,
                                                      size_t lat_len,
                                                      char *lon_out,
                                                      size_t lon_len)
{
    if (!location ||
        !app_text::output_buffer_available(city_id, city_id_len) ||
        !app_text::output_buffer_available(city_name, city_name_len)) {
        log_qweather_fixed_warning(kQweatherCityInvalidArgLog);
        return kQweatherCityLookupError;
    }
    char url[kQweatherApiUrlSize] = {};
    QweatherUrlStatus url_status = build_qweather_city_lookup_url(url, sizeof(url), location);
    if (!qweather_url_ready(url_status,
                            kQweatherStageCity,
                            kQweatherCityLocationTooLongLog)) {
        return url_status == kQweatherUrlLocationTooLong
                   ? kQweatherCityLookupNotFound
                   : kQweatherCityLookupError;
    }
    ESP_LOGI(TAG, QWEATHER_CITY_LOOKUP_FORMAT, location, qweather_geo_api_host());
    QweatherResponseBuffer response(kQweatherStageCity, kQweatherCityResponseBufferSize);
    if (!response) {
        return kQweatherCityLookupError;
    }
    if (qweather_http_get_text(url, response.get(), response.size()) != ESP_OK) {
        log_qweather_fixed_warning(kQweatherCityHttpFailedLog);
        return kQweatherCityLookupError;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewCityLabel, response.get());
        return kQweatherCityLookupError;
    }
    bool ok = false;
    QweatherCityLookupStatus status = kQweatherCityLookupNotFound;
    const cJSON *code = nullptr;
    const cJSON *locations = qweather_success_array(root.get(), kQweatherJsonLocationField, &code);
    const cJSON *first = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : nullptr;
    if (cJSON_IsObject(first)) {
        ok = parse_qweather_city_location(first,
                                          city_id,
                                          city_id_len,
                                          city_name,
                                          city_name_len,
                                          lat_out,
                                          lat_len,
                                          lon_out,
                                          lon_len);
        if (ok) {
            ESP_LOGI(TAG, QWEATHER_CITY_RESOLVED_FORMAT, city_name, city_id);
        }
        status = ok ? kQweatherCityLookupOk : kQweatherCityLookupError;
    } else {
        ESP_LOGW(TAG, QWEATHER_CITY_LOOKUP_FAILED_FORMAT, qweather_code_text(code));
    }
    return status;
}

bool qweather_lookup_city(const char *location,
                          char *city_id,
                          size_t city_id_len,
                          char *city_name,
                          size_t city_name_len,
                          char *lat_out,
                          size_t lat_len,
                          char *lon_out,
                          size_t lon_len)
{
    return qweather_lookup_city_status(location,
                                       city_id,
                                       city_id_len,
                                       city_name,
                                       city_name_len,
                                       lat_out,
                                       lat_len,
                                       lon_out,
                                       lon_len) == kQweatherCityLookupOk;
}

bool qweather_fetch_alert(const char *lat, const char *lon, WeatherAlertData *alert)
{
    if (!alert) {
        log_qweather_fixed_warning(kQweatherAlertInvalidArgLog);
        return false;
    }
    if (!lat || !lon || lat[0] == '\0' || lon[0] == '\0') {
        alert->active = false;
        return true;
    }

    const char *host = qweather_api_host();
    char url[kQweatherAlertUrlSize] = {};
    if (!qweather_url_ready(build_qweather_alert_url(url, sizeof(url), lat, lon),
                            kQweatherStageAlert)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_ALERT_LOOKUP_FORMAT, lat, lon, host);
    QweatherResponseBuffer response(kQweatherStageAlert, kQweatherAlertResponseBufferSize);
    if (!response) {
        return false;
    }
    if (qweather_http_get_text(url, response.get(), response.size()) != ESP_OK) {
        log_qweather_fixed_warning(kQweatherAlertHttpFailedLog);
        return false;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewAlertLabel, response.get());
        return false;
    }

    WeatherAlertData next = {};
    const cJSON *alerts = cJSON_GetObjectItem(root.get(), kQweatherAlertJsonAlertsField);
    int alert_count = cJSON_IsArray(alerts) ? cJSON_GetArraySize(alerts) : 0;
    for (int i = 0; i < alert_count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(alerts, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }
        QweatherAlertItem parsed = {};
        if (!parse_qweather_alert_item(item, &parsed)) {
            continue;
        }
        if (!parsed.title_format_ok) {
            log_qweather_fixed_warning(kQweatherAlertTitleFormatFailedLog);
        }
        add_weather_alert_title(&next, parsed.title, parsed.rank);
    }
    next.active = next.count > 0;
    time(&next.updated_at);
    *alert = next;

    return true;
}

bool qweather_fetch_now(const char *city_id, WeatherData *weather)
{
    if (!city_id || !weather) {
        log_qweather_fixed_warning(kQweatherNowInvalidArgLog);
        return false;
    }
    const char *host = qweather_api_host();
    char url[kQweatherApiUrlSize] = {};
    if (!qweather_url_ready(build_qweather_now_url(url, sizeof(url), city_id),
                            kQweatherStageNow,
                            kQweatherNowLocationTooLongLog)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_NOW_LOOKUP_FORMAT, city_id, host);
    QweatherResponseBuffer response(kQweatherStageNow, kQweatherNowResponseBufferSize);
    if (!response) {
        return false;
    }
    if (qweather_http_get_text(url, response.get(), response.size()) != ESP_OK) {
        log_qweather_fixed_warning(kQweatherNowHttpFailedLog);
        return false;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewNowLabel, response.get());
        return false;
    }
    bool ok = false;
    const cJSON *code = nullptr;
    const cJSON *now = qweather_success_object(root.get(), kQweatherJsonNowField, &code);
    if (now) {
        ok = parse_qweather_current_weather(now, weather);
    } else {
        ESP_LOGW(TAG, QWEATHER_NOW_FAILED_FORMAT, qweather_code_text(code));
    }
    return ok;
}

static bool qweather_fetch_daily_days(const char *city_id, int days, WeatherForecastData *forecast)
{
    if (!city_id || !forecast ||
        (days != kQweatherDaily3DayEndpointDays && days != kQweatherDaily7DayEndpointDays)) {
        log_qweather_fixed_warning(kQweatherDailyInvalidArgLog);
        return false;
    }
    const char *host = qweather_api_host();
    char url[kQweatherApiUrlSize] = {};
    if (!qweather_url_ready(build_qweather_daily_url(url, sizeof(url), city_id, days),
                            kQweatherStageDaily,
                            kQweatherDailyLocationTooLongLog)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_DAILY_LOOKUP_FORMAT, city_id, days, host);
    QweatherResponseBuffer response(kQweatherStageDaily, kQweatherDailyResponseBufferSize);
    if (!response) {
        return false;
    }
    esp_err_t http_err = qweather_http_get_text(url, response.get(), response.size());
    if (http_err != ESP_OK) {
        ESP_LOGW(TAG, QWEATHER_DAILY_HTTP_FAILED_FORMAT, esp_err_to_name(http_err));
        return false;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewDailyLabel, response.get());
        return false;
    }

    WeatherForecastData next = {};
    bool ok = false;
    const cJSON *code = nullptr;
    const cJSON *daily = qweather_success_array(root.get(), kQweatherDailyJsonDailyField, &code);
    if (daily) {
        if (parse_qweather_forecast_days(daily, &next)) {
            *forecast = next;
            ok = true;
        }
    } else {
        ESP_LOGW(TAG, QWEATHER_DAILY_FAILED_FORMAT, qweather_code_text(code));
    }
    return ok;
}

bool qweather_fetch_daily(const char *city_id, WeatherForecastData *forecast)
{
    if (qweather_fetch_daily_days(city_id, kQweatherDaily7DayEndpointDays, forecast)) {
        return true;
    }
    return qweather_fetch_daily_days(city_id, kQweatherDaily3DayEndpointDays, forecast);
}

bool qweather_fetch_air(const char *city_id, WeatherAirData *air)
{
    if (!city_id || !air) {
        log_qweather_fixed_warning(kQweatherAirInvalidArgLog);
        return false;
    }
    const char *host = qweather_api_host();
    char url[kQweatherApiUrlSize] = {};
    if (!qweather_url_ready(build_qweather_air_url(url, sizeof(url), city_id),
                            kQweatherStageAir,
                            kQweatherAirLocationTooLongLog)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_AIR_LOOKUP_FORMAT, city_id, host);
    QweatherResponseBuffer response(kQweatherStageAir, kQweatherAirResponseBufferSize);
    if (!response) {
        return false;
    }
    esp_err_t http_err = qweather_http_get_text(url, response.get(), response.size());
    if (http_err != ESP_OK) {
        ESP_LOGW(TAG, QWEATHER_AIR_HTTP_FAILED_FORMAT, esp_err_to_name(http_err));
        return false;
    }

    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewAirLabel, response.get());
        return false;
    }
    WeatherAirData next = {};
    bool ok = false;
    const cJSON *code = nullptr;
    const cJSON *now = qweather_success_object(root.get(), kQweatherJsonNowField, &code);
    if (now) {
        ok = parse_qweather_current_air(now, &next);
        next.ready = ok;
        if (ok) {
            time(&next.updated_at);
            *air = next;
        }
    } else {
        ESP_LOGW(TAG, QWEATHER_AIR_FAILED_FORMAT, qweather_code_text(code));
    }
    return ok;
}
