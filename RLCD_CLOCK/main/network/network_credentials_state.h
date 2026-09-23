// 声明 Wi-Fi、天气 API Key 与 API Host 的跨任务窄复制接口。
#pragma once

#include "qweather_api_host.h"
#include "wifi_failover_policy.h"

#include <stddef.h>

inline constexpr size_t kNetworkWifiSsidLen = 33;
inline constexpr size_t kNetworkWifiPasswordLen = 65;
inline constexpr size_t kNetworkWeatherApiKeyLen = 96;

struct NetworkCredentialsAvailability {
    bool wifi_configured = false;
    bool weather_api_key_configured = false;
    bool weather_api_host_configured = false;
};

// ESP-IDF STA-sized outputs use all bytes for maximum-length fields and are
// therefore not NUL-terminated in that one valid boundary case.
bool network_wifi_credentials_copy(char *ssid,
                                   size_t ssid_len,
                                   char *password,
                                   size_t password_len);
NetworkCredentialsAvailability network_credentials_availability();
bool network_wifi_credentials_configured();
bool network_weather_configuration_configured();
bool network_all_online_credentials_configured();
bool network_wifi_ssid_snapshot(char *out, size_t out_len);
bool network_wifi_alternate_ssid_snapshot(char *out, size_t out_len);
bool network_wifi_current_ssid_snapshot(char *out, size_t out_len);
bool network_weather_api_key_snapshot(char *out, size_t out_len);
bool network_weather_api_host_snapshot(char *out, size_t out_len);
