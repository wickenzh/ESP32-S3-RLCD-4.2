// 集中维护 Wi-Fi 凭据、天气 API Key 及其可用状态。
#include "network_credentials_state_internal.h"
#include "weather_provider.h"

#include "scoped_semaphore_lock.h"

#include <esp_attr.h>

#include <atomic>
#include <stdint.h>
#include <string.h>

namespace {
constexpr uint8_t kWifiConfiguredMask = 1U << 0;
constexpr uint8_t kWeatherApiKeyConfiguredMask = 1U << 1;
constexpr uint8_t kWeatherApiHostConfiguredMask = 1U << 2;
constexpr uint8_t kWifiSlotCount = 2;
struct WifiCredentialEntry {
    char ssid[kNetworkWifiSsidLen] = {};
    char password[kNetworkWifiPasswordLen] = {};
    bool configured = false;
};
struct NetworkCredentialsState {
    WifiCredentialEntry wifi[kWifiSlotCount] = {};
    char weather_api_key[kNetworkWeatherApiKeyLen] = {};
    char weather_api_host[kQweatherApiHostLen] = {};
    WifiCredentialSlot preferred_slot = WifiCredentialSlot::kSlotA;
    WifiCredentialSlot current_slot = WifiCredentialSlot::kSlotA;
    bool weather_api_key_configured = false;
    bool weather_api_host_configured = false;
};
StaticTaskMutex s_credentials_mutex;
EXT_RAM_BSS_ATTR NetworkCredentialsState s_credentials = {};
std::atomic<uint8_t> s_credentials_availability_mask{0};

static_assert((kWifiConfiguredMask & kWeatherApiKeyConfiguredMask) == 0,
              "credential availability flags must not overlap");
static_assert((kWifiConfiguredMask & kWeatherApiHostConfiguredMask) == 0 &&
                  (kWeatherApiKeyConfiguredMask &
                   kWeatherApiHostConfiguredMask) == 0,
              "credential availability flags must not overlap");
static_assert(sizeof(NetworkCredentialsState) == 426,
              "credential state size changed; re-evaluate PSRAM placement");

constexpr size_t wifi_slot_index(WifiCredentialSlot slot)
{
    return static_cast<size_t>(slot);
}

constexpr bool wifi_slot_valid(WifiCredentialSlot slot)
{
    return wifi_credential_slot_valid(slot) &&
           wifi_slot_index(slot) < kWifiSlotCount;
}

WifiCredentialSlot normalized_preferred_slot(
    WifiCredentialSlot requested,
    const WifiCredentialEntry (&wifi)[kWifiSlotCount])
{
    if (wifi_slot_valid(requested) &&
        wifi[wifi_slot_index(requested)].configured) {
        return requested;
    }
    if (wifi[wifi_slot_index(WifiCredentialSlot::kSlotA)].configured) {
        return WifiCredentialSlot::kSlotA;
    }
    if (wifi[wifi_slot_index(WifiCredentialSlot::kSlotB)].configured) {
        return WifiCredentialSlot::kSlotB;
    }
    return WifiCredentialSlot::kSlotA;
}

uint8_t credentials_availability_mask(const NetworkCredentialsState &credentials)
{
    uint8_t mask = 0;
    if (credentials.wifi[wifi_slot_index(credentials.preferred_slot)].configured) {
        mask |= kWifiConfiguredMask;
    }
    if (credentials.weather_api_key_configured) {
        mask |= kWeatherApiKeyConfiguredMask;
    }
    if (credentials.weather_api_host_configured) {
        mask |= kWeatherApiHostConfiguredMask;
    }
    return mask;
}

template <size_t N>
bool copy_field_snapshot(char *out,
                         size_t out_len,
                         const char (&field)[N],
                         const bool &configured)
{
    if (!out || out_len < N) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    if (!state_lock) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, field, N);
    const bool available = configured && out[0] != '\0';
    return available;
}

void clear_wifi_credentials_outputs(char *ssid,
                                    size_t ssid_len,
                                    char *password,
                                    size_t password_len)
{
    if (ssid && ssid_len > 0) {
        const size_t clear_len =
            ssid_len < kNetworkWifiSsidLen ? ssid_len : kNetworkWifiSsidLen;
        memset(ssid, 0, clear_len);
    }
    if (password && password_len > 0) {
        const size_t clear_len =
            password_len < kNetworkWifiPasswordLen
                ? password_len
                : kNetworkWifiPasswordLen;
        memset(password, 0, clear_len);
    }
}

void store_wifi_entry(WifiCredentialEntry *entry,
                      const char *ssid,
                      const char *password)
{
    if (!entry) {
        return;
    }
    const char *ssid_source = ssid ? ssid : "";
    const char *password_source = password ? password : "";
    const size_t ssid_length =
        strnlen(ssid_source, kNetworkWifiSsidLen - 1);
    const size_t password_length =
        strnlen(password_source, kNetworkWifiPasswordLen - 1);
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->ssid, ssid_source, ssid_length);
    memcpy(entry->password, password_source, password_length);
    entry->configured = ssid_length > 0;
}
} // namespace

bool network_credentials_state_init()
{
    return s_credentials_mutex.init();
}

bool network_wifi_credentials_copy(char *ssid,
                                   size_t ssid_len,
                                   char *password,
                                   size_t password_len)
{
    return network_wifi_credentials_for_slot_copy(
        network_wifi_current_slot(), ssid, ssid_len, password, password_len);
}

bool network_wifi_credentials_for_slot_copy(WifiCredentialSlot slot,
                                            char *ssid,
                                            size_t ssid_len,
                                            char *password,
                                            size_t password_len)
{
    clear_wifi_credentials_outputs(ssid, ssid_len, password, password_len);
    if (!ssid || !password ||
        ssid_len < kNetworkWifiSsidLen - 1 ||
        password_len < kNetworkWifiPasswordLen - 1 ||
        !wifi_slot_valid(slot)) {
        return false;
    }
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    if (!state_lock) {
        return false;
    }
    const WifiCredentialEntry &entry =
        s_credentials.wifi[wifi_slot_index(slot)];
    const size_t ssid_copy_len = kNetworkWifiSsidLen - 1;
    const size_t password_copy_len = kNetworkWifiPasswordLen - 1;
    memcpy(ssid, entry.ssid, ssid_copy_len);
    memcpy(password, entry.password, password_copy_len);
    const bool available = entry.configured && ssid[0] != '\0';
    if (!available) {
        clear_wifi_credentials_outputs(
            ssid, ssid_len, password, password_len);
    }
    return available;
}

NetworkCredentialsAvailability network_credentials_availability()
{
    const uint8_t mask =
        s_credentials_availability_mask.load(std::memory_order_acquire);
    const NetworkCredentialsAvailability availability = {
        (mask & kWifiConfiguredMask) != 0,
        (mask & kWeatherApiKeyConfiguredMask) != 0,
        (mask & kWeatherApiHostConfiguredMask) != 0,
    };
    return availability;
}

void network_credentials_store(const char *ssid_a,
                               const char *password_a,
                               const char *ssid_b,
                               const char *password_b,
                               WifiCredentialSlot preferred_slot,
                               const char *weather_api_key,
                               const char *weather_api_host,
                               bool weather_api_key_configured,
                               bool weather_api_host_configured)
{
    const char *api_key_source = weather_api_key ? weather_api_key : "";
    const char *api_host_source = weather_api_host ? weather_api_host : "";
    const size_t api_key_length =
        strnlen(api_key_source, kNetworkWeatherApiKeyLen - 1);
    const size_t api_host_length =
        strnlen(api_host_source, kQweatherApiHostLen - 1);

    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    if (!state_lock) {
        return;
    }
    memset(&s_credentials, 0, sizeof(s_credentials));
    store_wifi_entry(&s_credentials.wifi[0], ssid_a, password_a);
    store_wifi_entry(&s_credentials.wifi[1], ssid_b, password_b);
    memcpy(s_credentials.weather_api_key, api_key_source, api_key_length);
    memcpy(s_credentials.weather_api_host, api_host_source, api_host_length);
    s_credentials.preferred_slot = normalized_preferred_slot(
        preferred_slot, s_credentials.wifi);
    s_credentials.current_slot = s_credentials.preferred_slot;
    s_credentials.weather_api_key_configured =
        weather_api_key_configured && api_key_length > 0;
    s_credentials.weather_api_host_configured =
        weather_api_host_configured && api_host_length > 0;
    s_credentials_availability_mask.store(
        credentials_availability_mask(s_credentials),
        std::memory_order_release);
}

void network_credentials_clear()
{
    network_credentials_store("",
                              "",
                              "",
                              "",
                              WifiCredentialSlot::kSlotA,
                              "",
                              "",
                              false,
                              false);
}

bool network_wifi_select_slot(WifiCredentialSlot slot)
{
    if (!wifi_slot_valid(slot)) {
        return false;
    }
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    if (!state_lock ||
        !s_credentials.wifi[wifi_slot_index(slot)].configured) {
        return false;
    }
    s_credentials.current_slot = slot;
    return true;
}

WifiCredentialSlot network_wifi_current_slot()
{
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    return state_lock ? s_credentials.current_slot
                      : WifiCredentialSlot::kSlotA;
}

WifiCredentialSlot network_wifi_preferred_slot()
{
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    return state_lock ? s_credentials.preferred_slot
                      : WifiCredentialSlot::kSlotA;
}

bool network_wifi_alternate_slot_configured()
{
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    if (!state_lock) {
        return false;
    }
    const WifiCredentialSlot alternate =
        wifi_alternate_credential_slot(s_credentials.preferred_slot);
    return s_credentials.wifi[wifi_slot_index(alternate)].configured;
}

void network_wifi_preferred_slot_store(WifiCredentialSlot slot)
{
    if (!wifi_slot_valid(slot)) {
        return;
    }
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    if (!state_lock ||
        !s_credentials.wifi[wifi_slot_index(slot)].configured) {
        return;
    }
    s_credentials.preferred_slot = slot;
    s_credentials.current_slot = slot;
    s_credentials_availability_mask.store(
        credentials_availability_mask(s_credentials),
        std::memory_order_release);
}

bool network_wifi_credentials_configured()
{
    return network_credentials_availability().wifi_configured;
}

bool network_weather_configuration_configured()
{
    const NetworkCredentialsAvailability availability =
        network_credentials_availability();
    return weather_provider_load() == WeatherProvider::kOpenMeteo ||
           (availability.weather_api_key_configured &&
           availability.weather_api_host_configured);
}

bool network_all_online_credentials_configured()
{
    const NetworkCredentialsAvailability availability = network_credentials_availability();
    return availability.wifi_configured &&
           (weather_provider_load() == WeatherProvider::kOpenMeteo ||
            (availability.weather_api_key_configured && availability.weather_api_host_configured));
}

bool network_wifi_ssid_snapshot(char *out, size_t out_len)
{
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    if (!state_lock) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    const WifiCredentialEntry &entry =
        s_credentials.wifi[wifi_slot_index(s_credentials.preferred_slot)];
    if (!out || out_len < sizeof(entry.ssid)) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    memcpy(out, entry.ssid, sizeof(entry.ssid));
    return entry.configured && out[0] != '\0';
}

bool network_wifi_alternate_ssid_snapshot(char *out, size_t out_len)
{
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    if (!state_lock) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    const WifiCredentialSlot alternate =
        wifi_alternate_credential_slot(s_credentials.preferred_slot);
    const WifiCredentialEntry &entry =
        s_credentials.wifi[wifi_slot_index(alternate)];
    if (!out || out_len < sizeof(entry.ssid)) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    memcpy(out, entry.ssid, sizeof(entry.ssid));
    return entry.configured && out[0] != '\0';
}

bool network_wifi_current_ssid_snapshot(char *out, size_t out_len)
{
    ScopedSemaphoreLock state_lock(s_credentials_mutex);
    if (!state_lock) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    const WifiCredentialEntry &entry =
        s_credentials.wifi[wifi_slot_index(s_credentials.current_slot)];
    if (!out || out_len < sizeof(entry.ssid)) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    memcpy(out, entry.ssid, sizeof(entry.ssid));
    return entry.configured && out[0] != '\0';
}

bool network_weather_api_key_snapshot(char *out, size_t out_len)
{
    return copy_field_snapshot(
        out, out_len, s_credentials.weather_api_key, s_credentials.weather_api_key_configured);
}

bool network_weather_api_host_snapshot(char *out, size_t out_len)
{
    return copy_field_snapshot(
        out,
        out_len,
        s_credentials.weather_api_host,
        s_credentials.weather_api_host_configured);
}
