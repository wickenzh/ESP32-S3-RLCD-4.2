// 通过小智 MCP 校验并保存手动天气城市，复用现有 QWeather 和 NVS 配置路径。
#include "weather_city_mcp.h"

#include "app_state.h"
#include "network_credentials_state.h"
#include "network_services.h"
#include "ui_views.h"
#include "xiaozhi_mcp.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

namespace {
constexpr size_t kCityIdLen = 32;
constexpr size_t kCityCoordinateLen = 24;

portMUX_TYPE s_pending_city_mux = portMUX_INITIALIZER_UNLOCKED;
char s_pending_city[kManualWeatherCityLen] = {};
bool s_save_pending = false;

void set_pending_city(const char *city)
{
    portENTER_CRITICAL(&s_pending_city_mux);
    strlcpy(s_pending_city, city ? city : "", sizeof(s_pending_city));
    s_save_pending = true;
    portEXIT_CRITICAL(&s_pending_city_mux);
}

void get_pending_city(char *out, size_t out_len, bool *pending)
{
    if (!out || out_len == 0 || !pending) {
        return;
    }
    portENTER_CRITICAL(&s_pending_city_mux);
    strlcpy(out, s_pending_city, out_len);
    *pending = s_save_pending;
    portEXIT_CRITICAL(&s_pending_city_mux);
}

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
        set_pending_city("");
        if (result && result_len > 0) {
            std::snprintf(result,
                          result_len,
                          "automatic IP weather location will be restored");
        }
        return true;
    }
    if (g_offline_mode_ui_enabled || !network_weather_api_key_configured()) {
        if (result && result_len > 0) {
            std::snprintf(result, result_len, "weather service is not configured");
        }
        return false;
    }

    char city_id[kCityIdLen] = {};
    char resolved_city[kManualWeatherCityLen] = {};
    char latitude[kCityCoordinateLen] = {};
    char longitude[kCityCoordinateLen] = {};
    QweatherCityLookupStatus status = qweather_lookup_city_status(normalized,
                                                                  city_id,
                                                                  sizeof(city_id),
                                                                  resolved_city,
                                                                  sizeof(resolved_city),
                                                                  latitude,
                                                                  sizeof(latitude),
                                                                  longitude,
                                                                  sizeof(longitude));
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
    set_pending_city(canonical_city);
    if (result && result_len > 0) {
        std::snprintf(result,
                      result_len,
                      "weather city validated as %s and will be saved",
                      canonical_city);
    }
    return true;
}
} // namespace

void weather_city_mcp_init()
{
    xiaozhi_mcp_register_weather_city_handler(handle_weather_city);
}

bool weather_city_mcp_save_pending()
{
    portENTER_CRITICAL(&s_pending_city_mux);
    bool pending = s_save_pending;
    portEXIT_CRITICAL(&s_pending_city_mux);
    return pending;
}

bool weather_city_mcp_flush_pending_save()
{
    char city[kManualWeatherCityLen] = {};
    bool pending = false;
    get_pending_city(city, sizeof(city), &pending);
    if (!pending) {
        return true;
    }
    if (!save_manual_weather_city(city)) {
        return false;
    }
    portENTER_CRITICAL(&s_pending_city_mux);
    if (strcmp(s_pending_city, city) == 0) {
        s_pending_city[0] = '\0';
        s_save_pending = false;
    }
    portEXIT_CRITICAL(&s_pending_city_mux);
    if (!g_offline_mode_ui_enabled && g_app_events) {
        xEventGroupSetBits(g_app_events, kManualWeatherSyncBit);
    }
    notify_ui_task();
    return true;
}
