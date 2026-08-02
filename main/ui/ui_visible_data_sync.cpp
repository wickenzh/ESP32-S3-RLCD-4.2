// 编排可见工作页的按需联网补拉并刷新天气时钟网络状态。
#include "ui_visible_data_sync.h"

#include "app_constexpr.h"
#include "network_credentials_state.h"
#include "network_services.h"
#include "network_sync_schedule.h"
#include "ota_services.h"
#include "ui_text_format.h"
#include "ui_views.h"
#include "ui_visible_cache.h"

namespace {
constexpr size_t kWeatherCityTextSize = 48;
constexpr size_t kWeatherValueTextSize = 24;
constexpr const char *kWeatherTempFormat = "%s℃";
constexpr const char *kWeatherHumidityFormat = "%s%%";

#define UI_WEATHER_VISIBLE_SYNC_REQUEST_FORMAT "weather clock visible with %s weather, requesting sync"
#define UI_GALLERY_SAYING_SYNC_REQUEST_LOG "gallery visible with missing/stale daily saying, requesting sync"

constexpr const char *kVisibleDataFormatTexts[] = {
    kWeatherTempFormat,
    kWeatherHumidityFormat,
};
constexpr const char *kVisibleDataLogTexts[] = {
    UI_WEATHER_VISIBLE_SYNC_REQUEST_FORMAT,
    UI_GALLERY_SAYING_SYNC_REQUEST_LOG,
};

static_assert(kWeatherCityTextSize > 1,
              "weather city status text buffer must fit text and NUL");
static_assert(kWeatherValueTextSize > 1,
              "weather value status text buffer must fit text and NUL");
static_assert(array_count(kVisibleDataFormatTexts) > 0,
              "visible data format text registry must not be empty");
static_assert(array_count(kVisibleDataLogTexts) > 0,
              "visible data log text registry must not be empty");
static_assert(cstr_array_nonempty(kVisibleDataFormatTexts),
              "visible data format texts must be non-empty");
static_assert(cstr_array_nonempty(kVisibleDataLogTexts),
              "visible data log texts must be non-empty");

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
    bool changed = set_label_text_if_changed(g_weather_city_label, city);
    changed |= set_label_text_if_changed(g_weather_info_label, info);
    changed |= set_label_text_if_changed(g_weather_temp_label, temperature);
    changed |= set_label_text_if_changed(g_weather_humi_label, humidity);
    changed |= set_label_text_if_changed(g_weather_icon_label,
                                         weather_icon_text(icon_code));
    return changed;
}

bool weather_cache_stale(time_t now_value)
{
    return ui_weather_cache_stale(now_value, get_last_weather_sync_time());
}

bool saying_cache_stale(const struct tm &local_value, time_t now_value)
{
    char saying[kDailySayingLen] = {};
    time_t last_sync_time = 0;
    bool snapshot_ready = get_daily_saying_snapshot(saying,
                                                    sizeof(saying),
                                                    &last_sync_time);
    return ui_daily_saying_cache_stale(local_value,
                                       now_value,
                                       snapshot_ready,
                                       last_sync_time);
}

void request_weather_sync_if_needed(VisibleSyncRetryState<TickType_t> &retry,
                                    TickType_t tick_value,
                                    bool sync_in_flight,
                                    const char *reason)
{
    if (!network_visible_auto_sync_allowed(esp_timer_get_time())) {
        retry.reset_request();
        return;
    }
    if (retry.request_if_due(tick_value,
                             sync_in_flight,
                             ota_flow_active(),
                             pdMS_TO_TICKS(kWeatherClockAutoRetryMs),
                             kWeatherClockAutoSyncMaxAttempts,
                             pdMS_TO_TICKS(kWeatherClockAutoBackoffMs))) {
        ESP_LOGI(TAG, UI_WEATHER_VISIBLE_SYNC_REQUEST_FORMAT, reason);
        xEventGroupSetBits(g_app_events, kManualWeatherSyncBit);
    }
}
} // namespace

bool normal_work_page_active(int page)
{
    return active_work_page_load() == page &&
           !battery_low_mode_load() &&
           !setup_portal_active_load();
}

ActiveWorkPageState active_work_page_state(int active_page)
{
    ActiveWorkPageState state = {};
    bool normal_mode = !battery_low_mode_load() && !setup_portal_active_load();
    state.history = normal_mode && active_page == kWorkPageHistory;
    state.gallery = normal_mode && active_page == kWorkPageGallery;
    state.calendar = normal_mode && active_page == kWorkPageCalendar;
    state.weather_board = normal_mode && active_page == kWorkPageWeatherBoard;
    state.radio = normal_mode && active_page == kWorkPageRadio;
    state.xiaozhi = normal_mode && active_page == kWorkPageXiaozhiAI;
    // Low-battery and setup overlays historically retain the weather clock as
    // their active base page; keep that distinction outside normal-page gates.
    state.weather_clock = active_page == kWorkPageWeatherClock;
    state.uses_weather_data = ui_visible_weather_sync_active(normal_mode,
                                                             state.weather_clock,
                                                             state.weather_board);
    return state;
}

void update_visible_weather_sync(const ActiveWorkPageState &state,
                                 time_t now,
                                 TickType_t tick_now,
                                 VisibleSyncRetryState<TickType_t> &retry)
{
    if (!state.uses_weather_data ||
        !network_weather_api_key_configured() ||
        g_offline_mode_ui_enabled ||
        ota_flow_active()) {
        retry.reset_request();
        return;
    }

    EventBits_t sync_bits = xEventGroupGetBits(g_app_events);
    bool weather_ready = (sync_bits & kWeatherReadyBit) != 0;
    bool sync_in_flight =
        (sync_bits & (kManualWeatherSyncBit | kProvisioningSyncBit)) != 0;
    bool details_missing = state.weather_board && !weather_extended_data_ready();
    if (weather_ready && !weather_cache_stale(now) && !details_missing) {
        retry.reset();
        return;
    }
    request_weather_sync_if_needed(retry,
                                   tick_now,
                                   sync_in_flight,
                                   !weather_ready ? "missing"
                                                  : (details_missing ? "incomplete" : "stale"));
}

void update_visible_daily_saying_sync(const ActiveWorkPageState &state,
                                      const struct tm &local,
                                      time_t now,
                                      TickType_t tick_now,
                                      VisibleSyncRetryState<TickType_t> &retry)
{
    bool needs_sync = state.gallery &&
                      !g_offline_mode_ui_enabled &&
                      saying_cache_stale(local, now);
    if (!state.gallery) {
        retry.reset_request();
        return;
    }
    if (!needs_sync) {
        retry.reset();
        return;
    }

    EventBits_t sync_bits = xEventGroupGetBits(g_app_events);
    bool sync_in_flight =
        (sync_bits & (kManualSayingSyncBit | kProvisioningSyncBit)) != 0;
    if (!network_visible_auto_sync_allowed(esp_timer_get_time())) {
        retry.reset_request();
        return;
    }
    if (retry.request_if_due(tick_now,
                             sync_in_flight,
                             ota_flow_active(),
                             pdMS_TO_TICKS(kWeatherClockAutoRetryMs),
                             kWeatherClockAutoSyncMaxAttempts,
                             pdMS_TO_TICKS(kWeatherClockAutoBackoffMs))) {
        ESP_LOGI(TAG, "%s", UI_GALLERY_SAYING_SYNC_REQUEST_LOG);
        xEventGroupSetBits(g_app_events, kManualSayingSyncBit);
    }
}

bool update_weather_clock_network_status(EventBits_t bits,
                                         time_t now,
                                         TickType_t tick_now,
                                         VisibleSyncRetryState<TickType_t> &retry)
{
    if (bits & kWeatherReadyBit) {
        WeatherData weather = {};
        get_weather_snapshot(&weather, nullptr);
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
        bool changed = update_clock_weather_panel_text(city,
                                                       weather.text,
                                                       weather_temp,
                                                       weather_humi,
                                                       weather.icon);
        if (!weather_cache_stale(now)) {
            retry.reset();
        } else if (network_weather_api_key_configured() && !g_offline_mode_ui_enabled) {
            EventBits_t sync_bits = xEventGroupGetBits(g_app_events);
            bool sync_in_flight =
                (sync_bits & (kManualWeatherSyncBit | kProvisioningSyncBit)) != 0;
            request_weather_sync_if_needed(retry,
                                           tick_now,
                                           sync_in_flight,
                                           "stale");
        }
        return changed;
    }

    if (network_weather_api_key_configured() && !g_offline_mode_ui_enabled) {
        EventBits_t sync_bits = xEventGroupGetBits(g_app_events);
        bool sync_in_flight =
            (sync_bits & (kManualWeatherSyncBit | kProvisioningSyncBit)) != 0;
        request_weather_sync_if_needed(retry,
                                       tick_now,
                                       sync_in_flight,
                                       "missing");
        const char *weather_info_text =
            (bits & kWifiConnectedBit) ? kClockWeatherInfoSyncingText
                                       : kClockWeatherInfoWaitingText;
        return update_clock_weather_panel_text(kClockWeatherCityPlaceholder,
                                               weather_info_text,
                                               kClockWeatherTempPlaceholder,
                                               kClockWeatherHumidityPlaceholder,
                                               kClockWeatherUnknownIconCode);
    }

    retry.reset();
    return update_clock_weather_panel_text(
        kClockWeatherCityPlaceholder,
        g_offline_mode_ui_enabled ? kClockWeatherInfoWaitingText
                                  : kClockWeatherInfoMissingApiKeyText,
        kClockWeatherTempPlaceholder,
        kClockWeatherHumidityPlaceholder,
        kClockWeatherUnknownIconCode);
}
