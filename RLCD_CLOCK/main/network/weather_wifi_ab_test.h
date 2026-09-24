// 提供默认关闭的8小时天气WiFi对照实验，按单调时间分组。
#pragma once
#include <stdint.h>
#ifdef WEATHER_CLOCK_WIFI_AB_TEST
#include <atomic>
inline std::atomic<int> weather_wifi_ab_last_slot{-1};
#endif
#ifndef WEATHER_CLOCK_WIFI_AB_SLOT_MINUTES
#define WEATHER_CLOCK_WIFI_AB_SLOT_MINUTES 20
#endif
inline constexpr int64_t kWeatherWifiAbSlotDurationUs =
    static_cast<int64_t>(WEATHER_CLOCK_WIFI_AB_SLOT_MINUTES) * 60 * 1000000;
inline constexpr int64_t kWeatherWifiAbDurationUs = 8LL * 60 * 60 * 1000000;
static_assert(WEATHER_CLOCK_WIFI_AB_SLOT_MINUTES > 0,
              "weather Wi-Fi A/B slot must be positive");
static_assert(kWeatherWifiAbDurationUs % kWeatherWifiAbSlotDurationUs == 0,
              "weather Wi-Fi A/B duration must contain whole slots");
constexpr int weather_wifi_ab_slot(int64_t uptime_us)
{
    return uptime_us >= 0 && uptime_us < kWeatherWifiAbDurationUs
        ? static_cast<int>(uptime_us / kWeatherWifiAbSlotDurationUs) : -1;
}
