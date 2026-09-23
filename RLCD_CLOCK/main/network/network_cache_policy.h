// 声明天气整点、城市解析与每日文字缓存共用的新鲜度规则。
#pragma once

#include <stdint.h>
#include <time.h>

bool network_cache_age_is_fresh(time_t now,
                                time_t cached_at,
                                time_t max_age);
bool network_weather_cache_current_hour(time_t now, time_t cached_at);
bool network_daily_saying_cache_current_day(time_t now, time_t cached_at);
bool network_weather_city_resolution_cache_matches(
    bool valid,
    int64_t now_us,
    int64_t expires_at_us,
    const char *cached_location,
    const char *current_location,
    const char *city_id);
bool network_ip_geolocation_cache_matches(
    bool valid,
    int64_t now_us,
    int64_t expires_at_us,
    uint32_t cached_generation,
    uint32_t current_generation,
    const char *cached_preferred_ssid,
    const char *current_preferred_ssid,
    const char *cached_alternate_ssid,
    const char *current_alternate_ssid,
    const char *cached_current_ssid,
    const char *current_ssid,
    const char *cached_local_ip,
    const char *current_local_ip);
