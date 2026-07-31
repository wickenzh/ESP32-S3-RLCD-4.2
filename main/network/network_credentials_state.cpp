// 集中维护 Wi-Fi 凭据、天气 API Key 及其可用状态的完整快照。
#include "network_credentials_state.h"

#include "freertos/FreeRTOS.h"

#include <string.h>

namespace {
portMUX_TYPE s_credentials_mux = portMUX_INITIALIZER_UNLOCKED;
NetworkCredentialsSnapshot s_credentials;

template <size_t N>
void copy_text(char (&out)[N], const char *value)
{
    const char *source = value ? value : "";
    const size_t length = strnlen(source, N - 1);
    memcpy(out, source, length);
    out[length] = '\0';
}

template <size_t N>
bool copy_field_snapshot(char *out, size_t out_len, const char (&field)[N], const bool &configured)
{
    if (!out || out_len < N) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    portENTER_CRITICAL(&s_credentials_mux);
    memcpy(out, field, N);
    const bool available = configured && out[0] != '\0';
    portEXIT_CRITICAL(&s_credentials_mux);
    return available;
}
} // namespace

void network_credentials_snapshot(NetworkCredentialsSnapshot *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_credentials_mux);
    memcpy(out, &s_credentials, sizeof(*out));
    portEXIT_CRITICAL(&s_credentials_mux);
}

NetworkCredentialsAvailability network_credentials_availability()
{
    portENTER_CRITICAL(&s_credentials_mux);
    const NetworkCredentialsAvailability availability = {
        s_credentials.wifi_configured,
        s_credentials.weather_api_key_configured,
    };
    portEXIT_CRITICAL(&s_credentials_mux);
    return availability;
}

void network_credentials_store(const char *ssid,
                               const char *password,
                               const char *weather_api_key,
                               bool wifi_configured,
                               bool weather_api_key_configured)
{
    NetworkCredentialsSnapshot replacement;
    copy_text(replacement.wifi_ssid, ssid);
    copy_text(replacement.wifi_password, password);
    copy_text(replacement.weather_api_key, weather_api_key);
    replacement.wifi_configured = wifi_configured && replacement.wifi_ssid[0] != '\0';
    replacement.weather_api_key_configured =
        weather_api_key_configured && replacement.weather_api_key[0] != '\0';

    portENTER_CRITICAL(&s_credentials_mux);
    memcpy(&s_credentials, &replacement, sizeof(s_credentials));
    portEXIT_CRITICAL(&s_credentials_mux);
}

void network_credentials_clear()
{
    network_credentials_store("", "", "", false, false);
}

bool network_wifi_credentials_configured()
{
    return network_credentials_availability().wifi_configured;
}

bool network_weather_api_key_configured()
{
    return network_credentials_availability().weather_api_key_configured;
}

bool network_all_online_credentials_configured()
{
    const NetworkCredentialsAvailability availability = network_credentials_availability();
    return availability.wifi_configured && availability.weather_api_key_configured;
}

bool network_wifi_ssid_snapshot(char *out, size_t out_len)
{
    return copy_field_snapshot(
        out, out_len, s_credentials.wifi_ssid, s_credentials.wifi_configured);
}

bool network_weather_api_key_snapshot(char *out, size_t out_len)
{
    return copy_field_snapshot(
        out, out_len, s_credentials.weather_api_key, s_credentials.weather_api_key_configured);
}
