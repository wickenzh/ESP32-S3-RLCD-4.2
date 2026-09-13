// 维护天气数据的一致快照、成功更新时间和 ready 事件发布。
#include "weather_state_internal.h"

#include "app_event_group.h"
#include "app_metadata.h"
#include "scoped_semaphore_lock.h"
#include "weather_snapshot_store.h"

#include <esp_attr.h>
#include "esp_log.h"

#include <atomic>
#include <string.h>

namespace {
StaticTaskMutex s_weather_state_mutex;
EXT_RAM_BSS_ATTR WeatherSnapshotStore s_weather_store;
constexpr uint32_t kWeatherAlertActiveMask = 1U << 0;
constexpr uint32_t kWeatherAlertCountShift = 1;
constexpr uint32_t kWeatherAlertCountMask = 0x7U << kWeatherAlertCountShift;
constexpr uint32_t kWeatherAlertVersionShift = 4;
constexpr uint32_t kWeatherAlertVersionMask = 0x0fffffffU;
std::atomic<uint32_t> s_weather_alert_status{0};
std::atomic<bool> s_weather_ready{false};
#define WEATHER_UPDATED_LOG_FORMAT "weather updated: %s %s %sC %s%% icon=%s forecast=%s air=%s"
#define WEATHER_CURRENT_PUBLISHED_BEFORE_DEFER_FORMAT \
    "weather current published before deferred follow-up: %s %s %sC"
#define WEATHER_BASIC_UPDATED_LOG_FORMAT \
    "weather basic data updated: %s %s %sC %s%% icon=%s"
constexpr const char *kWeatherFetchStatusOk = "ok";
constexpr const char *kWeatherFetchStatusCached = "cached";
constexpr const char *kWeatherReadyEventUnavailableLog =
    "weather ready event skipped: app events unavailable";

static_assert(kMaxWeatherAlerts <= 7,
              "weather alert count must fit the packed status snapshot");
static_assert((kWeatherAlertActiveMask & kWeatherAlertCountMask) == 0,
              "weather alert active and count fields must not overlap");

uint32_t packed_weather_alert_version(uint32_t packed)
{
    return (packed >> kWeatherAlertVersionShift) & kWeatherAlertVersionMask;
}

uint32_t next_weather_alert_version(uint32_t packed)
{
    return (packed_weather_alert_version(packed) + 1U) &
           kWeatherAlertVersionMask;
}

uint32_t pack_weather_alert_status(const WeatherAlertData &alert,
                                   uint32_t version)
{
    int count = alert.count;
    if (count < 0) {
        count = 0;
    } else if (count > kMaxWeatherAlerts) {
        count = kMaxWeatherAlerts;
    }
    return (alert.active ? kWeatherAlertActiveMask : 0U) |
           (static_cast<uint32_t>(count) << kWeatherAlertCountShift) |
           ((version & kWeatherAlertVersionMask) << kWeatherAlertVersionShift);
}

void publish_weather_alert_status_locked()
{
    const uint32_t current =
        s_weather_alert_status.load(std::memory_order_relaxed);
    s_weather_alert_status.store(
        pack_weather_alert_status(s_weather_store.alert,
                                  next_weather_alert_version(current)),
        std::memory_order_release);
}

WeatherAlertStatusSnapshot unpack_weather_alert_status(uint32_t packed)
{
    return {
        (packed & kWeatherAlertActiveMask) != 0,
        static_cast<uint8_t>((packed & kWeatherAlertCountMask) >>
                             kWeatherAlertCountShift),
        packed_weather_alert_version(packed),
    };
}

void publish_weather_ready_event()
{
    if (!app_event_group_ready()) {
        ESP_LOGW(TAG, "%s", kWeatherReadyEventUnavailableLog);
        return;
    }
    s_weather_ready.store(true, std::memory_order_release);
    app_event_group_set_bits(kWeatherReadyBit);
}

bool commit_weather_snapshot(const WeatherData &next,
                             const WeatherAlertData &next_alert,
                             const WeatherForecastData &next_forecast,
                             const WeatherAirData &next_air,
                             bool alert_updated,
                             bool forecast_ok,
                             bool air_ok)
{
    time_t now = 0;
    time(&now);
    {
        ScopedSemaphoreLock lock(s_weather_state_mutex);
        if (!lock) {
            return false;
        }
        weather_snapshot_store_commit(&s_weather_store,
                                      next,
                                      next_alert,
                                      next_forecast,
                                      next_air,
                                      alert_updated,
                                      forecast_ok,
                                      air_ok,
                                      now);
        publish_weather_alert_status_locked();
    }
    publish_weather_ready_event();
    return true;
}

bool commit_basic_weather_snapshot(const WeatherData &next,
                                   const WeatherAlertData &next_alert,
                                   bool alert_updated)
{
    time_t now = 0;
    time(&now);
    {
        ScopedSemaphoreLock lock(s_weather_state_mutex);
        if (!lock) {
            return false;
        }
        weather_snapshot_store_commit_basic(&s_weather_store,
                                            next,
                                            next_alert,
                                            alert_updated,
                                            now);
        publish_weather_alert_status_locked();
    }
    publish_weather_ready_event();
    return true;
}
} // namespace

bool init_weather_state()
{
    return s_weather_state_mutex.init();
}

bool get_weather_full_snapshot(WeatherData *weather,
                               WeatherAlertData *alert,
                               WeatherForecastData *forecast,
                               WeatherAirData *air)
{
    ScopedSemaphoreLock lock(s_weather_state_mutex,pdMS_TO_TICKS(50));
    if (!lock) {
        return false;
    }
    weather_snapshot_store_read(s_weather_store, weather, alert, forecast, air);
    return true;
}

bool get_weather_snapshot(WeatherData *weather)
{
    return get_weather_full_snapshot(weather, nullptr, nullptr, nullptr);
}

WeatherAlertStatusSnapshot weather_alert_status_snapshot_load()
{
    return unpack_weather_alert_status(
        s_weather_alert_status.load(std::memory_order_acquire));
}

uint32_t weather_state_version_load()
{
    return packed_weather_alert_version(
        s_weather_alert_status.load(std::memory_order_acquire));
}

bool weather_ready_state_load()
{
    return s_weather_ready.load(std::memory_order_acquire);
}

bool weather_cache_status_snapshot_load(WeatherCacheStatusSnapshot *out)
{
    if (!out) {
        return false;
    }
    ScopedSemaphoreLock lock(s_weather_state_mutex,pdMS_TO_TICKS(50));
    if (!lock) {
        return false;
    }
    out->last_sync_time = s_weather_store.last_sync_time;
    out->version = packed_weather_alert_version(
        s_weather_alert_status.load(std::memory_order_acquire));
    out->extended_data_ready =
        weather_snapshot_store_extended_ready(s_weather_store);
    out->forecast_data_ready = s_weather_store.forecast.ready &&
        s_weather_store.forecast.count > 0 && s_weather_store.forecast.days[0].valid;
    return true;
}

bool get_weather_alert_title_snapshot(int requested_index,
                                      char *title,
                                      size_t title_len)
{
    if (!title || title_len == 0) {
        return false;
    }
    title[0] = '\0';
    ScopedSemaphoreLock lock(s_weather_state_mutex);
    if (!lock || !s_weather_store.alert.active ||
        s_weather_store.alert.count <= 0 ||
        s_weather_store.alert.count > kMaxWeatherAlerts) {
        return false;
    }
    int index = requested_index < 0 ? 0 : requested_index;
    index %= s_weather_store.alert.count;
    strlcpy(title, s_weather_store.alert.titles[index], title_len);
    return true;
}

void clear_weather_ready_event()
{
    s_weather_ready.store(false, std::memory_order_release);
    if (!app_event_group_ready()) {
        ESP_LOGW(TAG, "%s", kWeatherReadyEventUnavailableLog);
        return;
    }
    app_event_group_clear_bits(kWeatherReadyBit);
}

void commit_weather_update_snapshot(const WeatherData &next,
                                    const WeatherAlertData &next_alert,
                                    const WeatherForecastData &next_forecast,
                                    const WeatherAirData &next_air,
                                    bool alert_updated,
                                    bool forecast_ok,
                                    bool air_ok)
{
    if (!commit_weather_snapshot(next,
                                 next_alert,
                                 next_forecast,
                                 next_air,
                                 alert_updated,
                                 forecast_ok,
                                 air_ok)) {
        return;
    }
    ESP_LOGI(TAG, WEATHER_UPDATED_LOG_FORMAT,
             next.city,
             next.text,
             next.temp,
             next.humidity,
             next.icon,
             forecast_ok ? kWeatherFetchStatusOk : kWeatherFetchStatusCached,
             air_ok ? kWeatherFetchStatusOk : kWeatherFetchStatusCached);
}

void commit_weather_basic_snapshot(const WeatherData &next,
                                   const WeatherAlertData &next_alert,
                                   bool alert_updated)
{
    if (!commit_basic_weather_snapshot(next, next_alert, alert_updated)) {
        return;
    }
    ESP_LOGI(TAG,
             WEATHER_BASIC_UPDATED_LOG_FORMAT,
             next.city,
             next.text,
             next.temp,
             next.humidity,
             next.icon);
}

void commit_weather_resource_deferred_snapshot(
    const WeatherData &next,
    const WeatherAlertData &next_alert,
    const WeatherForecastData &next_forecast,
    bool alert_updated,
    bool forecast_ok)
{
    const WeatherAirData unavailable_air = {};
    if (!commit_weather_snapshot(next,
                                 next_alert,
                                 next_forecast,
                                 unavailable_air,
                                 alert_updated,
                                 forecast_ok,
                                 false)) {
        return;
    }
    ESP_LOGI(TAG,
             WEATHER_CURRENT_PUBLISHED_BEFORE_DEFER_FORMAT,
             next.city,
             next.text,
             next.temp);
}
