// 提供默认关闭的8小时天气WiFi对照实验，按单调时间分组。
#pragma once
#include <stdint.h>
#ifdef WEATHER_CLOCK_WIFI_AB_TEST
#include <atomic>
inline std::atomic<int> weather_wifi_ab_last_slot{-1};
#endif
constexpr int weather_wifi_ab_slot(int64_t uptime_us)
{
    return uptime_us >= 0 && uptime_us < 8LL * 60 * 60 * 1000000
        ? static_cast<int>(uptime_us / (20LL * 60 * 1000000)) : -1;
}
