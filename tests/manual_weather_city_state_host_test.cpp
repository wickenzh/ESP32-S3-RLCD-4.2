// 验证手动天气城市在并发读写时始终提供完整字符串快照。
#include "manual_weather_city_state.h"

#include "app_state.h"

#include <assert.h>
#include <atomic>
#include <string.h>
#include <thread>

int main()
{
    char city[kManualWeatherCityLen] = {};
    assert(!manual_weather_city_snapshot(city, sizeof(city)));
    assert(city[0] == '\0');
    assert(!manual_weather_city_is_configured());

    char too_small[4] = {'x', '\0'};
    assert(!manual_weather_city_snapshot(too_small, sizeof(too_small)));
    assert(too_small[0] == '\0');

    manual_weather_city_store("杭州");
    assert(manual_weather_city_snapshot(city, sizeof(city)));
    assert(strcmp(city, "杭州") == 0);
    assert(manual_weather_city_is_configured());

    constexpr const char *kCityA = "杭州";
    constexpr const char *kCityB = "上海市";
    std::atomic<bool> writer_done{false};
    std::thread writer([&]() {
        for (int i = 0; i < 10000; ++i) {
            manual_weather_city_store((i & 1) ? kCityA : kCityB);
        }
        writer_done.store(true, std::memory_order_release);
    });
    do {
        assert(manual_weather_city_snapshot(city, sizeof(city)));
        assert(strcmp(city, kCityA) == 0 || strcmp(city, kCityB) == 0);
    } while (!writer_done.load(std::memory_order_acquire));
    writer.join();

    manual_weather_city_store(nullptr);
    assert(!manual_weather_city_snapshot(city, sizeof(city)));
    assert(city[0] == '\0');
    assert(!manual_weather_city_is_configured());
    return 0;
}
