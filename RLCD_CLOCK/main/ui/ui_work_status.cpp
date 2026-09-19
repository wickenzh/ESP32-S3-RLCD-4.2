// 统一构建和刷新非天气时钟工作页顶部状态栏。
#include "ui_work_status.h"
#include "ui_celsius_marker.h"

#include "app_constexpr.h"
#include "app_display_config.h"
#include "app_metadata.h"
#include "local_sensor_state.h"
#include "work_page_ids.h"
#include "ui_bitmap.h"
#include "ui_clock_header_objects.h"
#include "ui_clock_sensor_objects.h"
#include "ui_canvas_primitives.h"
#include "ui_draw_cache.h"
#include "ui_flip_clock.h"
#include "ui_fonts.h"
#include "ui_icons.h"
#include "ui_page_state.h"
#include "ui_status_refresh_policy.h"
#include "ui_text_format.h"
#include "ui_widgets.h"

#include <esp_attr.h>
#include <esp_log.h>

namespace {

static constexpr int kStatusDateX = 198;
static constexpr int kStatusDateY = 15;
static constexpr int kStatusDateW = 182;
static constexpr int kStatusDateH = 26;
static constexpr int kStatusSummaryX = 210;
static constexpr int kStatusSummaryY = 36;
static constexpr int kStatusSummaryW = 98;
static constexpr int kStatusSummaryH = 18;
static constexpr int kStatusTimeX = 318;
static constexpr int kStatusTimeY = 36;
static constexpr int kStatusTimeW = 60;
static constexpr int kStatusTimeH = 18;
static constexpr int kStatusChimeX = 64;
static constexpr int kStatusWifiX = 90;
static constexpr int kStatusAlarmX = 116;
static constexpr int kStatusIconY = 15;
static constexpr int kStatusFirstWorkPage = kWorkPageWeatherClock;
static constexpr int kTrendDrawCacheInvalid = 99;
static constexpr int kStatusTimeCacheInvalid = -1;
static constexpr int kStatusMinutesPerHour = 60;

struct WorkPageStatusIconSlot {
    lv_color_t *buffer;
    lv_obj_t *canvas;
};

struct WorkPageStatusIcons {
    WorkPageStatusIconSlot chime;
    WorkPageStatusIconSlot wifi;
    WorkPageStatusIconSlot alarm;
};

struct WorkPageStatusState {
    WorkPageStatusLabels labels;
    WorkPageStatusIcons icons;
    int last_time_key;
};

EXT_RAM_BSS_ATTR WorkPageStatusState s_work_status_pages[kWorkPageCount] = {};
int s_last_temp_trend_drawn = kTrendDrawCacheInvalid;
int s_last_humi_trend_drawn = kTrendDrawCacheInvalid;
static constexpr const char *kStatusDatePlaceholder = "----/--/-- / 星期-";
static constexpr const char *kStatusSummaryPlaceholder = "-- C --%";
static constexpr size_t kStatusSensorSummaryTextSize = 32;
static constexpr const char *kStatusSensorSummaryFormat = "%.0f C %.0f%%";
static constexpr const char *kStatusSensorSummaryFallback = "-- C --%%";
static constexpr size_t kClockSensorValueTextSize = 32;
static constexpr const char *kClockSensorTempFormat = "%.1f℃";
static constexpr const char *kClockSensorHumidityFormat = "%.1f%%";
static constexpr const char *kClockSensorTempPlaceholder = "--.-℃";
static constexpr const char *kClockSensorHumidityPlaceholder = "--.-%%";
static constexpr const char *kStatusTimePlaceholder = "--:--";
static constexpr size_t kStatusTimeTextSize = 8;
static constexpr const char *kStatusTimeFormat = "%02d:%02d";
#define WORK_STATUS_ICON_INVALID_ARG_LOG "status icon invalid arg"
#define WORK_STATUS_ICON_INVALID_SIZE_FORMAT "status icon invalid size %dx%d row=%d"
#define WORK_STATUS_ICON_CANVAS_CREATE_FAILED_LOG "status icon canvas create failed"
#define WORK_STATUS_DATE_LABEL_CREATE_FAILED_FORMAT "work status date label create failed page=%d"
#define WORK_STATUS_SUMMARY_LABEL_CREATE_FAILED_FORMAT "work status summary label create failed page=%d"
#define WORK_STATUS_TIME_LABEL_CREATE_FAILED_FORMAT "work status time label create failed page=%d"

static_assert(kStatusDateX >= 0 && kStatusDateY >= 0 &&
                  kStatusDateX + kStatusDateW <= kDisplayWidth &&
                  kStatusDateY + kStatusDateH <= kDisplayHeight,
              "work status date label must fit display bounds");
static_assert(kStatusSummaryX >= 0 && kStatusSummaryY >= 0 &&
                  kStatusSummaryX + kStatusSummaryW <= kDisplayWidth &&
                  kStatusSummaryY + kStatusSummaryH <= kDisplayHeight,
              "work status summary label must fit display bounds");
static_assert(kStatusTimeX >= 0 && kStatusTimeY >= 0 &&
                  kStatusTimeX + kStatusTimeW <= kDisplayWidth &&
                  kStatusTimeY + kStatusTimeH <= kDisplayHeight,
              "work status time label must fit display bounds");
static_assert(kStatusChimeX >= 0 && kStatusWifiX >= 0 && kStatusAlarmX >= 0 && kStatusIconY >= 0,
              "work status icon positions must be non-negative");
static_assert(kStatusChimeX + CHIME_STATUS_ICON_WIDTH <= kDisplayWidth &&
                  kStatusWifiX + WIFI_STATUS_ICON_WIDTH <= kDisplayWidth &&
                  kStatusAlarmX + ALARM_STATUS_ICON_WIDTH <= kDisplayWidth &&
                  kStatusIconY + CHIME_STATUS_ICON_HEIGHT <= kDisplayHeight &&
                  kStatusIconY + WIFI_STATUS_ICON_HEIGHT <= kDisplayHeight &&
                  kStatusIconY + ALARM_STATUS_ICON_HEIGHT <= kDisplayHeight,
              "work status icons must fit display bounds");
static_assert(kStatusTimeTextSize >= sizeof("00:00"),
              "work status time buffer must fit HH:MM text");
static_assert(kClockSensorValueTextSize > 1,
              "clock sensor status text buffer must fit text and NUL");
static_assert(cstr_length(kClockSensorTempPlaceholder) + 1 <= kClockSensorValueTextSize,
              "clock sensor temperature placeholder must fit status text buffer");
static_assert(cstr_length(kClockSensorHumidityPlaceholder) + 1 <= kClockSensorValueTextSize,
              "clock sensor humidity placeholder must fit status text buffer");

enum class StatusLabelKind {
    kDate,
    kSummary,
    kTime,
};

bool is_shared_work_status_page(int page)
{
    return page >= kStatusFirstWorkPage && page < kWorkPageCount && page != kWorkPageWeatherClock;
}

void build_status_icon(lv_obj_t *screen,
                       lv_obj_t **canvas,
                       lv_color_t **buffer,
                       int x,
                       int y,
                       int width,
                       int height,
                       int bytes_per_row,
                       const uint8_t *bits)
{
    if (!screen || !canvas || !buffer) {
        ESP_LOGW(TAG, "%s", WORK_STATUS_ICON_INVALID_ARG_LOG);
        return;
    }
    if (width <= 0 || height <= 0 || bytes_per_row <= 0 || !bits) {
        ESP_LOGW(TAG, WORK_STATUS_ICON_INVALID_SIZE_FORMAT, width, height, bytes_per_row);
        return;
    }
    if (!ensure_canvas_buffer(buffer, width, height)) {
        return;
    }
    *canvas = lv_canvas_create(screen);
    if (!*canvas) {
        ESP_LOGW(TAG, "%s", WORK_STATUS_ICON_CANVAS_CREATE_FAILED_LOG);
        return;
    }
    configure_canvas_base(*canvas, *buffer, x, y, width, height);
    draw_1bit_icon(*canvas, width, height, bytes_per_row, bits, lv_color_black(), lv_color_white());
    lv_obj_add_flag(*canvas, LV_OBJ_FLAG_HIDDEN);
}

void log_status_label_create_failed(StatusLabelKind kind, int page)
{
    switch (kind) {
    case StatusLabelKind::kDate:
        ESP_LOGW(TAG, WORK_STATUS_DATE_LABEL_CREATE_FAILED_FORMAT, page);
        break;
    case StatusLabelKind::kSummary:
        ESP_LOGW(TAG, WORK_STATUS_SUMMARY_LABEL_CREATE_FAILED_FORMAT, page);
        break;
    case StatusLabelKind::kTime:
        ESP_LOGW(TAG, WORK_STATUS_TIME_LABEL_CREATE_FAILED_FORMAT, page);
        break;
    }
}

lv_obj_t *make_status_label(lv_obj_t *screen,
                            int page,
                            StatusLabelKind kind,
                            int x,
                            int y,
                            int w,
                            int h,
                            const char *placeholder,
                            const lv_font_t *font)
{
    lv_obj_t *label = make_label_with_font(screen, x, y, w, h, placeholder, font);
    if (!label) {
        log_status_label_create_failed(kind, page);
    }
    return label;
}

bool format_clock_sensor_status_text(char *temp,
                                     size_t temp_len,
                                     char *humi,
                                     size_t humi_len,
                                     int *temperature_trend,
                                     int *humidity_trend)
{
    float temperature = 0.0f;
    float humidity = 0.0f;
    bool sensor_ok = get_local_sensor_snapshot(&temperature,
                                               &humidity,
                                               temperature_trend,
                                               humidity_trend);
    if (sensor_ok) {
        ui_text::format_or_fallback(temp,
                                    temp_len,
                                    kClockSensorTempPlaceholder,
                                    kClockSensorTempFormat,
                                    temperature);
        ui_text::format_or_fallback(humi,
                                    humi_len,
                                    kClockSensorHumidityPlaceholder,
                                    kClockSensorHumidityFormat,
                                    humidity);
    } else {
        ui_text::copy(temp, temp_len, kClockSensorTempPlaceholder);
        ui_text::copy(humi, humi_len, kClockSensorHumidityPlaceholder);
    }
    return sensor_ok;
}

} // namespace

static bool update_work_page_sensor_summary(lv_obj_t *label);
static void style_work_page_sensor_summary(lv_obj_t *label);

void invalidate_work_status_draw_cache()
{
    s_last_temp_trend_drawn = kTrendDrawCacheInvalid;
    s_last_humi_trend_drawn = kTrendDrawCacheInvalid;
    for (WorkPageStatusState &status : s_work_status_pages) {
        status.last_time_key = kStatusTimeCacheInvalid;
    }
}

void invalidate_work_page_status_time_cache(int page)
{
    if (is_shared_work_status_page(page)) {
        s_work_status_pages[page].last_time_key = kStatusTimeCacheInvalid;
    }
}

void clear_work_status_icon_refs()
{
    for (WorkPageStatusState &status : s_work_status_pages) {
        status.icons.chime.canvas = nullptr;
        status.icons.wifi.canvas = nullptr;
        status.icons.alarm.canvas = nullptr;
    }
}

void clear_work_status_label_refs()
{
    for (WorkPageStatusState &status : s_work_status_pages) {
        status.labels = {};
        status.last_time_key = kStatusTimeCacheInvalid;
    }
}

void build_work_page_status_bar(lv_obj_t *screen,
                                int page,
                                bool show_summary,
                                bool show_time)
{
    if (!is_shared_work_status_page(page)) {
        return;
    }
    WorkPageStatusState &status = s_work_status_pages[page];
    WorkPageStatusLabels &labels = status.labels;
    labels = {};
    status.last_time_key = kStatusTimeCacheInvalid;
    if (!screen) {
        return;
    }
    labels.date = make_status_label(screen,
                                    page,
                                    StatusLabelKind::kDate,
                                    kStatusDateX,
                                    kStatusDateY,
                                    kStatusDateW,
                                    kStatusDateH,
                                    kStatusDatePlaceholder,
                                    &zh_font_16);
    if (labels.date) {
        lv_obj_set_style_text_align(labels.date, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    }
    if (show_summary) {
        labels.summary = make_status_label(screen,
                                           page,
                                           StatusLabelKind::kSummary,
                                           kStatusSummaryX,
                                           kStatusSummaryY,
                                           kStatusSummaryW,
                                           kStatusSummaryH,
                                           kStatusSummaryPlaceholder,
                                           &lv_font_montserrat_16);
        if (labels.summary) {
            style_work_page_sensor_summary(labels.summary);
            ui_enable_celsius_marker(labels.summary);
        }
    }
    if (show_time) {
        labels.time = make_status_label(screen,
                                        page,
                                        StatusLabelKind::kTime,
                                        kStatusTimeX,
                                        kStatusTimeY,
                                        kStatusTimeW,
                                        kStatusTimeH,
                                        kStatusTimePlaceholder,
                                        &lv_font_montserrat_16);
        if (labels.time) {
            lv_obj_set_style_text_align(labels.time, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
            lv_obj_set_style_pad_all(labels.time, 0, LV_PART_MAIN);
        }
    }
    build_status_icon(screen,
                      &status.icons.chime.canvas,
                      &status.icons.chime.buffer,
                      kStatusChimeX,
                      kStatusIconY,
                      CHIME_STATUS_ICON_WIDTH,
                      CHIME_STATUS_ICON_HEIGHT,
                      CHIME_STATUS_ICON_BYTES_PER_ROW,
                      chime_status_icon_bits);
    build_status_icon(screen,
                      &status.icons.wifi.canvas,
                      &status.icons.wifi.buffer,
                      kStatusWifiX,
                      kStatusIconY,
                      WIFI_STATUS_ICON_WIDTH,
                      WIFI_STATUS_ICON_HEIGHT,
                      WIFI_STATUS_ICON_BYTES_PER_ROW,
                      wifi_status_icon_bits);
    build_status_icon(screen,
                      &status.icons.alarm.canvas,
                      &status.icons.alarm.buffer,
                      kStatusAlarmX,
                      kStatusIconY,
                      ALARM_STATUS_ICON_WIDTH,
                      ALARM_STATUS_ICON_HEIGHT,
                      ALARM_STATUS_ICON_BYTES_PER_ROW,
                      alarm_status_icon_bits);
}

WorkPageStatusLabels get_work_page_status_labels(int page)
{
    switch (page) {
    case kWorkPageWeatherClock:
        return {clock_header_object_refs().date_label, nullptr, nullptr};
    default:
        return is_shared_work_status_page(page) ? s_work_status_pages[page].labels
                                                : WorkPageStatusLabels{};
    }
}

bool update_work_page_status_time(int page, const struct tm &local)
{
    if (!is_shared_work_status_page(page)) {
        return false;
    }
    WorkPageStatusState &status = s_work_status_pages[page];
    lv_obj_t *label = status.labels.time;
    if (!label) {
        return false;
    }
    const int time_key = local.tm_hour * kStatusMinutesPerHour + local.tm_min;
    if (status.last_time_key == time_key) {
        return false;
    }
    char text[kStatusTimeTextSize] = {};
    ui_text::format_or_fallback(text,
                                sizeof(text),
                                kStatusTimePlaceholder,
                                kStatusTimeFormat,
                                local.tm_hour,
                                local.tm_min);
    const bool changed = set_label_text_if_changed(label, text);
    status.last_time_key = time_key;
    return changed;
}

static bool update_work_page_sensor_summary(lv_obj_t *label)
{
    if (!label) {
        return false;
    }
    char text[kStatusSensorSummaryTextSize] = {};
    float temperature = 0.0f;
    float humidity = 0.0f;
    if (get_local_sensor_snapshot(&temperature, &humidity, nullptr, nullptr)) {
        ui_text::format_or_fallback(text,
                                    sizeof(text),
                                    kStatusSensorSummaryFallback,
                                    kStatusSensorSummaryFormat,
                                    temperature,
                                    humidity);
    } else {
        ui_text::copy(text, sizeof(text), kStatusSensorSummaryFallback);
    }
    return set_label_text_if_changed(label, text);
}

bool update_non_clock_work_page_sensor_status(int page)
{
    if (page == kWorkPageFlipClock) {
        return update_flip_clock_sensor_status();
    }
    return update_work_page_sensor_summary(get_work_page_status_labels(page).summary);
}

bool update_weather_clock_sensor_status()
{
    const ClockLocalSensorObjectRefs &objects = clock_local_sensor_object_refs();
    char temp[kClockSensorValueTextSize] = {};
    char humi[kClockSensorValueTextSize] = {};
    int temperature_trend = 0;
    int humidity_trend = 0;
    bool sensor_ok = format_clock_sensor_status_text(temp,
                                                     sizeof(temp),
                                                     humi,
                                                     sizeof(humi),
                                                     &temperature_trend,
                                                     &humidity_trend);
    bool changed = set_label_text_if_changed(objects.temperature_label, temp);
    changed |= set_label_text_if_changed(objects.humidity_label, humi);
    changed |= update_trend_icon(objects.temperature_trend_canvas,
                                 sensor_ok ? temperature_trend : 0,
                                 &s_last_temp_trend_drawn);
    changed |= update_trend_icon(objects.humidity_trend_canvas,
                                 sensor_ok ? humidity_trend : 0,
                                 &s_last_humi_trend_drawn);
    return changed;
}

static void style_work_page_sensor_summary(lv_obj_t *label)
{
    if (!label) {
        return;
    }
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
}

bool update_work_page_status_icons(int page,
                                   const UiStatusRefreshSnapshot &status,
                                   bool low_battery_mode,
                                   bool setup_active)
{
    if (!is_shared_work_status_page(page)) {
        return false;
    }
    const bool allow = !low_battery_mode && !setup_active;
    const bool chime_visible = allow && status.chime_enabled;
    const bool wifi_visible = allow && status.wifi_radio_on;
    const WorkPageStatusIcons &icons = s_work_status_pages[page].icons;
    lv_obj_t *chime = icons.chime.canvas;
    lv_obj_t *wifi = icons.wifi.canvas;
    lv_obj_t *alarm = icons.alarm.canvas;
    bool changed = false;
    changed |= set_obj_visible(chime, chime_visible);
    changed |= set_obj_visible(wifi, wifi_visible);
    changed |= set_obj_visible(alarm, allow && status.alarm_enabled);
    return changed;
}
