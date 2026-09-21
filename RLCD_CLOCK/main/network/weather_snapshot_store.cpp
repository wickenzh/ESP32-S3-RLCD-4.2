// 实现天气快照读取、扩展请求失败保留和基础同步失效规则。
#include "weather_snapshot_store.h"

#include <string.h>

namespace {

bool cstr_has_text(const char *text)
{
    return text && text[0] != '\0';
}

bool weather_coordinate_pair_available(const WeatherData &weather)
{
    return cstr_has_text(weather.lat) && cstr_has_text(weather.lon);
}

bool weather_location_matches(const WeatherData &previous,
                              const WeatherData &next)
{
    if (previous.open_meteo != next.open_meteo) return false;
    const bool previous_coordinates_available =
        weather_coordinate_pair_available(previous);
    const bool next_coordinates_available =
        weather_coordinate_pair_available(next);
    if (previous_coordinates_available || next_coordinates_available) {
        return previous_coordinates_available &&
               next_coordinates_available &&
               strcmp(previous.lat, next.lat) == 0 &&
               strcmp(previous.lon, next.lon) == 0;
    }
    return cstr_has_text(previous.city) &&
           cstr_has_text(next.city) &&
           strcmp(previous.city, next.city) == 0;
}

} // namespace

void weather_snapshot_store_read(const WeatherSnapshotStore &store,
                                 WeatherData *weather,
                                 WeatherAlertData *alert,
                                 WeatherForecastData *forecast,
                                 WeatherAirData *air)
{
    if (weather) {
        *weather = store.weather;
    }
    if (alert) {
        *alert = store.alert;
    }
    if (forecast) {
        *forecast = store.forecast;
    }
    if (air) {
        *air = store.air;
    }
}

bool weather_snapshot_store_extended_ready(const WeatherSnapshotStore &store)
{
    return !store.extended_refresh_required &&
           store.forecast.ready &&
           store.forecast.count > 0 &&
           store.forecast.days[0].valid &&
           store.air.ready;
}

void weather_snapshot_store_commit(WeatherSnapshotStore *store,
                                   const WeatherData &next,
                                   const WeatherAlertData &next_alert,
                                   const WeatherForecastData &next_forecast,
                                   const WeatherAirData &next_air,
                                   bool alert_updated,
                                   bool forecast_ok,
                                   bool air_ok,
                                   time_t synced_at)
{
    if (!store) {
        return;
    }
    const bool same_location =
        weather_location_matches(store->weather, next);
    store->weather = next;
    if (alert_updated) {
        store->alert = next_alert;
    } else if (!same_location) {
        store->alert = {};
    }
    if (forecast_ok) {
        store->forecast = next_forecast;
    } else if (!same_location) {
        store->forecast = {};
    }
    if (air_ok) {
        store->air = next_air;
    } else if (!same_location) {
        store->air = {};
    }
    if (forecast_ok && air_ok) {
        store->extended_refresh_required = false;
    }
    store->last_sync_time = synced_at;
}

void weather_snapshot_store_commit_basic(WeatherSnapshotStore *store,
                                         const WeatherData &next,
                                         const WeatherAlertData &next_alert,
                                         bool alert_updated,
                                         time_t synced_at)
{
    if (!store) {
        return;
    }
    const bool same_location =
        weather_location_matches(store->weather, next);
    store->weather = next;
    if (alert_updated) {
        store->alert = next_alert;
    } else if (!same_location) {
        store->alert = {};
    }
    if (!same_location) {
        store->forecast = {};
        store->air = {};
    }
    store->extended_refresh_required = true;
    store->last_sync_time = synced_at;
}
