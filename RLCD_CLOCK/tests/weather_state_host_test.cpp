// 直接验证生产天气状态的静态 mutex、事件发布和并发完整快照。
#include "weather_state_internal.h"
#include "weather_provider.h"

#include "app_metadata.h"

#include "app_event_group.h"

#include <assert.h>
#include <atomic>
#include <string.h>
#include <thread>

const char *const TAG = "WeatherStateHost";

std::atomic<bool> g_fail_weather_mutex_take{false};
std::atomic<int> g_weather_mutex_create_count{0};

namespace {
std::atomic<bool> g_events_ready{false};
std::atomic<int> g_event_set_count{0};
std::atomic<int> g_event_clear_count{0};

void fill_snapshot(char marker,
                   WeatherData *weather,
                   WeatherAlertData *alert,
                   WeatherForecastData *forecast,
                   WeatherAirData *air)
{
    const char text[] = {marker, '\0'};
    strlcpy(weather->city, text, sizeof(weather->city));
    strlcpy(weather->text, text, sizeof(weather->text));
    strlcpy(weather->temp, text, sizeof(weather->temp));
    strlcpy(weather->humidity, text, sizeof(weather->humidity));
    strlcpy(weather->icon, text, sizeof(weather->icon));
    strlcpy(weather->lat, "30.0", sizeof(weather->lat));
    strlcpy(weather->lon, "120.0", sizeof(weather->lon));

    alert->active = true;
    alert->count = 1;
    strlcpy(alert->titles[0], text, sizeof(alert->titles[0]));

    forecast->ready = true;
    forecast->count = 1;
    forecast->days[0].valid = true;
    strlcpy(forecast->days[0].date, text, sizeof(forecast->days[0].date));

    air->ready = true;
    strlcpy(air->aqi, text, sizeof(air->aqi));
}

void assert_snapshot_marker(char marker,
                            const WeatherData &weather,
                            const WeatherAlertData &alert,
                            const WeatherForecastData &forecast,
                            const WeatherAirData &air)
{
    assert(weather.city[0] == marker);
    assert(weather.text[0] == marker);
    assert(weather.temp[0] == marker);
    assert(weather.humidity[0] == marker);
    assert(weather.icon[0] == marker);
    assert(alert.active && alert.count == 1);
    assert(alert.titles[0][0] == marker);
    assert(forecast.ready && forecast.count == 1);
    assert(forecast.days[0].valid && forecast.days[0].date[0] == marker);
    assert(air.ready && air.aqi[0] == marker);
}

void commit_marker(char marker, bool alert_updated = true)
{
    WeatherData weather = {};
    WeatherAlertData alert = {};
    WeatherForecastData forecast = {};
    WeatherAirData air = {};
    fill_snapshot(marker, &weather, &alert, &forecast, &air);
    commit_weather_update_snapshot(
        weather, alert, forecast, air, alert_updated, true, true);
}
} // namespace

bool app_event_group_ready()
{
    return g_events_ready.load(std::memory_order_acquire);
}

EventBits_t app_event_group_set_bits(EventBits_t bits)
{
    assert(bits == kWeatherReadyBit);
    g_event_set_count.fetch_add(1, std::memory_order_release);
    return bits;
}

EventBits_t app_event_group_clear_bits(EventBits_t bits)
{
    assert(bits == kWeatherReadyBit);
    g_event_clear_count.fetch_add(1, std::memory_order_release);
    return 0;
}

int main()
{
    WeatherAlertStatusSnapshot alert_status =
        weather_alert_status_snapshot_load();
    assert(!alert_status.active);
    assert(alert_status.count == 0);
    assert(alert_status.version == 0);
    assert(weather_state_version_load() == 0);
    assert(!weather_ready_state_load());
    WeatherCacheStatusSnapshot cache_status = {};
    assert(!weather_cache_status_snapshot_load(&cache_status));
    assert(cache_status.last_sync_time == 0);
    assert(cache_status.version == 0);
    assert(!cache_status.extended_data_ready);
    assert(!cache_status.forecast_data_ready);
    assert(init_weather_state());
    assert(init_weather_state());
    assert(g_weather_mutex_create_count.load() == 1);
    assert(weather_cache_status_snapshot_load(&cache_status));
    assert(cache_status.last_sync_time == 0);
    assert(cache_status.version == 0);
    assert(!cache_status.extended_data_ready);

    commit_marker('A');
    assert(g_event_set_count.load() == 0);
    assert(!weather_ready_state_load());
    alert_status = weather_alert_status_snapshot_load();
    assert(alert_status.active);
    assert(alert_status.count == 1);
    assert(alert_status.version == 1);
    assert(weather_state_version_load() == alert_status.version);

    g_events_ready.store(true, std::memory_order_release);
    commit_marker('B');
    assert(g_event_set_count.load() == 1);
    assert(weather_ready_state_load());
    alert_status = weather_alert_status_snapshot_load();
    assert(alert_status.active);
    assert(alert_status.count == 1);
    assert(alert_status.version == 2);
    assert(weather_state_version_load() == alert_status.version);
    assert(weather_cache_status_snapshot_load(&cache_status));
    assert(cache_status.last_sync_time > 0);
    assert(cache_status.version == alert_status.version);
    assert(cache_status.extended_data_ready);
    assert(cache_status.forecast_data_ready);

    char alert_title[kWeatherAlertTitleLen] = {};
    assert(get_weather_alert_title_snapshot(-1,
                                            alert_title,
                                            sizeof(alert_title)));
    assert(strcmp(alert_title, "B") == 0);
    assert(!get_weather_alert_title_snapshot(0, nullptr, 0));

    WeatherData weather = {};
    WeatherAlertData alert = {};
    WeatherForecastData forecast = {};
    WeatherAirData air = {};
    assert(get_weather_full_snapshot(&weather, &alert, &forecast, &air));
    assert_snapshot_marker('B', weather, alert, forecast, air);

    commit_marker('C', false);
    assert(get_weather_full_snapshot(&weather, &alert, &forecast, &air));
    assert(weather.city[0] == 'C');
    assert(alert.titles[0][0] == 'B');
    assert(forecast.days[0].date[0] == 'C');
    assert(air.aqi[0] == 'C');
    alert_status = weather_alert_status_snapshot_load();
    assert(alert_status.active);
    assert(alert_status.count == 1);
    assert(alert_status.version == 3);

    clear_weather_ready_event();
    assert(g_event_clear_count.load() == 1);
    assert(!weather_ready_state_load());

    g_fail_weather_mutex_take.store(true, std::memory_order_release);
    commit_marker('A');
    assert(g_event_set_count.load() == 2);
    cache_status = {1, 1, true};
    assert(!weather_cache_status_snapshot_load(&cache_status));
    assert(cache_status.last_sync_time == 1);
    assert(cache_status.version == 1);
    assert(cache_status.extended_data_ready);
    WeatherAlertStatusSnapshot failed_status =
        weather_alert_status_snapshot_load();
    assert(failed_status.version == alert_status.version);
    assert(weather_state_version_load() == failed_status.version);
    assert(!get_weather_alert_title_snapshot(0,
                                             alert_title,
                                             sizeof(alert_title)));
    assert(alert_title[0] == '\0');
    fill_snapshot('Z', &weather, &alert, &forecast, &air);
    assert(!get_weather_full_snapshot(&weather, &alert, &forecast, &air));
    assert_snapshot_marker('Z', weather, alert, forecast, air);
    g_fail_weather_mutex_take.store(false, std::memory_order_release);
    assert(get_weather_full_snapshot(&weather, &alert, &forecast, &air));
    assert(weather.city[0] == 'C');
    assert(alert.titles[0][0] == 'B');
    assert(forecast.days[0].date[0] == 'C');
    assert(air.aqi[0] == 'C');

    commit_marker('B');
    std::atomic<bool> writer_done{false};
    std::thread writer([&]() {
        for (int i = 0; i < 4000; ++i) {
            commit_marker((i & 1) ? 'A' : 'B');
        }
        writer_done.store(true, std::memory_order_release);
    });
    uint32_t last_alert_version = failed_status.version;
    do {
        assert(get_weather_full_snapshot(&weather, &alert, &forecast, &air));
        assert(weather.city[0] == 'A' || weather.city[0] == 'B');
        assert_snapshot_marker(weather.city[0], weather, alert, forecast, air);
        WeatherAlertStatusSnapshot current_status =
            weather_alert_status_snapshot_load();
        assert(current_status.active);
        assert(current_status.count == 1);
        assert(current_status.version >= last_alert_version);
        last_alert_version = current_status.version;
    } while (!writer_done.load(std::memory_order_acquire));
    writer.join();
    const uint32_t old_generation=weather_provider_generation();
    weather_provider_store(WeatherProvider::kOpenMeteo);
    assert(weather_cache_status_snapshot_load(&cache_status));
    assert(cache_status.last_sync_time == 0);
    assert(!cache_status.extended_data_ready);
    assert(!cache_status.forecast_data_ready);
    invalidate_weather_configuration();
    assert(!weather_ready_state_load());
    weather.configuration_generation=old_generation;
    commit_weather_update_snapshot(weather,alert,forecast,air,true,true,true);
    assert(!weather_ready_state_load());
    weather.open_meteo=true;
    weather.configuration_generation=weather_provider_generation();
    commit_weather_update_snapshot(weather,{},forecast,air,true,true,true);
    assert(weather_ready_state_load());
    assert(weather_cache_status_snapshot_load(&cache_status));
    assert(cache_status.last_sync_time > 0);
    const time_t open_meteo_sync_time = cache_status.last_sync_time;
    weather.configuration_generation = old_generation;
    commit_weather_update_snapshot(weather, {}, forecast, air, true, true, true);
    assert(weather_cache_status_snapshot_load(&cache_status));
    assert(cache_status.last_sync_time == open_meteo_sync_time);
    weather_provider_store(WeatherProvider::kQweather);
    assert(weather_cache_status_snapshot_load(&cache_status));
    assert(cache_status.last_sync_time == 0);
    invalidate_weather_configuration();
    weather.open_meteo = false;
    weather.configuration_generation = weather_provider_generation();
    commit_weather_basic_snapshot(weather, {}, true);
    assert(weather_cache_status_snapshot_load(&cache_status));
    assert(cache_status.last_sync_time > 0);
    return 0;
}
