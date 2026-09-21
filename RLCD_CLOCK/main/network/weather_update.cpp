// 负责选择手动城市或 IP 定位，并按请求作用域提交天气结果。
#include "weather_update.h"
#include "open_meteo_client.h"
#include "weather_provider.h"

#include "ip_geolocation_client.h"
#include "network_https_resources.h"
#include "network_https_resources_internal.h"
#include "network_credentials_state.h"
#include "qweather_client.h"

#include "app_constexpr.h"
#include "app_metadata.h"
#include "manual_weather_city_state.h"
#include "network_cache_policy.h"
#include "network_sync_runtime.h"
#include "network_sync_schedule.h"
#include "network_sync_wait.h"
#include "ascii_text.h"
#include "qweather_location_text.h"
#include "startup_state.h"
#include "weather_state_internal.h"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <string.h>

namespace {
constexpr size_t kQweatherCityIdSize = 24;
constexpr size_t kWeatherCityNameSize = 32;
constexpr size_t kWeatherCoordinateTextSize = sizeof(WeatherData{}.lat);
constexpr int64_t kWeatherIpRetryContextTtlUs = 2LL * 60LL * 1000000LL;
constexpr int64_t kWeatherCityResolutionCacheTtlUs =
    24LL * 60LL * 60LL * 1000000LL;
static_assert(kQweatherCityIdSize > 1, "QWeather city id buffer must fit text and NUL");
static_assert(kWeatherCityNameSize <= kManualWeatherCityLen,
              "QWeather city name must fit manual weather city storage");
static_assert(kWeatherCoordinateTextSize == sizeof(WeatherData{}.lon),
              "weather latitude and longitude buffers must remain aligned");
static_assert(kWeatherIpRetryContextTtlUs > 0,
              "weather IP retry context lifetime must be positive");
static_assert(kWeatherCityResolutionCacheTtlUs > kWeatherIpRetryContextTtlUs,
              "weather city resolution cache must outlive deferred retry context");

struct WeatherUpdateWorkspace {
    uint32_t configuration_generation;
    char manual_city[kManualWeatherCityLen];
    char location[kWeatherLocationTextSize];
    char city_id[kQweatherCityIdSize];
    char ip_city[kWeatherCityNameSize];
    char lookup_city[kWeatherCityNameSize];
    WeatherData weather;
    WeatherAlertData alert;
    WeatherForecastData forecast;
    WeatherAirData air;
};

struct WeatherIpRetryContext {
    bool valid = false;
    int64_t expires_at_us = 0;
    char location[kWeatherLocationTextSize] = {};
    char ip_city[kWeatherCityNameSize] = {};
};

struct WeatherCityResolutionCache {
    bool valid = false;
    int64_t expires_at_us = 0;
    char location[kWeatherLocationTextSize] = {};
    char city_id[kQweatherCityIdSize] = {};
    char city_name[kWeatherCityNameSize] = {};
    char lat[kWeatherCoordinateTextSize] = {};
    char lon[kWeatherCoordinateTextSize] = {};
};

// perform_weather_update() is owned by the serialized network task. Keeping
// this batch workspace in PSRAM avoids retaining the complete weather snapshot
// on that task's stack while later HTTPS requests allocate their TLS buffers.
EXT_RAM_BSS_ATTR WeatherUpdateWorkspace s_weather_update_workspace;
EXT_RAM_BSS_ATTR WeatherIpRetryContext s_weather_ip_retry_context;
EXT_RAM_BSS_ATTR WeatherCityResolutionCache s_weather_city_resolution_cache;

static_assert(sizeof(WeatherUpdateWorkspace) > 1024,
              "weather update workspace should remain off the network task stack");
static_assert(sizeof(WeatherCityResolutionCache) < 256,
              "weather city resolution cache must remain a small PSRAM record");

#define WEATHER_UPDATE_MANUAL_CITY_FORMAT "weather update using manual city: %s"
#define WEATHER_MANUAL_CITY_LOOKUP_FAILED_FORMAT "manual weather city lookup failed: %s"
#define WEATHER_MANUAL_CITY_UPDATE_FAILED_FORMAT "weather update failed for manual city: %s"
#define WEATHER_RETRY_IP_CITY_LOOKUP_FORMAT "retry qweather city lookup by ip city: %s"
#define WEATHER_USING_IP_COORDINATES_FORMAT "using ip coordinates for weather now: %s"
#define WEATHER_STARTUP_FOLLOWUP_DEFERRED_FORMAT \
    "startup weather %s deferred: internal_free=%u internal_largest=%u dma_largest=%u"
#define WEATHER_RUNTIME_CHANGE_DEFERRED_FORMAT \
    "weather update %s deferred after runtime state change"
constexpr const char *kWeatherIpLookupUpdateFailedLog = "weather update failed after ip lookup";
constexpr const char *kWeatherIpGeolocationLookupFailedLog = "ip geolocation lookup failed";
constexpr const char *kWeatherIpRetryContextReusedLog =
    "weather update reusing deferred IP location";
constexpr const char *kWeatherCityResolutionCacheReusedLog =
    "weather update reusing cached city resolution";

void clear_weather_ip_retry_context()
{
    s_weather_ip_retry_context = {};
}

void store_weather_ip_retry_context(const WeatherUpdateWorkspace &workspace)
{
    if (!cstr_nonempty(workspace.location)) {
        clear_weather_ip_retry_context();
        return;
    }
    WeatherIpRetryContext next = {};
    next.valid = true;
    next.expires_at_us = esp_timer_get_time() + kWeatherIpRetryContextTtlUs;
    strlcpy(next.location, workspace.location, sizeof(next.location));
    strlcpy(next.ip_city, workspace.ip_city, sizeof(next.ip_city));
    s_weather_ip_retry_context = next;
}

bool restore_weather_ip_retry_context(WeatherUpdateWorkspace *workspace)
{
    if (!workspace || !s_weather_ip_retry_context.valid) {
        return false;
    }
    if (esp_timer_get_time() >= s_weather_ip_retry_context.expires_at_us) {
        clear_weather_ip_retry_context();
        return false;
    }
    strlcpy(workspace->location,
            s_weather_ip_retry_context.location,
            sizeof(workspace->location));
    strlcpy(workspace->ip_city,
            s_weather_ip_retry_context.ip_city,
            sizeof(workspace->ip_city));
    ESP_LOGI(TAG, "%s", kWeatherIpRetryContextReusedLog);
    return true;
}

void clear_weather_city_resolution_cache()
{
    s_weather_city_resolution_cache = {};
}

void store_weather_city_resolution_cache(const WeatherUpdateWorkspace &workspace)
{
    if (!cstr_nonempty(workspace.location) ||
        !cstr_nonempty(workspace.city_id) ||
        !cstr_nonempty(workspace.weather.lat) ||
        !cstr_nonempty(workspace.weather.lon)) {
        clear_weather_city_resolution_cache();
        return;
    }
    WeatherCityResolutionCache next = {};
    next.valid = true;
    next.expires_at_us =
        esp_timer_get_time() + kWeatherCityResolutionCacheTtlUs;
    strlcpy(next.location, workspace.location, sizeof(next.location));
    strlcpy(next.city_id, workspace.city_id, sizeof(next.city_id));
    strlcpy(next.city_name, workspace.lookup_city, sizeof(next.city_name));
    strlcpy(next.lat, workspace.weather.lat, sizeof(next.lat));
    strlcpy(next.lon, workspace.weather.lon, sizeof(next.lon));
    s_weather_city_resolution_cache = next;
}

bool restore_weather_city_resolution_cache(WeatherUpdateWorkspace *workspace)
{
    if (!workspace ||
        !network_weather_city_resolution_cache_matches(
            s_weather_city_resolution_cache.valid,
            esp_timer_get_time(),
            s_weather_city_resolution_cache.expires_at_us,
            s_weather_city_resolution_cache.location,
            workspace->location,
            s_weather_city_resolution_cache.city_id)) {
        clear_weather_city_resolution_cache();
        return false;
    }
    strlcpy(workspace->city_id,
            s_weather_city_resolution_cache.city_id,
            sizeof(workspace->city_id));
    strlcpy(workspace->lookup_city,
            s_weather_city_resolution_cache.city_name,
            sizeof(workspace->lookup_city));
    strlcpy(workspace->weather.lat,
            s_weather_city_resolution_cache.lat,
            sizeof(workspace->weather.lat));
    strlcpy(workspace->weather.lon,
            s_weather_city_resolution_cache.lon,
            sizeof(workspace->weather.lon));
    ESP_LOGI(TAG, "%s", kWeatherCityResolutionCacheReusedLog);
    return true;
}

void log_weather_update_warning(const char *message)
{
    ESP_LOGW(TAG, "%s", cstr_nonempty(message) ? message : "weather update failed");
}

void log_weather_runtime_change_deferred(const char *stage)
{
    ESP_LOGI(TAG,
             WEATHER_RUNTIME_CHANGE_DEFERRED_FORMAT,
             cstr_nonempty(stage) ? stage : "follow-up");
}

bool weather_sync_can_continue(const char *stage)
{
    if (network_sync_continuation_allowed()) {
        return true;
    }
    log_weather_runtime_change_deferred(stage);
    return false;
}

bool prepare_weather_followup_request(const char *stage)
{
    if (!weather_sync_can_continue(stage)) {
        return false;
    }
    // Each QWeather helper owns and releases its response/TLS buffers before
    // returning. Keep the longer settle and memory gate through the first
    // minute, including the staggered background refresh after the boot UI.
    const bool startup_pressure = network_startup_pressure_window_active(
        startup_screen_active(),
        esp_timer_get_time());
    if (!wait_for_network_sync_settle(
            network_weather_request_settle_delay_ms(startup_pressure))) {
        log_weather_runtime_change_deferred(stage);
        return false;
    }
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

QweatherCityLookupStatus lookup_weather_city(const char *location,
                                             char *city_id,
                                             char *city_name,
                                             WeatherData *weather)
{
    if (!city_id || !city_name || !weather) {
        return kQweatherCityLookupError;
    }
    return qweather_lookup_city_status(location,
                                       city_id,
                                       kQweatherCityIdSize,
                                       city_name,
                                       kWeatherCityNameSize,
                                       weather->lat,
                                       sizeof(weather->lat),
                                       weather->lon,
                                       sizeof(weather->lon));
}

WeatherUpdateResult fetch_and_commit_weather(const char *city_id,
                                             WeatherUpdateWorkspace &workspace,
                                             WeatherUpdateScope scope)
{
    if (!city_id) {
        return WeatherUpdateResult::kFailed;
    }
    if (!prepare_weather_followup_request("current")) {
        return WeatherUpdateResult::kResourceDeferred;
    }
    if (!qweather_fetch_now(city_id, &workspace.weather)) {
        return WeatherUpdateResult::kFailed;
    }
    workspace.weather.configuration_generation = workspace.configuration_generation;

    bool alert_updated = false;
    bool forecast_ok = false;
    if (!prepare_weather_followup_request("alert")) {
        commit_weather_resource_deferred_snapshot(workspace.weather,
                                                  workspace.alert,
                                                  workspace.forecast,
                                                  alert_updated,
                                                  forecast_ok);
        return WeatherUpdateResult::kResourceDeferred;
    }
    alert_updated = qweather_fetch_alert(workspace.weather.lat,
                                         workspace.weather.lon,
                                         &workspace.alert);
    if (scope == WeatherUpdateScope::kCurrentAndAlerts) {
        commit_weather_basic_snapshot(workspace.weather,
                                      workspace.alert,
                                      alert_updated);
        return WeatherUpdateResult::kSuccess;
    }
    if (!prepare_weather_followup_request("forecast")) {
        commit_weather_resource_deferred_snapshot(workspace.weather,
                                                  workspace.alert,
                                                  workspace.forecast,
                                                  alert_updated,
                                                  forecast_ok);
        return WeatherUpdateResult::kResourceDeferred;
    }
    forecast_ok = qweather_fetch_daily(city_id, &workspace.forecast);
    if (!prepare_weather_followup_request("air")) {
        commit_weather_resource_deferred_snapshot(workspace.weather,
                                                  workspace.alert,
                                                  workspace.forecast,
                                                  alert_updated,
                                                  forecast_ok);
        return WeatherUpdateResult::kResourceDeferred;
    }
    bool air_ok = qweather_fetch_air(workspace.weather.lat,
                                     workspace.weather.lon,
                                     &workspace.air);
    commit_weather_update_snapshot(workspace.weather,
                                   workspace.alert,
                                   workspace.forecast,
                                   workspace.air,
                                   alert_updated,
                                   forecast_ok,
                                   air_ok);
    return WeatherUpdateResult::kSuccess;
}

WeatherUpdateResult update_weather_by_manual_city(const char *manual_city,
                                                  WeatherUpdateWorkspace &workspace,
                                                  WeatherUpdateScope scope)
{
    ESP_LOGI(TAG, WEATHER_UPDATE_MANUAL_CITY_FORMAT, manual_city);
    strlcpy(workspace.location, manual_city, sizeof(workspace.location));
    bool have_city_id = restore_weather_city_resolution_cache(&workspace);
    if (!have_city_id) {
        have_city_id =
            lookup_weather_city(manual_city,
                                workspace.city_id,
                                workspace.lookup_city,
                                &workspace.weather) ==
            kQweatherCityLookupOk;
        if (have_city_id) {
            store_weather_city_resolution_cache(workspace);
        }
    }
    if (!have_city_id) {
        ESP_LOGW(TAG, WEATHER_MANUAL_CITY_LOOKUP_FAILED_FORMAT, manual_city);
        return WeatherUpdateResult::kFailed;
    }
    copy_first_nonempty_text(workspace.weather.city,
                             sizeof(workspace.weather.city),
                             workspace.lookup_city,
                             manual_city);
    WeatherUpdateResult result =
        fetch_and_commit_weather(workspace.city_id, workspace, scope);
    if (result == WeatherUpdateResult::kSuccess ||
        result == WeatherUpdateResult::kResourceDeferred) {
        return result;
    }
    ESP_LOGW(TAG, WEATHER_MANUAL_CITY_UPDATE_FAILED_FORMAT, manual_city);
    return WeatherUpdateResult::kFailed;
}

WeatherUpdateResult update_weather_by_ip_location(WeatherUpdateWorkspace &workspace,
                                                  WeatherUpdateScope scope)
{
    if (!restore_weather_ip_retry_context(&workspace)) {
        if (!ip_geolocation_lookup(workspace.location,
                                   sizeof(workspace.location),
                                   workspace.ip_city,
                                   sizeof(workspace.ip_city))) {
            clear_weather_ip_retry_context();
            log_weather_update_warning(kWeatherIpGeolocationLookupFailedLog);
            return WeatherUpdateResult::kFailed;
        }
        trim_ascii_whitespace(workspace.location);
        store_weather_ip_retry_context(workspace);
    }
    bool have_city_id = restore_weather_city_resolution_cache(&workspace);
    if (!have_city_id) {
        if (!prepare_weather_followup_request("city lookup")) {
            return WeatherUpdateResult::kResourceDeferred;
        }
        QweatherCityLookupStatus city_status =
            lookup_weather_city(workspace.location,
                                workspace.city_id,
                                workspace.lookup_city,
                                &workspace.weather);
        have_city_id = city_status == kQweatherCityLookupOk;
        if (qweather_city_lookup_should_try_alternate(city_status) &&
            workspace.ip_city[0] != '\0') {
            ESP_LOGW(TAG, WEATHER_RETRY_IP_CITY_LOOKUP_FORMAT, workspace.ip_city);
            if (!prepare_weather_followup_request("city lookup retry")) {
                return WeatherUpdateResult::kResourceDeferred;
            }
            city_status = lookup_weather_city(workspace.ip_city,
                                              workspace.city_id,
                                              workspace.lookup_city,
                                              &workspace.weather);
            have_city_id = city_status == kQweatherCityLookupOk;
        }
        if (have_city_id) {
            store_weather_city_resolution_cache(workspace);
        }
    }
    copy_first_nonempty_text(workspace.weather.city,
                             sizeof(workspace.weather.city),
                             workspace.ip_city,
                             workspace.lookup_city,
                             workspace.location);
    if (!have_city_id) {
        copy_ip_coordinate_location(workspace.location,
                                    workspace.city_id,
                                    sizeof(workspace.city_id),
                                    &workspace.weather);
        ESP_LOGW(TAG, WEATHER_USING_IP_COORDINATES_FORMAT, workspace.city_id);
    }
    WeatherUpdateResult result =
        fetch_and_commit_weather(workspace.city_id, workspace, scope);
    if (result == WeatherUpdateResult::kResourceDeferred) {
        return result;
    }
    clear_weather_ip_retry_context();
    if (result == WeatherUpdateResult::kSuccess) {
        return result;
    }
    log_weather_update_warning(kWeatherIpLookupUpdateFailedLog);
    return WeatherUpdateResult::kFailed;
}
} // namespace

WeatherUpdateResult perform_weather_update(WeatherUpdateScope scope)
{
    if (weather_provider_load() == WeatherProvider::kOpenMeteo) {
        return perform_open_meteo_update(scope);
    }
    if (!network_weather_configuration_configured()) {
        clear_weather_ip_retry_context();
        clear_weather_city_resolution_cache();
        clear_weather_ready_event();
        return WeatherUpdateResult::kFailed;
    }
    if (!network_sync_continuation_allowed()) {
        log_weather_runtime_change_deferred("start");
        return WeatherUpdateResult::kFailed;
    }

    WeatherUpdateWorkspace &workspace = s_weather_update_workspace;
    memset(&workspace, 0, sizeof(workspace));
    workspace.configuration_generation = weather_provider_generation();
    if (manual_weather_city_snapshot(workspace.manual_city,
                                     sizeof(workspace.manual_city))) {
        trim_ascii_whitespace(workspace.manual_city);
    }
    if (workspace.manual_city[0] != '\0') {
        clear_weather_ip_retry_context();
        return update_weather_by_manual_city(workspace.manual_city,
                                             workspace,
                                             scope);
    }
    return update_weather_by_ip_location(workspace, scope);
}
