// 刷新温湿时钟的数字牌、日期农历、传感器文本、趋势和舒适度图标。
#include "ui_flip_clock.h"
#include "ui_flip_clock_objects.h"

#include "calendar_lunar.h"
#include "flip_sensor_icons.h"
#include "local_sensor_state.h"
#include "ui_bitmap.h"
#include "ui_clock_time.h"
#include "ui_flip_sensor_mood.h"
#include "ui_inverted_clock_card.h"
#include "ui_icons.h"
#include "ui_page_state.h"
#include "ui_text_format.h"
#include "ui_widgets.h"
#include "work_page_ids.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace {

constexpr int kCardCount = inverted_clock_card::kCount;
constexpr int kHourCardIndex = 0;
constexpr int kMinuteCardIndex = 1;
constexpr int kSecondCardIndex = 2;
constexpr int kTrendDrawCacheInvalid = 99;
constexpr size_t kSensorTextSize = 16;
constexpr size_t kDayTextSize = 8;
constexpr const char *kTempPlaceholder = "--.- C";
constexpr const char *kHumiPlaceholder = "--%";
constexpr const char *kDayPlaceholder = "--";
constexpr const char *kTempFormat = "%.1f C";
constexpr const char *kHumiFormat = "%.0f%%";
constexpr const char *kDayFormat = "%d";

int s_last_hour = -1;
int s_last_minute = -1;
int s_last_second = -1;
int s_last_sensor_minute = -1;
uint32_t s_last_sensor_version = 0;
bool s_sensor_version_valid = false;
int s_last_temp_mood = -1;
int s_last_humi_mood = -1;
int s_last_temp_trend = kTrendDrawCacheInvalid;
int s_last_humi_trend = kTrendDrawCacheInvalid;
int s_last_date_key = -1;

const uint8_t *sensor_mood_icon_bits(int mood,
                                     const uint8_t *comfort_bits,
                                     const uint8_t *ok_bits,
                                     const uint8_t *bad_bits)
{
    if (mood == kSensorMoodComfort) {
        return comfort_bits;
    }
    if (mood == kSensorMoodOk) {
        return ok_bits;
    }
    if (mood == kSensorMoodBad) {
        return bad_bits;
    }
    return nullptr;
}

const uint8_t *temp_mood_icon_bits(int mood)
{
    return sensor_mood_icon_bits(mood,
                                 flip_temp_comfort_icon_bits,
                                 flip_temp_ok_icon_bits,
                                 flip_temp_bad_icon_bits);
}

const uint8_t *humi_mood_icon_bits(int mood)
{
    return sensor_mood_icon_bits(mood,
                                 flip_humi_comfort_icon_bits,
                                 flip_humi_ok_icon_bits,
                                 flip_humi_bad_icon_bits);
}

bool update_mood_icon(lv_obj_t *canvas, int mood, int *last_mood, const uint8_t *bits)
{
    if (!canvas || !last_mood || mood == *last_mood) {
        return false;
    }
    *last_mood = mood;
    if (!bits) {
        lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
        lv_obj_invalidate(canvas);
        return true;
    }
    draw_1bit_icon(canvas,
                   FLIP_SENSOR_ICON_WIDTH,
                   FLIP_SENSOR_ICON_HEIGHT,
                   FLIP_SENSOR_ICON_BYTES_PER_ROW,
                   bits,
                   lv_color_white(),
                   lv_color_black());
    return true;
}

bool update_inverted_trend_icon(lv_obj_t *canvas, int trend, int *last_trend)
{
    if (!canvas || !last_trend || trend == *last_trend) {
        return false;
    }
    *last_trend = trend;
    const uint8_t *bits = trend > 0 ? trend_up_icon_bits : (trend < 0 ? trend_down_icon_bits : nullptr);
    if (bits) {
        draw_1bit_icon_centered(canvas,
                                TREND_ICON_WIDTH,
                                TREND_ICON_HEIGHT,
                                TREND_ICON_BYTES_PER_ROW,
                                bits,
                                lv_color_white(),
                                lv_color_black());
    } else {
        lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
        lv_obj_invalidate(canvas);
    }
    return true;
}

template <typename... Labels>
bool set_text_on_labels(const char *text, Labels... labels)
{
    bool changed = false;
    ((changed |= set_label_text_if_changed(labels, text)), ...);
    return changed;
}

bool update_sensor_text()
{
    const FlipClockObjectRefs &objects = flip_clock_object_refs();
    if (!objects.sensor_label &&
        !objects.sensor_bold_label &&
        !objects.humidity_label &&
        !objects.humidity_bold_label) {
        return false;
    }
    const uint32_t sensor_version = local_sensor_state_version();
    if (s_sensor_version_valid && sensor_version == s_last_sensor_version) {
        return false;
    }
    LocalSensorStateSnapshot sensor;
    if (!local_sensor_state_snapshot_load(&sensor)) {
        return false;
    }
    char temp_text[kSensorTextSize] = {};
    char humi_text[kSensorTextSize] = {};
    if (sensor.available) {
        ui_text::format_or_fallback(temp_text,
                                    sizeof(temp_text),
                                    kTempPlaceholder,
                                    kTempFormat,
                                    sensor.temperature);
        ui_text::format_or_fallback(humi_text,
                                    sizeof(humi_text),
                                    kHumiPlaceholder,
                                    kHumiFormat,
                                    sensor.humidity);
    } else {
        strlcpy(temp_text, kTempPlaceholder, sizeof(temp_text));
        strlcpy(humi_text, kHumiPlaceholder, sizeof(humi_text));
    }
    const int temp_mood = sensor.available
                              ? temperature_mood(sensor.temperature)
                              : kSensorMoodUnavailable;
    const int humi_mood = sensor.available
                              ? humidity_mood(sensor.humidity)
                              : kSensorMoodUnavailable;
    bool changed = false;
    changed |= set_text_on_labels(temp_text,
                                  objects.sensor_label,
                                  objects.sensor_bold_label,
                                  objects.sensor_bold_y_label);
    changed |= set_text_on_labels(humi_text,
                                  objects.humidity_label,
                                  objects.humidity_bold_label,
                                  objects.humidity_bold_y_label);
    changed |= update_mood_icon(objects.temp_mood_canvas,
                                temp_mood,
                                &s_last_temp_mood,
                                temp_mood_icon_bits(temp_mood));
    changed |= update_mood_icon(objects.humi_mood_canvas,
                                humi_mood,
                                &s_last_humi_mood,
                                humi_mood_icon_bits(humi_mood));
    changed |= update_inverted_trend_icon(objects.temp_trend_canvas,
                                          sensor.available ? sensor.temperature_trend : 0,
                                          &s_last_temp_trend);
    changed |= update_inverted_trend_icon(objects.humi_trend_canvas,
                                          sensor.available ? sensor.humidity_trend : 0,
                                          &s_last_humi_trend);
    s_last_sensor_version = sensor.version;
    s_sensor_version_valid = true;
    return changed;
}

bool update_date_text(const struct tm &local)
{
    const FlipClockObjectRefs &objects = flip_clock_object_refs();
    if (!objects.day_label &&
        !objects.day_bold_label &&
        !objects.day_bold_y_label &&
        !objects.lunar_label &&
        !objects.lunar_bold_x_label &&
        !objects.lunar_bold_y_label &&
        !objects.lunar_bold_xy_label) {
        return false;
    }
    CalendarDayInfo info = {};
    const bool lunar_ok = calendar_day_info(local, &info);
    char day_text[kDayTextSize] = {};
    ui_text::format_or_fallback(day_text, sizeof(day_text), kDayPlaceholder, kDayFormat, local.tm_mday);
    const char *lunar_text = lunar_ok && info.subtext[0] ? info.subtext : kDayPlaceholder;
    bool changed = false;
    changed |= set_text_on_labels(day_text,
                                  objects.day_label,
                                  objects.day_bold_label,
                                  objects.day_bold_y_label);
    changed |= set_text_on_labels(lunar_text,
                                  objects.lunar_label,
                                  objects.lunar_bold_x_label,
                                  objects.lunar_bold_y_label,
                                  objects.lunar_bold_xy_label);
    return changed;
}

void reset_refresh_cache()
{
    s_last_hour = -1;
    s_last_minute = -1;
    s_last_second = -1;
    s_last_sensor_minute = -1;
    s_last_sensor_version = 0;
    s_sensor_version_valid = false;
    s_last_temp_mood = -1;
    s_last_humi_mood = -1;
    s_last_temp_trend = kTrendDrawCacheInvalid;
    s_last_humi_trend = kTrendDrawCacheInvalid;
    s_last_date_key = -1;
}

} // namespace

void invalidate_flip_clock_draw_cache()
{
    reset_refresh_cache();
}

void invalidate_flip_clock_time_sensor_draw_cache()
{
    s_last_hour = -1;
    s_last_minute = -1;
    s_last_second = -1;
    s_last_sensor_minute = -1;
    s_last_sensor_version = 0;
    s_sensor_version_valid = false;
}

bool update_flip_clock_sensor_status()
{
    return update_sensor_text();
}

bool update_flip_clock_page(const struct tm &local,
                            const ClockUiTimeSnapshot &time_snapshot)
{
    build_flip_clock_page();
    if (!work_page_root(kWorkPageFlipClock)) {
        return false;
    }
    int last_values[kCardCount] = {
        s_last_hour,
        s_last_minute,
        s_last_second,
    };
    const FlipClockObjectRefs &objects = flip_clock_object_refs();
    bool changed = update_inverted_clock_cards(local,
                                                objects.card_canvas,
                                                last_values);
    s_last_hour = last_values[kHourCardIndex];
    s_last_minute = last_values[kMinuteCardIndex];
    s_last_second = last_values[kSecondCardIndex];

    if (time_snapshot.date_key != s_last_date_key) {
        s_last_date_key = time_snapshot.date_key;
        changed |= update_date_text(local);
    }
    if (local.tm_min != s_last_sensor_minute) {
        s_last_sensor_minute = local.tm_min;
        changed |= update_sensor_text();
    }
    return changed;
}
