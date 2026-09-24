// 对接 IP 定位、QWeather 城市查询、实时天气和天气预警接口。
#include "qweather_client.h"

#include "network_http_client.h"

#include "app_constexpr.h"
#include "app_metadata.h"
#include "app_text_format.h"
#include "network_credentials_state.h"
#include "qweather_api_host.h"
#include "qweather_alert_parser.h"
#include "qweather_alert_text.h"
#include "qweather_city_lookup_retry_policy.h"
#include "qweather_city_parser.h"
#include "qweather_current_parser.h"
#include "qweather_daily_fallback_policy.h"
#include "qweather_forecast_parser.h"
#include "qweather_response.h"
#include "qweather_url.h"
#include "weather_city_contract.h"

#include "esp_log.h"

#include <new>
#include <type_traits>

namespace {
constexpr size_t kQweatherCityResponseBufferSize = 8192;
constexpr size_t kQweatherNowResponseBufferSize = 8192;
constexpr size_t kQweatherAlertResponseBufferSize = 16384;
constexpr size_t kQweatherDailyResponseBufferSize = 24576;
constexpr size_t kQweatherAirResponseBufferSize = 8192;
static_assert(kQweatherCityResponseBufferSize > 1, "QWeather city response buffer must fit text and NUL");
static_assert(kQweatherNowResponseBufferSize > 1, "QWeather now response buffer must fit text and NUL");
static_assert(kQweatherAlertResponseBufferSize > kQweatherNowResponseBufferSize,
              "QWeather alert response buffer should remain larger than now response buffer");
static_assert(kQweatherDailyResponseBufferSize > kQweatherAlertResponseBufferSize,
              "QWeather daily response buffer should remain the largest weather response buffer");
static_assert(kQweatherAirResponseBufferSize > 1, "QWeather air response buffer must fit text and NUL");
static_assert(kQweatherEncodedLocationSize > kManualWeatherCityLen,
              "encoded weather location buffer must fit manual city text");
static_assert(kQweatherRequestUrlSize > kQweatherEncodedLocationSize,
              "general QWeather API URL buffer must fit encoded location text");
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
#define QWEATHER_CITY_LOOKUP_FORMAT "qweather city lookup: %s via configured API Host"
#define QWEATHER_CITY_RESOLVED_FORMAT "qweather city resolved: %s id=%s"
#define QWEATHER_CITY_LOOKUP_FAILED_FORMAT "qweather city lookup failed code=%s"
constexpr const char *kQweatherAlertInvalidArgLog = "qweather alert invalid arg";
constexpr const char *kQweatherAlertHttpFailedLog = "qweather alert http failed";
constexpr const char *kQweatherAlertTitleFormatFailedLog = "qweather alert title format failed";
#define QWEATHER_ALERT_LOOKUP_FORMAT "qweather alert lookup: %s,%s via configured API Host"
#define QWEATHER_ALERT_FAILED_FORMAT "qweather alert failed code=%s"
#define QWEATHER_ALERT_STAGING_ALLOC_FAILED_FORMAT \
    "qweather alert staging alloc failed size=%u"
constexpr const char *kQweatherNowInvalidArgLog = "qweather now invalid arg";
constexpr const char *kQweatherNowLocationTooLongLog = "qweather now location too long";
constexpr const char *kQweatherNowHttpFailedLog = "qweather now http failed";
#define QWEATHER_NOW_LOOKUP_FORMAT "qweather now lookup: %s via configured API Host"
#define QWEATHER_NOW_FAILED_FORMAT "qweather now failed code=%s"
constexpr const char *kQweatherDailyInvalidArgLog = "qweather daily invalid arg";
constexpr const char *kQweatherDailyLocationTooLongLog = "qweather daily location too long";
#define QWEATHER_DAILY_LOOKUP_FORMAT "qweather daily lookup: %s %dd via configured API Host"
#define QWEATHER_DAILY_HTTP_FAILED_FORMAT "qweather daily http failed err=%s"
#define QWEATHER_DAILY_FAILED_FORMAT "qweather daily failed code=%s"
#define QWEATHER_DAILY_STAGING_ALLOC_FAILED_FORMAT \
    "qweather daily staging alloc failed size=%u"
constexpr const char *kQweatherDailyShorterFallbackLog =
    "qweather 7d access restricted; trying 3d endpoint once";
constexpr const char *kQweatherAirInvalidArgLog = "qweather air invalid arg";
constexpr const char *kQweatherAirLocationTooLongLog = "qweather air location too long";
#define QWEATHER_AIR_LOOKUP_FORMAT "qweather air lookup: %s,%s via configured API Host"
#define QWEATHER_AIR_HTTP_FAILED_FORMAT "qweather air http failed err=%s"
#define QWEATHER_AIR_FAILED_FORMAT "qweather air failed code=%s"
void log_qweather_fixed_warning(const char *message);

static_assert(std::is_trivially_destructible<WeatherAlertData>::value,
              "QWeather alert heap staging requires trivial destruction");
static_assert(sizeof(WeatherAlertData) > 384,
              "QWeather alert staging should remain off the network task stack");
static_assert((kQweatherRequestUrlSize + kQweatherAlertResponseBufferSize) %
                      alignof(WeatherAlertData) ==
                  0,
              "QWeather alert staging must stay naturally aligned");
static_assert(std::is_trivially_destructible<WeatherForecastData>::value,
              "QWeather forecast heap staging requires trivial destruction");
static_assert(sizeof(WeatherForecastData) > 512,
              "QWeather forecast staging should remain off the network task stack");
static_assert((kQweatherRequestUrlSize + kQweatherDailyResponseBufferSize) %
                      alignof(WeatherForecastData) ==
                  0,
              "QWeather forecast staging must stay naturally aligned");

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

esp_err_t qweather_http_get_text(HttpTextSession *session,
                                 const char *url,
                                 char *response,
                                 size_t response_len)
{
    char api_key[kNetworkWeatherApiKeyLen] = {};
    if (!network_weather_api_key_snapshot(api_key, sizeof(api_key))) {
        return ESP_ERR_INVALID_STATE;
    }
    return session ? session->get(url, response, response_len, api_key)
                   : http_get_text(url, response, response_len, api_key);
}

bool load_qweather_api_host(char *host, size_t host_len)
{
    if (network_weather_api_host_snapshot(host, host_len)) {
        return true;
    }
    log_qweather_fixed_warning("qweather API Host unavailable");
    return false;
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
                                                      size_t lon_len,
                                                      HttpTextSession *session)
{
    if (!location ||
        !app_text::output_buffer_available(city_id, city_id_len) ||
        !app_text::output_buffer_available(city_name, city_name_len)) {
        log_qweather_fixed_warning(kQweatherCityInvalidArgLog);
        return kQweatherCityLookupError;
    }
    QweatherResponseBuffer response(
        kQweatherStageCity,
        kQweatherCityResponseBufferSize,
        kQweatherRequestUrlSize);
    if (!response) {
        return kQweatherCityLookupError;
    }
    char *url = response.request_url();
    char host[kQweatherApiHostLen] = {};
    if (!load_qweather_api_host(host, sizeof(host))) {
        return kQweatherCityLookupError;
    }
    QweatherUrlStatus url_status =
        build_qweather_city_lookup_url(
            url, response.request_url_size(), host, location);
    if (!qweather_url_ready(url_status,
                            kQweatherStageCity,
                            kQweatherCityLocationTooLongLog)) {
        return url_status == kQweatherUrlLocationTooLong
                   ? kQweatherCityLookupNotFound
                   : kQweatherCityLookupError;
    }
    ESP_LOGI(TAG, QWEATHER_CITY_LOOKUP_FORMAT, location);
    if (qweather_http_get_text(session, url, response.get(), response.size()) != ESP_OK) {
        log_qweather_fixed_warning(kQweatherCityHttpFailedLog);
        return kQweatherCityLookupError;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewCityLabel, response.get());
        return kQweatherCityLookupError;
    }
    bool ok = false;
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
    } else {
        ESP_LOGW(TAG, QWEATHER_CITY_LOOKUP_FAILED_FORMAT, qweather_code_text(code));
    }
    return qweather_city_lookup_response_status(ok,
                                                cJSON_IsArray(locations),
                                                cJSON_IsObject(first),
                                                qweather_code_text(code));
}

bool qweather_fetch_alert(const char *lat,
                          const char *lon,
                          WeatherAlertData *alert,
                          HttpTextSession *session)
{
    if (!alert) {
        log_qweather_fixed_warning(kQweatherAlertInvalidArgLog);
        return false;
    }
    if (!lat || !lon || lat[0] == '\0' || lon[0] == '\0') {
        *alert = WeatherAlertData{};
        return true;
    }

    char host[kQweatherApiHostLen] = {};
    if (!load_qweather_api_host(host, sizeof(host))) {
        return false;
    }
    QweatherResponseBuffer response(
        kQweatherStageAlert,
        kQweatherAlertResponseBufferSize,
        kQweatherRequestUrlSize,
        sizeof(WeatherAlertData));
    if (!response) {
        return false;
    }
    char *url = response.request_url();
    if (!qweather_url_ready(build_qweather_alert_url(
                                url,
                                response.request_url_size(),
                                host,
                                lat,
                                lon),
                            kQweatherStageAlert)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_ALERT_LOOKUP_FORMAT, lat, lon);
    if (qweather_http_get_text(session, url, response.get(), response.size()) != ESP_OK) {
        log_qweather_fixed_warning(kQweatherAlertHttpFailedLog);
        return false;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewAlertLabel, response.get());
        return false;
    }
    const cJSON *code = nullptr;
    const cJSON *alerts =
        qweather_alert_success_array(root.get(), kQweatherAlertJsonAlertsField, &code);
    if (!alerts) {
        ESP_LOGW(TAG, QWEATHER_ALERT_FAILED_FORMAT, qweather_code_text(code));
        return false;
    }

    void *alert_storage = response.staging();
    if (!alert_storage ||
        response.staging_size() < sizeof(WeatherAlertData)) {
        ESP_LOGW(TAG,
                 QWEATHER_ALERT_STAGING_ALLOC_FAILED_FORMAT,
                 static_cast<unsigned>(sizeof(WeatherAlertData)));
        return false;
    }
    WeatherAlertData *next =
        new (alert_storage) WeatherAlertData{};
    int alert_count = cJSON_GetArraySize(alerts);
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
        add_weather_alert_title(next, parsed.title, parsed.rank);
    }
    next->active = next->count > 0;
    time(&next->updated_at);
    *alert = *next;

    return true;
}

bool qweather_fetch_now(const char *city_id,
                        WeatherData *weather,
                        HttpTextSession *session)
{
    if (!city_id || !weather) {
        log_qweather_fixed_warning(kQweatherNowInvalidArgLog);
        return false;
    }
    char host[kQweatherApiHostLen] = {};
    if (!load_qweather_api_host(host, sizeof(host))) {
        return false;
    }
    QweatherResponseBuffer response(
        kQweatherStageNow,
        kQweatherNowResponseBufferSize,
        kQweatherRequestUrlSize);
    if (!response) {
        return false;
    }
    char *url = response.request_url();
    if (!qweather_url_ready(build_qweather_now_url(
                                url,
                                response.request_url_size(),
                                host,
                                city_id),
                            kQweatherStageNow,
                            kQweatherNowLocationTooLongLog)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_NOW_LOOKUP_FORMAT, city_id);
    if (qweather_http_get_text(session, url, response.get(), response.size()) != ESP_OK) {
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

static QweatherDailyAttemptStatus qweather_fetch_daily_days(
    const char *city_id,
    int days,
    WeatherForecastData *forecast,
    HttpTextSession *session)
{
    if (!city_id || !forecast ||
        (days != kQweatherDaily3DayEndpointDays && days != kQweatherDaily7DayEndpointDays)) {
        log_qweather_fixed_warning(kQweatherDailyInvalidArgLog);
        return QweatherDailyAttemptStatus::kFailed;
    }
    char host[kQweatherApiHostLen] = {};
    if (!load_qweather_api_host(host, sizeof(host))) {
        return QweatherDailyAttemptStatus::kFailed;
    }
    QweatherResponseBuffer response(
        kQweatherStageDaily,
        kQweatherDailyResponseBufferSize,
        kQweatherRequestUrlSize,
        sizeof(WeatherForecastData));
    if (!response) {
        return QweatherDailyAttemptStatus::kFailed;
    }
    char *url = response.request_url();
    if (!qweather_url_ready(build_qweather_daily_url(
                                url,
                                response.request_url_size(),
                                host,
                                city_id,
                                days),
                            kQweatherStageDaily,
                            kQweatherDailyLocationTooLongLog)) {
        return QweatherDailyAttemptStatus::kFailed;
    }
    ESP_LOGI(TAG, QWEATHER_DAILY_LOOKUP_FORMAT, city_id, days);
    esp_err_t http_err = qweather_http_get_text(session,
                                                url,
                                                response.get(),
                                                response.size());
    if (http_err != ESP_OK) {
        ESP_LOGW(TAG, QWEATHER_DAILY_HTTP_FAILED_FORMAT, esp_err_to_name(http_err));
        return QweatherDailyAttemptStatus::kFailed;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewDailyLabel, response.get());
        return QweatherDailyAttemptStatus::kFailed;
    }

    void *forecast_storage = response.staging();
    if (!forecast_storage ||
        response.staging_size() < sizeof(WeatherForecastData)) {
        ESP_LOGW(TAG,
                 QWEATHER_DAILY_STAGING_ALLOC_FAILED_FORMAT,
                 static_cast<unsigned>(sizeof(WeatherForecastData)));
        return QweatherDailyAttemptStatus::kFailed;
    }
    WeatherForecastData *next =
        new (forecast_storage) WeatherForecastData{};
    bool ok = false;
    const cJSON *code = nullptr;
    const cJSON *daily = qweather_success_array(root.get(), kQweatherDailyJsonDailyField, &code);
    if (daily) {
        if (parse_qweather_forecast_days(daily, next)) {
            *forecast = *next;
            ok = true;
        }
    } else {
        ESP_LOGW(TAG, QWEATHER_DAILY_FAILED_FORMAT, qweather_code_text(code));
    }
    return qweather_daily_attempt_status(ok, qweather_code_text(code));
}

bool qweather_fetch_daily(const char *city_id,
                          WeatherForecastData *forecast,
                          HttpTextSession *session)
{
    const QweatherDailyAttemptStatus preferred =
        qweather_fetch_daily_days(city_id,
                                  kQweatherDaily7DayEndpointDays,
                                  forecast,
                                  session);
    if (preferred == QweatherDailyAttemptStatus::kSuccess) {
        return true;
    }
    if (!qweather_daily_should_try_shorter(preferred)) {
        return false;
    }
    ESP_LOGI(TAG, "%s", kQweatherDailyShorterFallbackLog);
    return qweather_fetch_daily_days(city_id,
                                     kQweatherDaily3DayEndpointDays,
                                     forecast,
                                     session) ==
           QweatherDailyAttemptStatus::kSuccess;
}

bool qweather_fetch_air(const char *lat,
                        const char *lon,
                        WeatherAirData *air,
                        HttpTextSession *session)
{
    if (!lat || !lon || lat[0] == '\0' || lon[0] == '\0' || !air) {
        log_qweather_fixed_warning(kQweatherAirInvalidArgLog);
        return false;
    }
    char host[kQweatherApiHostLen] = {};
    if (!load_qweather_api_host(host, sizeof(host))) {
        return false;
    }
    QweatherResponseBuffer response(
        kQweatherStageAir,
        kQweatherAirResponseBufferSize,
        kQweatherRequestUrlSize);
    if (!response) {
        return false;
    }
    char *url = response.request_url();
    if (!qweather_url_ready(build_qweather_air_url(
                                url,
                                response.request_url_size(),
                                host,
                                lat,
                                lon),
                            kQweatherStageAir,
                            kQweatherAirLocationTooLongLog)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_AIR_LOOKUP_FORMAT, lat, lon);
    esp_err_t http_err = qweather_http_get_text(session,
                                                url,
                                                response.get(),
                                                response.size());
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
    ok = parse_qweather_current_air(root.get(), &next);
    next.ready = ok;
    if (ok) {
        time(&next.updated_at);
        *air = next;
    } else {
        ESP_LOGW(TAG, QWEATHER_AIR_FAILED_FORMAT, "invalid-response");
    }
    return ok;
}
