// 以完整字符串快照保存手动天气城市，避免跨任务读取半写入 UTF-8。
#include "manual_weather_city_state.h"

#include "app_state.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

namespace {
portMUX_TYPE s_manual_weather_city_mux = portMUX_INITIALIZER_UNLOCKED;
char s_manual_weather_city[kManualWeatherCityLen] = {};
}

bool manual_weather_city_snapshot(char *out, size_t out_len)
{
    if (!out || out_len < sizeof(s_manual_weather_city)) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    portENTER_CRITICAL(&s_manual_weather_city_mux);
    memcpy(out, s_manual_weather_city, sizeof(s_manual_weather_city));
    const bool configured = s_manual_weather_city[0] != '\0';
    portEXIT_CRITICAL(&s_manual_weather_city_mux);
    return configured;
}

void manual_weather_city_store(const char *city)
{
    char replacement[kManualWeatherCityLen] = {};
    strlcpy(replacement, city ? city : "", sizeof(replacement));
    portENTER_CRITICAL(&s_manual_weather_city_mux);
    memcpy(s_manual_weather_city, replacement, sizeof(s_manual_weather_city));
    portEXIT_CRITICAL(&s_manual_weather_city_mux);
}

bool manual_weather_city_is_configured()
{
    portENTER_CRITICAL(&s_manual_weather_city_mux);
    const bool configured = s_manual_weather_city[0] != '\0';
    portEXIT_CRITICAL(&s_manual_weather_city_mux);
    return configured;
}
