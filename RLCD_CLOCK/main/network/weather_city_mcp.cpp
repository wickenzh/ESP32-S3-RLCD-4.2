// 通过小智 MCP 校验并保存手动天气城市，复用现有 QWeather 和 NVS 配置路径。
#include "weather_city_mcp_internal.h"

#include "app_event_group.h"
#include "network_config.h"
#include "network_credentials_state.h"
#include "network_sync_request_generation.h"
#include "offline_mode_state.h"
#include "qweather_client.h"
#include "weather_provider.h"
#include "open_meteo_client.h"
#include "ui_task_notify.h"
#include "weather_city_contract.h"
#include "weather_city_pending_state_internal.h"
#include "xiaozhi_mcp.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

namespace {
constexpr size_t kCityIdLen = 32;
constexpr size_t kCityCoordinateLen = 24;

bool handle_weather_city(const XiaozhiMcpWeatherCityRequest &request,
                         char *result,
                         size_t result_len)
{
    char normalized[kManualWeatherCityLen] = {};
    if (!normalize_weather_city_input(request.city, normalized, sizeof(normalized)) ||
        normalized[0] == '\0') {
        if (result && result_len > 0) {
            std::snprintf(result, result_len, "invalid weather city");
        }
        return false;
    }
    bool automatic = strcmp(normalized, "自动") == 0 ||
                     strcmp(normalized, "自动定位") == 0 ||
                     strcmp(normalized, "自动模式") == 0 ||
                     strcmp(normalized, "恢复自动") == 0 ||
                     strcmp(normalized, "IP定位") == 0 ||
                     strcmp(normalized, "IP自动定位") == 0 ||
                     strcasecmp(normalized, "auto") == 0 ||
                     strcasecmp(normalized, "automatic") == 0;
    if (automatic) {
        if (!weather_city_pending_store("")) {
            if (result && result_len > 0) {
                std::snprintf(result, result_len, "weather city state unavailable");
            }
            return false;
        }
        if (result && result_len > 0) {
            std::snprintf(result,
                          result_len,
                          "automatic IP weather location will be restored");
        }
        return true;
    }
    if (offline_mode_enabled_load() ||
        !network_weather_configuration_configured()) {
        if (result && result_len > 0) {
            std::snprintf(result, result_len, "weather service is not configured");
        }
        return false;
    }

    char city_id[kCityIdLen] = {};
    char resolved_city[kManualWeatherCityLen] = {};
    char latitude[kCityCoordinateLen] = {};
    char longitude[kCityCoordinateLen] = {};
    QweatherCityLookupStatus status;
    if(weather_provider_load()==WeatherProvider::kOpenMeteo) {
        WeatherData location;
        const auto result_status=open_meteo_lookup_city(normalized,&location);
        status=result_status==OpenMeteoCityStatus::kOk?kQweatherCityLookupOk:
               result_status==OpenMeteoCityStatus::kNotFound?kQweatherCityLookupNotFound:kQweatherCityLookupError;
        std::snprintf(resolved_city,sizeof(resolved_city),"%s",location.city);
        std::snprintf(latitude,sizeof(latitude),"%s",location.lat);
        std::snprintf(longitude,sizeof(longitude),"%s",location.lon);
    } else {
    status = qweather_lookup_city_status(normalized,
                                                                  city_id,
                                                                  sizeof(city_id),
                                                                  resolved_city,
                                                                  sizeof(resolved_city),
                                                                  latitude,
                                                                  sizeof(latitude),
                                                                  longitude,
                                                                  sizeof(longitude));
    }
    if (status != kQweatherCityLookupOk || resolved_city[0] == '\0') {
        if (result && result_len > 0) {
            std::snprintf(result,
                          result_len,
                          status == kQweatherCityLookupNotFound
                              ? "weather city was not found"
                              : "weather city validation failed");
        }
        return false;
    }
    char canonical_city[kManualWeatherCityLen] = {};
    if (!normalize_weather_city_input(resolved_city,
                                      canonical_city,
                                      sizeof(canonical_city)) ||
        canonical_city[0] == '\0') {
        return false;
    }
    if (!weather_city_pending_store(canonical_city)) {
        if (result && result_len > 0) {
            std::snprintf(result, result_len, "weather city state unavailable");
        }
        return false;
    }
    if (result && result_len > 0) {
        std::snprintf(result,
                      result_len,
                      "weather city validated as %s and will be saved",
                      canonical_city);
    }
    return true;
}
} // namespace

bool weather_city_mcp_init()
{
    if (!weather_city_pending_state_init()) {
        return false;
    }
    xiaozhi_mcp_register_weather_city_handler(handle_weather_city);
    return true;
}

bool weather_city_mcp_save_pending()
{
    return weather_city_pending_exists();
}

bool weather_city_mcp_flush_pending_save()
{
    WeatherCityPendingSnapshot snapshot = {};
    if (!weather_city_pending_snapshot(&snapshot)) {
        return false;
    }
    if (!snapshot.pending) {
        return true;
    }
    if (!save_manual_weather_city(snapshot.city)) {
        return false;
    }
    (void)weather_city_pending_clear(snapshot.generation);
    if (!offline_mode_enabled_load() && app_event_group_ready()) {
        (void)publish_network_sync_request(kManualWeatherSyncBit);
    }
    notify_ui_task();
    return true;
}
