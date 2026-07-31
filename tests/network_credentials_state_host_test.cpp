// 验证联网凭据在并发读写时始终以同一代完整快照对外提供。
#include "network_credentials_state.h"

#include <assert.h>
#include <atomic>
#include <string.h>
#include <thread>

namespace {
constexpr const char *kSsidA = "clock-net-a";
constexpr const char *kPasswordA = "password-a";
constexpr const char *kApiKeyA = "weather-key-a";
constexpr const char *kSsidB = "clock-net-b";
constexpr const char *kPasswordB = "password-b";
constexpr const char *kApiKeyB = "weather-key-b";

bool snapshot_matches(const NetworkCredentialsSnapshot &snapshot,
                      const char *ssid,
                      const char *password,
                      const char *api_key)
{
    return snapshot.wifi_configured &&
           snapshot.weather_api_key_configured &&
           strcmp(snapshot.wifi_ssid, ssid) == 0 &&
           strcmp(snapshot.wifi_password, password) == 0 &&
           strcmp(snapshot.weather_api_key, api_key) == 0;
}
} // namespace

int main()
{
    network_credentials_clear();
    NetworkCredentialsSnapshot snapshot = {};
    network_credentials_snapshot(&snapshot);
    assert(!snapshot.wifi_configured);
    assert(!snapshot.weather_api_key_configured);
    assert(snapshot.wifi_ssid[0] == '\0');
    assert(snapshot.wifi_password[0] == '\0');
    assert(snapshot.weather_api_key[0] == '\0');
    assert(!network_all_online_credentials_configured());

    char ssid[kNetworkWifiSsidLen] = {};
    char api_key[kNetworkWeatherApiKeyLen] = {};
    char too_small[4] = {'x', '\0'};
    assert(!network_wifi_ssid_snapshot(too_small, sizeof(too_small)));
    assert(too_small[0] == '\0');
    assert(!network_weather_api_key_snapshot(nullptr, 0));

    network_credentials_store(kSsidA, kPasswordA, kApiKeyA, true, true);
    network_credentials_snapshot(&snapshot);
    assert(snapshot_matches(snapshot, kSsidA, kPasswordA, kApiKeyA));
    assert(network_wifi_ssid_snapshot(ssid, sizeof(ssid)));
    assert(strcmp(ssid, kSsidA) == 0);
    assert(network_weather_api_key_snapshot(api_key, sizeof(api_key)));
    assert(strcmp(api_key, kApiKeyA) == 0);
    assert(network_all_online_credentials_configured());

    std::atomic<bool> writer_done{false};
    std::thread writer([&]() {
        for (int i = 0; i < 10000; ++i) {
            if (i & 1) {
                network_credentials_store(kSsidA, kPasswordA, kApiKeyA, true, true);
            } else {
                network_credentials_store(kSsidB, kPasswordB, kApiKeyB, true, true);
            }
        }
        writer_done.store(true, std::memory_order_release);
    });
    do {
        network_credentials_snapshot(&snapshot);
        assert(snapshot_matches(snapshot, kSsidA, kPasswordA, kApiKeyA) ||
               snapshot_matches(snapshot, kSsidB, kPasswordB, kApiKeyB));
    } while (!writer_done.load(std::memory_order_acquire));
    writer.join();

    network_credentials_store("", "unused", "", true, true);
    network_credentials_snapshot(&snapshot);
    assert(!snapshot.wifi_configured);
    assert(!snapshot.weather_api_key_configured);
    assert(!network_wifi_credentials_configured());
    assert(!network_weather_api_key_configured());

    network_credentials_clear();
    return 0;
}
