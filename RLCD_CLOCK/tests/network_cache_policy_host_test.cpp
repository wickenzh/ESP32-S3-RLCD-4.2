// 验证天气整点与每日文字自然日缓存共用的新鲜度规则。
#include "network_cache_policy.h"

#include <assert.h>
#include <stdlib.h>

namespace {
time_t local_epoch(int year,
                   int month,
                   int day,
                   int hour,
                   int minute,
                   int second)
{
    struct tm value = {};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    value.tm_isdst = -1;
    return mktime(&value);
}
} // namespace

int main()
{
    setenv("TZ", "Asia/Shanghai", 1);
    tzset();

    constexpr time_t kNow = 1000;
    assert(!network_cache_age_is_fresh(kNow, 0, 60));
    assert(!network_cache_age_is_fresh(kNow, kNow - 1, 0));
    assert(!network_cache_age_is_fresh(kNow, kNow + 1, 60));
    assert(network_cache_age_is_fresh(kNow, kNow - 59, 60));
    assert(!network_cache_age_is_fresh(kNow, kNow - 60, 60));

    const time_t weather_sync = local_epoch(2026, 7, 13, 10, 15, 0);
    assert(!network_weather_cache_current_hour(weather_sync, 0));
    assert(network_weather_cache_current_hour(
        local_epoch(2026, 7, 13, 10, 59, 59),
        weather_sync));
    assert(!network_weather_cache_current_hour(
        local_epoch(2026, 7, 13, 11, 0, 0),
        weather_sync));
    assert(!network_weather_cache_current_hour(
        local_epoch(2026, 7, 14, 10, 15, 0),
        weather_sync));
    assert(!network_weather_cache_current_hour(
        local_epoch(2027, 1, 1, 0, 0, 0),
        local_epoch(2026, 12, 31, 23, 59, 59)));

    const time_t saying_sync = local_epoch(2026, 7, 13, 0, 1, 0);
    assert(network_daily_saying_cache_current_day(
        local_epoch(2026, 7, 13, 23, 59, 59),
        saying_sync));
    assert(!network_daily_saying_cache_current_day(
        local_epoch(2026, 7, 14, 0, 0, 0),
        saying_sync));
    assert(!network_daily_saying_cache_current_day(
        local_epoch(2027, 1, 1, 0, 0, 0),
        local_epoch(2026, 12, 31, 23, 59, 59)));

    // Invalid wall-clock years use elapsed-age fallback. Future timestamps
    // remain stale instead of being accepted after a backward clock jump.
    assert(network_weather_cache_current_hour(3600, 1));
    assert(!network_weather_cache_current_hour(3601, 1));
    assert(!network_weather_cache_current_hour(1, 3600));
    assert(network_daily_saying_cache_current_day(86400, 1));
    assert(!network_daily_saying_cache_current_day(86401, 1));
    assert(!network_daily_saying_cache_current_day(1, 86400));

    constexpr int64_t kNowUs = 10'000'000;
    constexpr int64_t kExpiresAtUs = 20'000'000;
    assert(network_weather_city_resolution_cache_matches(
        true,
        kNowUs,
        kExpiresAtUs,
        "120.2999,30.4183",
        "120.2999,30.4183",
        "101210101"));
    assert(!network_weather_city_resolution_cache_matches(
        false,
        kNowUs,
        kExpiresAtUs,
        "120.2999,30.4183",
        "120.2999,30.4183",
        "101210101"));

    assert(network_ip_geolocation_cache_matches(
        true, kNowUs, kExpiresAtUs, 7, 7,
        "Home", "Home", "Guest", "Guest", "Home", "Home",
        "10.0.0.8", "10.0.0.8"));
    assert(!network_ip_geolocation_cache_matches(
        false, kNowUs, kExpiresAtUs, 7, 7,
        "Home", "Home", "Guest", "Guest", "Home", "Home",
        "10.0.0.8", "10.0.0.8"));
    assert(!network_ip_geolocation_cache_matches(
        true, kExpiresAtUs, kExpiresAtUs, 7, 7,
        "Home", "Home", "Guest", "Guest", "Home", "Home",
        "10.0.0.8", "10.0.0.8"));
    assert(!network_ip_geolocation_cache_matches(
        true, kNowUs, kExpiresAtUs, 7, 8,
        "Home", "Home", "Guest", "Guest", "Home", "Home",
        "10.0.0.8", "10.0.0.8"));
    assert(!network_ip_geolocation_cache_matches(
        true, kNowUs, kExpiresAtUs, 7, 7,
        "Home", "Home", "Guest", "Other", "Home", "Home",
        "10.0.0.8", "10.0.0.8"));
    assert(!network_ip_geolocation_cache_matches(
        true, kNowUs, kExpiresAtUs, 7, 7,
        "Home", "Home", "Guest", "Guest", "Home", "Home",
        "10.0.0.8", "10.0.0.9"));
    assert(!network_ip_geolocation_cache_matches(
        true, kNowUs, kExpiresAtUs, 7, 7,
        "Home", "Home", "Guest", "Guest", "Guest", "Home",
        "10.0.0.8", "10.0.0.8"));
    assert(!network_weather_city_resolution_cache_matches(
        true,
        kNowUs,
        kNowUs,
        "120.2999,30.4183",
        "120.2999,30.4183",
        "101210101"));
    assert(!network_weather_city_resolution_cache_matches(
        true,
        kNowUs,
        kExpiresAtUs,
        "120.2999,30.4183",
        "121.4737,31.2304",
        "101210101"));
    assert(!network_weather_city_resolution_cache_matches(
        true,
        kNowUs,
        kExpiresAtUs,
        "",
        "120.2999,30.4183",
        "101210101"));
    assert(!network_weather_city_resolution_cache_matches(
        true,
        kNowUs,
        kExpiresAtUs,
        "120.2999,30.4183",
        "120.2999,30.4183",
        ""));
    assert(!network_weather_city_resolution_cache_matches(
        true,
        -1,
        kExpiresAtUs,
        "120.2999,30.4183",
        "120.2999,30.4183",
        "101210101"));
    assert(!network_weather_city_resolution_cache_matches(
        true,
        kNowUs,
        kExpiresAtUs,
        nullptr,
        "120.2999,30.4183",
        "101210101"));
    assert(!network_weather_city_resolution_cache_matches(
        true,
        kNowUs,
        kExpiresAtUs,
        "120.2999,30.4183",
        nullptr,
        "101210101"));
    return 0;
}
