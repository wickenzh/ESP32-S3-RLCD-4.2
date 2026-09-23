// 验证联网凭据在并发读写时始终以同一代成对快照对外提供。
#include "network_credentials_state.h"
#include "network_credentials_state_internal.h"
#include "weather_provider.h"

#include <assert.h>
#include <atomic>
#include <string.h>
#include <thread>

std::atomic<bool> g_fail_mutex_take{false};
std::atomic<int> g_mutex_take_count{0};
std::atomic<int> g_mutex_give_count{0};
std::atomic<int> g_mutex_active_holds{0};

namespace {
constexpr const char *kSsidA = "clock-net-a";
constexpr const char *kPasswordA = "password-a";
constexpr const char *kApiKeyA = "weather-key-a";
constexpr const char *kApiHostA = "a.re.qweatherapi.com";
constexpr const char *kSsidB = "clock-net-b";
constexpr const char *kPasswordB = "password-b";
constexpr const char *kApiKeyB = "weather-key-b";
constexpr const char *kApiHostB = "b.re.qweatherapi.com";
constexpr const char *kShortSsid = "n";
constexpr const char *kShortPassword = "p";
constexpr const char *kShortApiKey = "k";
constexpr const char *kShortApiHost = "s.qweatherapi.com";
constexpr const char *kMaxSsid = "12345678901234567890123456789012";
constexpr const char *kMaxPassword =
    "1234567890123456789012345678901234567890123456789012345678901234";
bool credentials_match(const char *actual_ssid,
                       const char *actual_password,
                       const char *ssid,
                       const char *password)
{
    return strcmp(actual_ssid, ssid) == 0 &&
           strcmp(actual_password, password) == 0 &&
           actual_ssid[0] != '\0';
}

void expect_text_with_zero_tail(const char *actual,
                                size_t capacity,
                                const char *expected)
{
    assert(actual);
    assert(expected);
    assert(strcmp(actual, expected) == 0);
    const size_t text_bytes = strlen(expected) + 1;
    assert(text_bytes <= capacity);
    for (size_t i = text_bytes; i < capacity; ++i) {
        assert(actual[i] == '\0');
    }
}

void expect_mutex_released()
{
    assert(g_mutex_active_holds.load(std::memory_order_acquire) == 0);
    assert(g_mutex_take_count.load(std::memory_order_acquire) ==
           g_mutex_give_count.load(std::memory_order_acquire));
}

void store_single_credentials(const char *ssid,
                              const char *password,
                              const char *api_key,
                              const char *api_host,
                              bool weather_key_configured,
                              bool weather_host_configured)
{
    network_credentials_store(ssid,
                              password,
                              "",
                              "",
                              WifiCredentialSlot::kSlotA,
                              api_key,
                              api_host,
                              weather_key_configured,
                              weather_host_configured);
}
} // namespace

int main()
{
    char ssid[kNetworkWifiSsidLen] = {};
    char password[kNetworkWifiPasswordLen] = {};
    memset(ssid, 0x7f, sizeof(ssid));
    memset(password, 0x7f, sizeof(password));
    assert(!network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(ssid[0] == '\0');
    assert(password[0] == '\0');
    store_single_credentials(
        kSsidA, kPasswordA, kApiKeyA, kApiHostA, true, true);
    assert(!network_all_online_credentials_configured());

    assert(network_credentials_state_init());
    assert(network_credentials_state_init());
    network_credentials_clear();
    assert(network_wifi_preferred_slot() == WifiCredentialSlot::kSlotA);
    assert(network_wifi_current_slot() == WifiCredentialSlot::kSlotA);
    assert(!network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(ssid[0] == '\0');
    assert(password[0] == '\0');
    assert(!network_all_online_credentials_configured());

    network_credentials_store(kSsidA,
                              kPasswordA,
                              kSsidB,
                              kPasswordB,
                              WifiCredentialSlot::kSlotB,
                              kApiKeyA,
                              kApiHostA,
                              true,
                              true);
    assert(network_wifi_preferred_slot() == WifiCredentialSlot::kSlotB);
    assert(network_wifi_current_slot() == WifiCredentialSlot::kSlotB);
    assert(network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(credentials_match(ssid, password, kSsidB, kPasswordB));
    assert(network_wifi_ssid_snapshot(ssid, sizeof(ssid)));
    assert(strcmp(ssid, kSsidB) == 0);
    assert(network_wifi_current_ssid_snapshot(ssid, sizeof(ssid)));
    assert(strcmp(ssid, kSsidB) == 0);
    assert(network_wifi_alternate_ssid_snapshot(ssid, sizeof(ssid)));
    assert(strcmp(ssid, kSsidA) == 0);
    assert(network_wifi_alternate_slot_configured());
    assert(network_wifi_select_slot(WifiCredentialSlot::kSlotA));
    assert(network_wifi_current_slot() == WifiCredentialSlot::kSlotA);
    assert(network_wifi_current_ssid_snapshot(ssid, sizeof(ssid)));
    assert(strcmp(ssid, kSsidA) == 0);
    assert(network_wifi_preferred_slot() == WifiCredentialSlot::kSlotB);
    assert(network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(credentials_match(ssid, password, kSsidA, kPasswordA));
    network_wifi_preferred_slot_store(WifiCredentialSlot::kSlotA);
    assert(network_wifi_preferred_slot() == WifiCredentialSlot::kSlotA);
    assert(network_wifi_current_slot() == WifiCredentialSlot::kSlotA);
    store_single_credentials(
        kSsidA, kPasswordA, kApiKeyA, kApiHostA, true, true);

    char api_key[kNetworkWeatherApiKeyLen] = {};
    char too_small[4] = {'x', '\0'};
    assert(!network_wifi_ssid_snapshot(too_small, sizeof(too_small)));
    assert(too_small[0] == '\0');
    assert(!network_weather_api_key_snapshot(nullptr, 0));
    memset(password, 'x', sizeof(password));
    assert(!network_wifi_credentials_copy(
        too_small, sizeof(too_small), password, sizeof(password)));
    assert(too_small[0] == '\0');
    assert(password[0] == '\0');
    memset(ssid, 'x', sizeof(ssid));
    assert(!network_wifi_credentials_copy(
        ssid, sizeof(ssid), too_small, sizeof(too_small)));
    assert(ssid[0] == '\0');
    assert(too_small[0] == '\0');
    memset(password, 'x', sizeof(password));
    assert(!network_wifi_credentials_copy(
        nullptr, 0, password, sizeof(password)));
    assert(password[0] == '\0');
    memset(ssid, 'x', sizeof(ssid));
    assert(!network_wifi_credentials_copy(
        ssid, sizeof(ssid), nullptr, 0));
    assert(ssid[0] == '\0');

    store_single_credentials(
        kSsidA, kPasswordA, kApiKeyA, kApiHostA, true, true);
    assert(network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(credentials_match(ssid, password, kSsidA, kPasswordA));
    char driver_ssid[kNetworkWifiSsidLen - 1] = {};
    char driver_password[kNetworkWifiPasswordLen - 1] = {};
    assert(network_wifi_credentials_copy(
        driver_ssid,
        sizeof(driver_ssid),
        driver_password,
        sizeof(driver_password)));
    assert(credentials_match(
        driver_ssid, driver_password, kSsidA, kPasswordA));
    store_single_credentials(
        kMaxSsid, kMaxPassword, kApiKeyA, kApiHostA, true, true);
    assert(network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(credentials_match(ssid, password, kMaxSsid, kMaxPassword));
    assert(network_wifi_credentials_copy(
        driver_ssid,
        sizeof(driver_ssid),
        driver_password,
        sizeof(driver_password)));
    assert(memcmp(driver_ssid, kMaxSsid, sizeof(driver_ssid)) == 0);
    assert(memcmp(driver_password,
                  kMaxPassword,
                  sizeof(driver_password)) == 0);
    assert(memchr(driver_ssid, '\0', sizeof(driver_ssid)) == nullptr);
    assert(memchr(driver_password, '\0', sizeof(driver_password)) == nullptr);
    store_single_credentials(
        kSsidA, kPasswordA, kApiKeyA, kApiHostA, true, true);
    assert(network_wifi_ssid_snapshot(ssid, sizeof(ssid)));
    assert(strcmp(ssid, kSsidA) == 0);
    assert(network_weather_api_key_snapshot(api_key, sizeof(api_key)));
    assert(strcmp(api_key, kApiKeyA) == 0);
    char api_host[kQweatherApiHostLen] = {};
    assert(network_weather_api_host_snapshot(api_host, sizeof(api_host)));
    assert(strcmp(api_host, kApiHostA) == 0);
    assert(network_weather_configuration_configured());
    assert(network_all_online_credentials_configured());
    expect_mutex_released();

    store_single_credentials(kShortSsid,
                             kShortPassword,
                             kShortApiKey,
                             kShortApiHost,
                             true,
                             true);
    assert(network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(network_weather_api_key_snapshot(api_key, sizeof(api_key)));
    assert(network_weather_api_host_snapshot(api_host, sizeof(api_host)));
    expect_text_with_zero_tail(ssid, sizeof(ssid), kShortSsid);
    expect_text_with_zero_tail(password, sizeof(password), kShortPassword);
    expect_text_with_zero_tail(api_key, sizeof(api_key), kShortApiKey);
    expect_text_with_zero_tail(api_host, sizeof(api_host), kShortApiHost);
    store_single_credentials(
        kSsidA, kPasswordA, kApiKeyA, kApiHostA, true, true);

    const NetworkCredentialsAvailability availability_before_failure =
        network_credentials_availability();
    g_fail_mutex_take.store(true, std::memory_order_release);
    store_single_credentials(
        kSsidB, kPasswordB, kApiKeyB, kApiHostB, true, true);
    memset(ssid, 0x7f, sizeof(ssid));
    memset(password, 0x7f, sizeof(password));
    assert(!network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(ssid[0] == '\0');
    assert(password[0] == '\0');
    memset(ssid, 'x', sizeof(ssid));
    assert(!network_wifi_ssid_snapshot(ssid, sizeof(ssid)));
    assert(ssid[0] == '\0');
    memset(ssid, 'x', sizeof(ssid));
    assert(!network_wifi_current_ssid_snapshot(ssid, sizeof(ssid)));
    assert(ssid[0] == '\0');
    assert(network_credentials_availability().wifi_configured ==
           availability_before_failure.wifi_configured);
    assert(network_credentials_availability().weather_api_key_configured ==
           availability_before_failure.weather_api_key_configured);
    assert(network_credentials_availability().weather_api_host_configured ==
           availability_before_failure.weather_api_host_configured);
    g_fail_mutex_take.store(false, std::memory_order_release);
    assert(network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(credentials_match(ssid, password, kSsidA, kPasswordA));
    expect_mutex_released();

    std::atomic<bool> writer_done{false};
    std::thread writer([&]() {
        for (int i = 0; i < 10000; ++i) {
            if (i & 1) {
                store_single_credentials(
                    kSsidA, kPasswordA, kApiKeyA, kApiHostA, true, true);
            } else {
                store_single_credentials(
                    kSsidB, kPasswordB, kApiKeyB, kApiHostB, true, true);
            }
        }
        writer_done.store(true, std::memory_order_release);
    });
    do {
        assert(network_wifi_credentials_copy(
            ssid, sizeof(ssid), password, sizeof(password)));
        assert(credentials_match(ssid, password, kSsidA, kPasswordA) ||
               credentials_match(ssid, password, kSsidB, kPasswordB));
    } while (!writer_done.load(std::memory_order_acquire));
    writer.join();

    writer_done.store(false, std::memory_order_release);
    std::thread availability_writer([&]() {
        for (int i = 0; i < 10000; ++i) {
            if (i & 1) {
                store_single_credentials(
                    kSsidA, kPasswordA, kApiKeyA, kApiHostA, true, true);
            } else {
                network_credentials_clear();
            }
        }
        writer_done.store(true, std::memory_order_release);
    });
    do {
        const NetworkCredentialsAvailability availability =
            network_credentials_availability();
        assert(availability.wifi_configured ==
               availability.weather_api_key_configured);
        assert(availability.weather_api_key_configured ==
               availability.weather_api_host_configured);
    } while (!writer_done.load(std::memory_order_acquire));
    availability_writer.join();

    store_single_credentials("", "unused", "", "", true, true);
    assert(!network_wifi_credentials_copy(
        ssid, sizeof(ssid), password, sizeof(password)));
    assert(ssid[0] == '\0');
    assert(password[0] == '\0');
    assert(!network_wifi_credentials_configured());
    const NetworkCredentialsAvailability empty_availability =
        network_credentials_availability();
    assert(!empty_availability.weather_api_key_configured);
    assert(!empty_availability.weather_api_host_configured);

    network_credentials_clear();
    expect_mutex_released();
    weather_provider_store(WeatherProvider::kOpenMeteo);
    assert(network_weather_configuration_configured());
    assert(!network_all_online_credentials_configured());
    store_single_credentials("Demo", "password", "", "", false, false);
    assert(network_all_online_credentials_configured());
    weather_provider_store(WeatherProvider::kQweather);
    assert(!network_weather_configuration_configured());
    return 0;
}
