// 声明 Wi-Fi 凭据与天气 API Key 的跨任务完整快照接口。
#pragma once

#include <stddef.h>

inline constexpr size_t kNetworkWifiSsidLen = 33;
inline constexpr size_t kNetworkWifiPasswordLen = 65;
inline constexpr size_t kNetworkWeatherApiKeyLen = 96;

struct NetworkCredentialsSnapshot {
    char wifi_ssid[kNetworkWifiSsidLen] = {};
    char wifi_password[kNetworkWifiPasswordLen] = {};
    char weather_api_key[kNetworkWeatherApiKeyLen] = {};
    bool wifi_configured = false;
    bool weather_api_key_configured = false;
};

struct NetworkCredentialsAvailability {
    bool wifi_configured = false;
    bool weather_api_key_configured = false;
};

void network_credentials_snapshot(NetworkCredentialsSnapshot *out);
NetworkCredentialsAvailability network_credentials_availability();
void network_credentials_store(const char *ssid,
                               const char *password,
                               const char *weather_api_key,
                               bool wifi_configured,
                               bool weather_api_key_configured);
void network_credentials_clear();
bool network_wifi_credentials_configured();
bool network_weather_api_key_configured();
bool network_all_online_credentials_configured();
bool network_wifi_ssid_snapshot(char *out, size_t out_len);
bool network_weather_api_key_snapshot(char *out, size_t out_len);
