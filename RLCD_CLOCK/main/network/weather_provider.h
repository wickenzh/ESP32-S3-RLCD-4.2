// 定义天气源选择及配置变更代次，旧设备默认使用和风天气。
#pragma once

#include <atomic>
#include <stdint.h>
#include <string.h>

enum class WeatherProvider : uint8_t { kQweather = 0, kOpenMeteo = 1 };

namespace weather_provider_detail {
inline std::atomic<uint32_t> state{0};
}

inline uint32_t weather_provider_generation()
{
    return weather_provider_detail::state.load(std::memory_order_acquire);
}

inline WeatherProvider weather_provider_load()
{
    return static_cast<WeatherProvider>(weather_provider_generation() & 1U);
}

inline void weather_provider_store(WeatherProvider provider)
{
    auto &state = weather_provider_detail::state;
    uint32_t old = state.load(std::memory_order_relaxed);
    const uint32_t selected = provider == WeatherProvider::kOpenMeteo ? 1U : 0U;
    while (!state.compare_exchange_weak(old, ((old + 2U) & ~1U) | selected,
                                        std::memory_order_release,
                                        std::memory_order_relaxed)) {}
}

inline bool parse_weather_provider(const char *text, WeatherProvider *out)
{
    if (!text || !out) return false;
    if (strcmp(text, "qweather") == 0) { *out = WeatherProvider::kQweather; return true; }
    if (strcmp(text, "open_meteo") == 0) { *out = WeatherProvider::kOpenMeteo; return true; }
    return false;
}
