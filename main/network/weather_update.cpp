// 负责选择手动城市或 IP 定位，并组合提交完整天气更新结果。
#include "network_services.h"
#include "network_https_resources.h"
#include "network_credentials_state.h"

#include "app_constexpr.h"
#include "manual_weather_city_state.h"
#include "network_sync_schedule.h"
#include "qweather_location_text.h"
#include "startup_state.h"
#include "weather_state.h"

namespace {
constexpr size_t kQweatherCityIdSize = 24;
constexpr size_t kWeatherCityNameSize = 32;
static_assert(kQweatherCityIdSize > 1, "QWeather city id buffer must fit text and NUL");
static_assert(kWeatherCityNameSize <= kManualWeatherCityLen,
              "QWeather city name must fit manual weather city storage");

#define WEATHER_UPDATE_MANUAL_CITY_FORMAT "weather update using manual city: %s"
#define WEATHER_MANUAL_CITY_LOOKUP_FAILED_FORMAT "manual weather city lookup failed: %s"
#define WEATHER_MANUAL_CITY_UPDATE_FAILED_FORMAT "weather update failed for manual city: %s"
#define WEATHER_RETRY_IP_CITY_LOOKUP_FORMAT "retry qweather city lookup by ip city: %s"
#define WEATHER_USING_IP_COORDINATES_FORMAT "using ip coordinates for weather now: %s"
#define WEATHER_STARTUP_FOLLOWUP_DEFERRED_FORMAT \
    "startup weather %s deferred: internal_free=%u internal_largest=%u dma_largest=%u"
constexpr const char *kWeatherIpLookupUpdateFailedLog = "weather update failed after ip lookup";
constexpr const char *kWeatherIpGeolocationLookupFailedLog = "ip geolocation lookup failed";

void log_weather_update_warning(const char *message)
{
    ESP_LOGW(TAG, "%s", cstr_nonempty(message) ? message : "weather update failed");
}

bool prepare_weather_followup_request(const char *stage)
{
    // Each QWeather helper owns and releases its response/TLS buffers before
    // returning. Keep the longer settle and memory gate through the first
    // minute, including the staggered background refresh after the boot UI.
    bool startup_pressure = network_startup_pressure_window_active(
        startup_screen_active(),
        esp_timer_get_time());
    vTaskDelay(pdMS_TO_TICKS(
        network_weather_request_settle_delay_ms(startup_pressure)));
    if (!startup_pressure) {
        return true;
    }
    const NetworkHttpsMemorySnapshot memory = capture_network_https_memory_snapshot();
    if (network_startup_followup_https_allowed(startup_pressure,
                                               memory.internal_free,
                                               memory.internal_largest,
                                               memory.dma_largest)) {
        return true;
    }
    ESP_LOGW(TAG,
             WEATHER_STARTUP_FOLLOWUP_DEFERRED_FORMAT,
             cstr_nonempty(stage) ? stage : "follow-up",
             static_cast<unsigned>(memory.internal_free),
             static_cast<unsigned>(memory.internal_largest),
             static_cast<unsigned>(memory.dma_largest));
    return false;
}

bool lookup_weather_city(const char *location,
                         char *city_id,
                         char *city_name,
                         WeatherData *weather)
{
    if (!city_id || !city_name || !weather) {
        return false;
    }
    return qweather_lookup_city(location,
                                city_id,
                                kQweatherCityIdSize,
                                city_name,
                                kWeatherCityNameSize,
                                weather->lat,
                                sizeof(weather->lat),
                                weather->lon,
                                sizeof(weather->lon));
}

WeatherUpdateResult fetch_and_commit_weather(const char *city_id, WeatherData *next)
{
    if (!city_id || !next) {
        return WeatherUpdateResult::kFailed;
    }
    if (!prepare_weather_followup_request("current")) {
        return WeatherUpdateResult::kResourceDeferred;
    }
    if (!qweather_fetch_now(city_id, next)) {
        return WeatherUpdateResult::kFailed;
    }

    WeatherAlertData next_alert = {};
    WeatherForecastData next_forecast = {};
    WeatherAirData next_air = {};
    if (!prepare_weather_followup_request("alert")) {
        return WeatherUpdateResult::kResourceDeferred;
    }
    (void)qweather_fetch_alert(next->lat, next->lon, &next_alert);
    if (!prepare_weather_followup_request("forecast")) {
        return WeatherUpdateResult::kResourceDeferred;
    }
    bool forecast_ok = qweather_fetch_daily(city_id, &next_forecast);
    if (!prepare_weather_followup_request("air")) {
        return WeatherUpdateResult::kResourceDeferred;
    }
    bool air_ok = qweather_fetch_air(city_id, &next_air);
    commit_weather_update_snapshot(*next, next_alert, next_forecast, next_air, forecast_ok, air_ok);
    return WeatherUpdateResult::kSuccess;
}

WeatherUpdateResult update_weather_by_manual_city(const char *manual_city)
{
    char city_id[kQweatherCityIdSize] = {};
    char lookup_city[kWeatherCityNameSize] = {};
    WeatherData next = {};

    ESP_LOGI(TAG, WEATHER_UPDATE_MANUAL_CITY_FORMAT, manual_city);
    bool have_city_id = lookup_weather_city(manual_city, city_id, lookup_city, &next);
    if (!have_city_id) {
        ESP_LOGW(TAG, WEATHER_MANUAL_CITY_LOOKUP_FAILED_FORMAT, manual_city);
        return WeatherUpdateResult::kFailed;
    }
    copy_first_nonempty_text(next.city, sizeof(next.city), lookup_city, manual_city);
    WeatherUpdateResult result = fetch_and_commit_weather(city_id, &next);
    if (result == WeatherUpdateResult::kSuccess ||
        result == WeatherUpdateResult::kResourceDeferred) {
        return result;
    }
    ESP_LOGW(TAG, WEATHER_MANUAL_CITY_UPDATE_FAILED_FORMAT, manual_city);
    return WeatherUpdateResult::kFailed;
}

WeatherUpdateResult update_weather_by_ip_location()
{
    char location[kWeatherLocationTextSize] = {};
    char city_id[kQweatherCityIdSize] = {};
    char ip_city[kWeatherCityNameSize] = {};
    char lookup_city[kWeatherCityNameSize] = {};
    WeatherData next = {};

    if (!ip_geolocation_lookup(location, sizeof(location), ip_city, sizeof(ip_city))) {
        log_weather_update_warning(kWeatherIpGeolocationLookupFailedLog);
        return WeatherUpdateResult::kFailed;
    }
    trim_ascii(location);
    if (!prepare_weather_followup_request("city lookup")) {
        return WeatherUpdateResult::kResourceDeferred;
    }
    bool have_city_id = lookup_weather_city(location, city_id, lookup_city, &next);
    if (!have_city_id && ip_city[0] != '\0') {
        ESP_LOGW(TAG, WEATHER_RETRY_IP_CITY_LOOKUP_FORMAT, ip_city);
        if (!prepare_weather_followup_request("city lookup retry")) {
            return WeatherUpdateResult::kResourceDeferred;
        }
        have_city_id = lookup_weather_city(ip_city, city_id, lookup_city, &next);
    }
    copy_first_nonempty_text(next.city, sizeof(next.city), ip_city, lookup_city, location);
    if (!have_city_id) {
        copy_ip_coordinate_location(location, city_id, sizeof(city_id), &next);
        ESP_LOGW(TAG, WEATHER_USING_IP_COORDINATES_FORMAT, city_id);
    }
    WeatherUpdateResult result = fetch_and_commit_weather(city_id, &next);
    if (result == WeatherUpdateResult::kSuccess ||
        result == WeatherUpdateResult::kResourceDeferred) {
        return result;
    }
    log_weather_update_warning(kWeatherIpLookupUpdateFailedLog);
    return WeatherUpdateResult::kFailed;
}
} // namespace

WeatherUpdateResult perform_weather_update()
{
    if (!network_weather_api_key_configured() || battery_low_mode_load()) {
        clear_weather_ready_event();
        return WeatherUpdateResult::kFailed;
    }

    char manual_city[kManualWeatherCityLen] = {};
    if (manual_weather_city_snapshot(manual_city, sizeof(manual_city))) {
        trim_ascii(manual_city);
    }
    if (manual_city[0] != '\0') {
        return update_weather_by_manual_city(manual_city);
    }
    return update_weather_by_ip_location();
}
