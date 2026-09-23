// 调用独立 IP 定位服务并转换为天气城市查询所需的位置文本。
#include "ip_geolocation_client.h"

#include "network_http_client.h"

#include "app_constexpr.h"
#include "app_metadata.h"
#include "app_text_format.h"
#include "network_cache_policy.h"
#include "network_credentials_state.h"
#include "qweather_location_text.h"
#include "qweather_response.h"
#include "wifi_portal_state.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <atomic>
#include <string.h>
#include <type_traits>

namespace {
constexpr size_t kIpGeoResponseBufferSize = 2048;
constexpr int64_t kIpGeoLocationCacheTtlUs = 6LL * 60LL * 60LL * 1000000LL;
constexpr const char *kIpGeolocationUrl = "https://uapis.cn/api/v1/network/myip";
constexpr const char *kIpGeoStage = "ip location";
constexpr const char *kIpGeoJsonLatitudeField = "latitude";
constexpr const char *kIpGeoJsonLongitudeField = "longitude";
constexpr const char *kIpGeoJsonRegionField = "region";
constexpr const char *kIpLocationMissingCoordinateLog =
    "ip location response missing latitude/longitude";
#define IP_LOCATION_RESOLVED_FORMAT "ip location resolved: %s city=%s"

struct IpGeolocationCache {
    bool valid;
    uint32_t generation;
    int64_t expires_at_us;
    char preferred_ssid[kNetworkWifiSsidLen];
    char alternate_ssid[kNetworkWifiSsidLen];
    char current_ssid[kNetworkWifiSsidLen];
    char station_ip[16];
    char location[40];
    char city[32];
};
static_assert(std::is_trivial_v<IpGeolocationCache>,
              "IP geolocation cache must remain zero-initialized BSS data");

// The serialized network task is the sole cache reader/writer. Configuration
// tasks only bump the atomic generation to invalidate the current entry.
EXT_RAM_BSS_ATTR IpGeolocationCache s_ip_geolocation_cache;
std::atomic<uint32_t> s_ip_geolocation_cache_generation{0};

bool coordinates_available(const cJSON *latitude, const cJSON *longitude)
{
    return cJSON_IsNumber(latitude) && cJSON_IsNumber(longitude);
}

void log_ip_geolocation_warning(const char *message)
{
    ESP_LOGW(TAG, "%s", cstr_nonempty(message) ? message : kIpGeoStage);
}

static_assert(kIpGeoResponseBufferSize > 1,
              "IP geolocation response buffer must fit text and NUL");
} // namespace

bool ip_geolocation_lookup(char *location, size_t location_len, char *city, size_t city_len)
{
    if (!app_text::output_buffer_available(location, location_len) ||
        !app_text::output_buffer_available(city, city_len)) {
        log_ip_geolocation_warning(kIpLocationInvalidArgLog);
        return false;
    }
    QweatherResponseBuffer response(kIpGeoStage, kIpGeoResponseBufferSize);
    if (!response) {
        return false;
    }
    if (http_get_text(kIpGeolocationUrl, response.get(), response.size()) != ESP_OK) {
        return false;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        return false;
    }
    const cJSON *latitude = cJSON_GetObjectItem(root.get(), kIpGeoJsonLatitudeField);
    const cJSON *longitude = cJSON_GetObjectItem(root.get(), kIpGeoJsonLongitudeField);
    const cJSON *region = cJSON_GetObjectItem(root.get(), kIpGeoJsonRegionField);
    if (!coordinates_available(latitude, longitude)) {
        log_ip_geolocation_warning(kIpLocationMissingCoordinateLog);
        return false;
    }
    if (!format_ip_coordinates(location,
                               location_len,
                               longitude->valuedouble,
                               latitude->valuedouble)) {
        return false;
    }
    const char *region_text = qweather_json_string_value(region);
    if (region_text) {
        copy_ip_region_city(city, city_len, region_text);
    }
    if (city[0] == '\0') {
        strlcpy(city, location, city_len);
    }
    ESP_LOGI(TAG, IP_LOCATION_RESOLVED_FORMAT, location, city);
    return true;
}

void ip_geolocation_cache_invalidate()
{
    s_ip_geolocation_cache_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool ip_geolocation_lookup_cached(char *location,
                                  size_t location_len,
                                  char *city,
                                  size_t city_len)
{
    if (!app_text::output_buffer_available(location, location_len) ||
        !app_text::output_buffer_available(city, city_len)) {
        return ip_geolocation_lookup(location, location_len, city, city_len);
    }
    location[0] = '\0';
    city[0] = '\0';

    char preferred_ssid[kNetworkWifiSsidLen] = {};
    char alternate_ssid[kNetworkWifiSsidLen] = {};
    char current_ssid[kNetworkWifiSsidLen] = {};
    char station_ip[sizeof(s_ip_geolocation_cache.station_ip)] = {};
    const bool has_network_profile = network_wifi_ssid_snapshot(
        preferred_ssid, sizeof(preferred_ssid));
    const bool has_current_network = network_wifi_current_ssid_snapshot(
        current_ssid, sizeof(current_ssid));
    const bool station_ip_available =
        wifi_station_ip_snapshot(station_ip, sizeof(station_ip));
    (void)network_wifi_alternate_ssid_snapshot(alternate_ssid,
                                                sizeof(alternate_ssid));
    const uint32_t generation =
        s_ip_geolocation_cache_generation.load(std::memory_order_acquire);
    const int64_t now_us = esp_timer_get_time();
    const bool cache_key_available = has_network_profile &&
        has_current_network && station_ip_available &&
        preferred_ssid[0] != '\0' && current_ssid[0] != '\0' &&
        station_ip[0] != '\0';
    if (cache_key_available &&
        network_ip_geolocation_cache_matches(
            s_ip_geolocation_cache.valid,
            now_us,
            s_ip_geolocation_cache.expires_at_us,
            s_ip_geolocation_cache.generation,
            generation,
            s_ip_geolocation_cache.preferred_ssid,
            preferred_ssid,
            s_ip_geolocation_cache.alternate_ssid,
            alternate_ssid,
            s_ip_geolocation_cache.current_ssid,
            current_ssid,
            s_ip_geolocation_cache.station_ip,
            station_ip)) {
        const size_t location_size = strlen(s_ip_geolocation_cache.location) + 1;
        const size_t city_size = strlen(s_ip_geolocation_cache.city) + 1;
        if (location_size <= location_len && city_size <= city_len) {
            memcpy(location, s_ip_geolocation_cache.location, location_size);
            memcpy(city, s_ip_geolocation_cache.city, city_size);
            if (generation ==
                s_ip_geolocation_cache_generation.load(std::memory_order_acquire)) {
                return true;
            }
            location[0] = '\0';
            city[0] = '\0';
            return false;
        }
    }

    if (!ip_geolocation_lookup(location, location_len, city, city_len)) {
        return false;
    }
    if (generation !=
        s_ip_geolocation_cache_generation.load(std::memory_order_acquire)) {
        location[0] = '\0';
        city[0] = '\0';
        return false;
    }
    if (cache_key_available &&
        generation == s_ip_geolocation_cache_generation.load(std::memory_order_acquire)) {
        s_ip_geolocation_cache.valid = true;
        s_ip_geolocation_cache.generation = generation;
        s_ip_geolocation_cache.expires_at_us = now_us + kIpGeoLocationCacheTtlUs;
        strlcpy(s_ip_geolocation_cache.preferred_ssid,
                preferred_ssid,
                sizeof(s_ip_geolocation_cache.preferred_ssid));
        strlcpy(s_ip_geolocation_cache.alternate_ssid,
                alternate_ssid,
                sizeof(s_ip_geolocation_cache.alternate_ssid));
        strlcpy(s_ip_geolocation_cache.current_ssid,
                current_ssid,
                sizeof(s_ip_geolocation_cache.current_ssid));
        strlcpy(s_ip_geolocation_cache.station_ip,
                station_ip,
                sizeof(s_ip_geolocation_cache.station_ip));
        strlcpy(s_ip_geolocation_cache.location,
                location,
                sizeof(s_ip_geolocation_cache.location));
        strlcpy(s_ip_geolocation_cache.city,
                city,
                sizeof(s_ip_geolocation_cache.city));
    }
    return true;
}
