// 编排可见工作页的按需联网补拉并刷新天气时钟网络状态。
#include "ui_visible_data_sync.h"

#include "app_event_group.h"
#include "app_metadata.h"
#include "app_runtime_timing.h"
#include "daily_saying_state.h"
#include "network_credentials_state.h"
#include "network_runtime_events.h"
#include "offline_mode_state.h"
#include "network_sync_schedule.h"
#include "ota_services.h"
#include "qweather_icons.h"
#include "qweather_timeout_ab_test.h"
#include "ui_clock.h"
#include "ui_clock_weather_text.h"
#include "ui_text_format.h"
#include "ui_visible_cache.h"
#include "ui_work_page_catalog.h"
#include "weather_state.h"
#include "weather_wifi_ab_test.h"

#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr size_t kWeatherCityTextSize = 48;
constexpr size_t kWeatherValueTextSize = 24;
constexpr const char *kWeatherTempFormat = "%s℃";
constexpr const char *kWeatherHumidityFormat = "%s%%";

#define UI_WEATHER_VISIBLE_SYNC_REQUEST_FORMAT "weather clock visible with %s weather, requesting sync"
#define UI_GALLERY_SAYING_SYNC_REQUEST_LOG "gallery visible with missing/stale daily saying, requesting sync"

static_assert(kWeatherCityTextSize > 1,
              "weather city status text buffer must fit text and NUL");
static_assert(kWeatherValueTextSize > 1,
              "weather value status text buffer must fit text and NUL");

void format_weather_status_text(const WeatherData &weather,
                                char *city,
                                size_t city_len,
                                char *temp,
                                size_t temp_len,
                                char *humi,
                                size_t humi_len)
{
    ui_text::copy(city, city_len, weather.city);
    ui_text::format_or_fallback(temp,
                                temp_len,
                                kClockWeatherTempPlaceholder,
                                kWeatherTempFormat,
                                weather.temp);
    ui_text::format_or_fallback(humi,
                                humi_len,
                                kClockWeatherHumidityPlaceholder,
                                kWeatherHumidityFormat,
                                weather.humidity);
}

bool update_clock_weather_panel_text(const char *city,
                                     const char *info,
                                     const char *temperature,
                                     const char *humidity,
                                     const char *icon_code)
{
    return set_clock_weather_panel_text(city,
                                        info,
                                        temperature,
                                        humidity,
                                        weather_icon_text(icon_code).c_str());
}

bool weather_cache_stale(time_t now_value,
                         const WeatherCacheStatusSnapshot &cache_status)
{
    return ui_weather_cache_stale(now_value, cache_status.last_sync_time);
}

bool saying_cache_stale(time_t now_value,
                        const DailySayingCacheSnapshot *cache_status)
{
    return ui_daily_saying_cache_stale(now_value,
                                       cache_status && cache_status->available,
                                       cache_status ? cache_status->last_sync_time
                                                    : 0);
}

void cancel_visible_sync_request(VisibleSyncRetryState<TickType_t> &retry,
                                 TickType_t tick_now,
                                 EventBits_t request_bit,
                                 bool reset_attempts)
{
    if (retry.requested()) {
        cancel_pending_network_sync_requests(request_bit);
    }
    if (reset_attempts) {
        retry.reset();
    } else {
        retry.cancel_request(tick_now,
                             kWeatherClockAutoSyncMaxAttempts,
                             pdMS_TO_TICKS(kWeatherClockAutoBackoffMs));
    }
}

void request_weather_sync_if_needed(VisibleSyncRetryState<TickType_t> &retry,
                                    TickType_t tick_value,
                                    bool sync_in_flight,
                                    bool ota_active,
                                    const char *reason)
{
    if (!network_visible_auto_sync_allowed(esp_timer_get_time())) {
        cancel_visible_sync_request(retry,
                                    tick_value,
                                    kVisibleWeatherSyncBit,
                                    false);
        return;
    }
    if (retry.request_if_due(tick_value,
                             sync_in_flight,
                             ota_active,
                             pdMS_TO_TICKS(kWeatherClockAutoRetryMs),
                             kWeatherClockAutoSyncMaxAttempts,
                             pdMS_TO_TICKS(kWeatherClockAutoBackoffMs))) {
        ESP_LOGI(TAG, UI_WEATHER_VISIBLE_SYNC_REQUEST_FORMAT, reason);
        app_event_group_set_bits(kVisibleWeatherSyncBit);
    }
}
} // namespace

ActiveWorkPageState active_work_page_state_for_mode(int active_page,
                                                    bool normal_mode)
{
    ActiveWorkPageState state = {};
    state.history = normal_mode && active_page == kWorkPageHistory;
    state.gallery = normal_mode && active_page == kWorkPageGallery;
    state.calendar = normal_mode && active_page == kWorkPageCalendar;
    state.weather_board = normal_mode && active_page == kWorkPageWeatherBoard;
    state.flip_clock = normal_mode && active_page == kWorkPageFlipClock;
    state.xiaozhi = normal_mode && active_page == kWorkPageXiaozhiAI;
    state.aggregate_clock = normal_mode && active_page == kWorkPageAggregateClock;
    // Low-battery and setup overlays historically retain the weather clock as
    // their active base page; keep that distinction outside normal-page gates.
    state.weather_clock = active_page == kWorkPageWeatherClock;
    const WorkPageDataRequirements data_requirements =
        work_page_data_requirements(active_page);
    state.uses_weather_data = normal_mode && data_requirements.weather;
    state.uses_extended_weather_data =
        normal_mode && data_requirements.extended_weather;
    state.uses_daily_saying = normal_mode && data_requirements.daily_saying;
    return state;
}

void update_visible_weather_sync(const ActiveWorkPageState &state,
                                 time_t now,
                                 TickType_t tick_now,
                                 const WeatherCacheStatusSnapshot *cache_status,
                                 VisibleSyncRetryState<TickType_t> &retry)
{
    if (!state.uses_weather_data) {
        cancel_visible_sync_request(retry,
                                    tick_now,
                                    kVisibleWeatherSyncBit,
                                    false);
        return;
    }
    const bool weather_ready = weather_ready_state_load();
    bool details_missing = state.uses_extended_weather_data &&
                           cache_status &&
                           !(state.aggregate_clock ? cache_status->forecast_data_ready
                                                   : cache_status->extended_data_ready);
    bool cache_fresh = weather_ready &&
                       cache_status &&
                       !weather_cache_stale(now, *cache_status) &&
                       !details_missing;
#ifdef WEATHER_CLOCK_WIFI_AB_TEST
    // One requested round per slot; keep existing visibility/safety gates.
    const int ab_slot = weather_wifi_ab_slot(esp_timer_get_time());
    if (ab_slot >= 0) {
        cache_fresh = weather_wifi_ab_last_slot.load() == ab_slot;
    }
#endif
#ifdef WEATHER_CLOCK_QWEATHER_TIMEOUT_AB_TEST
    // One requested round per slot; keep existing visibility/safety gates.
    const int timeout_ab_slot = qweather_timeout_ab_slot(
        esp_timer_get_time());
    if (timeout_ab_slot >= 0) {
        cache_fresh = qweather_timeout_ab_last_slot.load() ==
                      timeout_ab_slot;
    }
#endif
    // The weather-state owner publishes ready together with the EventGroup
    // notification. Stale cache content can survive configuration removal, so
    // only the fully idle steady state can skip the shared request-bit read.
    if (cache_fresh && retry.idle()) {
        return;
    }
    if (!network_wifi_credentials_configured() ||
        !network_weather_configuration_configured() ||
        offline_mode_enabled_load()) {
        cancel_visible_sync_request(retry,
                                    tick_now,
                                    kVisibleWeatherSyncBit,
                                    false);
        return;
    }
    const EventBits_t sync_bits = app_event_group_get_bits();
    bool sync_in_flight =
        (sync_bits & (kManualWeatherSyncBit |
                      kVisibleWeatherSyncBit |
                      kProvisioningSyncBit)) != 0;

    const bool ota_active = ota_flow_active();
    if (ota_active) {
        cancel_visible_sync_request(retry,
                                    tick_now,
                                    kVisibleWeatherSyncBit,
                                    false);
        return;
    }
    if (weather_ready && !cache_status) {
        return;
    }
    if (cache_fresh) {
        cancel_visible_sync_request(retry,
                                    tick_now,
                                    kVisibleWeatherSyncBit,
                                    true);
        return;
    }
    request_weather_sync_if_needed(retry,
                                   tick_now,
                                   sync_in_flight,
                                   ota_active,
                                   !weather_ready ? "missing"
                                                  : (details_missing ? "incomplete" : "stale"));
}

void update_visible_daily_saying_sync(const ActiveWorkPageState &state,
                                      time_t now,
                                      TickType_t tick_now,
                                      const DailySayingCacheSnapshot *cache_status,
                                      VisibleSyncRetryState<TickType_t> &retry)
{
    if (!state.uses_daily_saying) {
        cancel_visible_sync_request(retry,
                                    tick_now,
                                    kVisibleSayingSyncBit,
                                    false);
        return;
    }
    const bool cache_fresh = !saying_cache_stale(now, cache_status);
    if (cache_fresh && retry.idle()) {
        return;
    }
    if (cache_fresh) {
        cancel_visible_sync_request(retry,
                                    tick_now,
                                    kVisibleSayingSyncBit,
                                    true);
        return;
    }
    if (!network_wifi_credentials_configured() ||
        offline_mode_enabled_load()) {
        cancel_visible_sync_request(retry,
                                    tick_now,
                                    kVisibleSayingSyncBit,
                                    false);
        return;
    }

    const bool ota_active = ota_flow_active();
    if (ota_active) {
        cancel_visible_sync_request(retry,
                                    tick_now,
                                    kVisibleSayingSyncBit,
                                    false);
        return;
    }
    EventBits_t sync_bits = app_event_group_get_bits();
    bool sync_in_flight =
        (sync_bits & (kManualSayingSyncBit |
                      kVisibleSayingSyncBit |
                      kProvisioningSyncBit)) != 0;
    if (!network_visible_auto_sync_allowed(esp_timer_get_time())) {
        cancel_visible_sync_request(retry,
                                    tick_now,
                                    kVisibleSayingSyncBit,
                                    false);
        return;
    }
    if (retry.request_if_due(tick_now,
                             sync_in_flight,
                             ota_active,
                             pdMS_TO_TICKS(kWeatherClockAutoRetryMs),
                             kWeatherClockAutoSyncMaxAttempts,
                             pdMS_TO_TICKS(kWeatherClockAutoBackoffMs))) {
        ESP_LOGI(TAG, "%s", UI_GALLERY_SAYING_SYNC_REQUEST_LOG);
        app_event_group_set_bits(kVisibleSayingSyncBit);
    }
}

bool update_weather_clock_network_status(EventBits_t bits)
{
    if (bits & kWeatherReadyBit) {
        WeatherData weather = {};
        if (!get_weather_snapshot(&weather)) {
            return false;
        }
        char city[kWeatherCityTextSize] = {};
        char weather_temp[kWeatherValueTextSize] = {};
        char weather_humi[kWeatherValueTextSize] = {};
        format_weather_status_text(weather,
                                   city,
                                   sizeof(city),
                                   weather_temp,
                                   sizeof(weather_temp),
                                   weather_humi,
                                   sizeof(weather_humi));
        return update_clock_weather_panel_text(city,
                                               weather.text,
                                               weather_temp,
                                               weather_humi,
                                               weather.icon);
    }

    const bool offline_mode = offline_mode_enabled_load();
    if (network_weather_configuration_configured() && !offline_mode) {
        const char *weather_info_text =
            (bits & kWifiConnectedBit) ? kClockWeatherInfoSyncingText
                                       : kClockWeatherInfoWaitingText;
        return update_clock_weather_panel_text(kClockWeatherCityPlaceholder,
                                               weather_info_text,
                                               kClockWeatherTempPlaceholder,
                                               kClockWeatherHumidityPlaceholder,
                                               kClockWeatherUnknownIconCode);
    }

    return update_clock_weather_panel_text(
        kClockWeatherCityPlaceholder,
        offline_mode ? kClockWeatherInfoWaitingText
                     : kClockWeatherInfoMissingApiKeyText,
        kClockWeatherTempPlaceholder,
        kClockWeatherHumidityPlaceholder,
        kClockWeatherUnknownIconCode);
}
