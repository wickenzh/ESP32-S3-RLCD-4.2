// 在网络任务栈上校验配网保存的天气 API 密钥和可选手动城市。
#include "provisioning_validation.h"

#include "app_metadata.h"
#include "manual_weather_city_state.h"
#include "qweather_client.h"
#include "weather_provider.h"
#include "open_meteo_client.h"

#include "esp_attr.h"
#include "esp_log.h"

namespace {
constexpr size_t kProvisioningWeatherCityIdSize = 24;
constexpr size_t kProvisioningWeatherCityNameSize = 32;
constexpr const char *kProvisioningWeatherApiProbeCityId = "101010100";
#define PROVISIONING_MANUAL_WEATHER_CITY_VALIDATED_FORMAT \
    "provisioning manual weather city validated: %s id=%s"
constexpr const char *kProvisioningWeatherApiValidationFailedLog =
    "provisioning weather API validation failed";
constexpr const char *kProvisioningWeatherCityValidationFailedLog =
    "provisioning manual weather city validation failed";
constexpr const char *kProvisioningWeatherCityValidationErrorLog =
    "provisioning manual weather city validation interrupted";

// Validation runs only in the serialized network task. Keep the probe result
// out of that task's stack while the QWeather HTTPS request owns TLS memory.
EXT_RAM_BSS_ATTR WeatherData s_provisioning_probe_weather;
static_assert(sizeof(WeatherData) >= 96,
              "provisioning weather probe should remain off the network task stack");
} // namespace

WifiPortalSaveResult validate_saved_provisioning_weather_configuration()
{
    if(weather_provider_load()==WeatherProvider::kOpenMeteo) {
        return open_meteo_validate_configuration()?WifiPortalSaveResult::kSuccess:WifiPortalSaveResult::kWeatherApiFailed;
    }
    s_provisioning_probe_weather = {};
    if (!qweather_fetch_now(kProvisioningWeatherApiProbeCityId,
                            &s_provisioning_probe_weather)) {
        ESP_LOGW(TAG, "%s", kProvisioningWeatherApiValidationFailedLog);
        return WifiPortalSaveResult::kWeatherApiFailed;
    }

    char weather_city[kManualWeatherCityLen] = {};
    if (!manual_weather_city_snapshot(weather_city, sizeof(weather_city))) {
        return WifiPortalSaveResult::kSuccess;
    }
    char city_id[kProvisioningWeatherCityIdSize] = {};
    char city_name[kProvisioningWeatherCityNameSize] = {};
    QweatherCityLookupStatus status = qweather_lookup_city_status(weather_city,
                                                                  city_id,
                                                                  sizeof(city_id),
                                                                  city_name,
                                                                  sizeof(city_name));
    if (status == kQweatherCityLookupOk) {
        ESP_LOGI(TAG,
                 PROVISIONING_MANUAL_WEATHER_CITY_VALIDATED_FORMAT,
                 city_name,
                 city_id);
        return WifiPortalSaveResult::kSuccess;
    }
    if (status == kQweatherCityLookupNotFound) {
        ESP_LOGW(TAG, "%s", kProvisioningWeatherCityValidationFailedLog);
        return WifiPortalSaveResult::kWeatherCityInvalid;
    }
    ESP_LOGW(TAG, "%s", kProvisioningWeatherCityValidationErrorLog);
    return WifiPortalSaveResult::kWeatherApiFailed;
}
