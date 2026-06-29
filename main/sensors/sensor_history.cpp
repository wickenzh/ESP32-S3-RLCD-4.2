// 维护本地温湿度趋势和 24 小时历史样本的内存与 NVS 数据。
#include "sensor_services.h"

#include "ui_views.h"

struct HourlySensorHistoryMeta {
    uint32_t magic = kHourlyHistoryMagic;
    uint16_t version = 2;
    uint16_t count = kHourlyHistoryCount;
    int64_t last_saved_at = 0;
};

struct LegacyHourlySensorHistoryBlob {
    uint32_t magic = kHourlyHistoryMagic;
    uint16_t version = 1;
    uint16_t count = kLegacyHourlyHistoryCount;
    HourlySensorSample samples[kLegacyHourlyHistoryCount] = {};
};

bool is_system_time_plausible(struct tm *local_out);
int periodic_sample_minutes(const struct tm &local, int day_minutes, int night_minutes);

namespace {
constexpr const char *kSensorNvsNamespace = "sensor";
constexpr const char *kHourlyHistoryMetaKey = "hourmeta";
constexpr const char *kLegacyHourlyHistoryKey = "hourly24";
constexpr const char *kHourlySlotKeyFormat = "h%02d";
constexpr size_t kHourlySlotKeyBufferSize = 8;
constexpr int kMsPerSecond = 1000;
constexpr int kSecondsPerMinute = 60;
constexpr int kSecondsPerHour = 60 * kSecondsPerMinute;
constexpr int kWeatherSyncFallbackSeconds = kSecondsPerHour;
constexpr int kWeatherSyncSearchHours = 30;
constexpr int kUnknownTimeSensorSampleMs = 60000;
constexpr int kSensorSampleDayMinutes = 1;
constexpr int kSensorSampleNightMinutes = 2;
static_assert(kHourlyHistoryCount <= 99, "hourly slot key format h%02d supports two-digit indexes");
static_assert(kHourlySlotKeyBufferSize >= sizeof("h00"), "hourly slot key buffer must fit hNN plus terminator");

int seconds_until_next_interval(const struct tm &local, int interval_seconds)
{
    if (interval_seconds <= 0) {
        ESP_LOGW(TAG, "sensor interval invalid: %d", interval_seconds);
        return kSecondsPerMinute;
    }
    int seconds_into_hour = local.tm_min * kSecondsPerMinute + local.tm_sec;
    int seconds_to_next = interval_seconds - (seconds_into_hour % interval_seconds);
    if (seconds_to_next <= 0 || seconds_to_next > interval_seconds) {
        seconds_to_next = interval_seconds;
    }
    return seconds_to_next;
}

TickType_t next_periodic_sample_tick(TickType_t now,
                                     int day_minutes,
                                     int night_minutes,
                                     int unknown_time_ms)
{
    struct tm local = {};
    if (!is_system_time_plausible(&local)) {
        return now + pdMS_TO_TICKS(unknown_time_ms);
    }
    int interval_seconds = periodic_sample_minutes(local, day_minutes, night_minutes) * kSecondsPerMinute;
    int seconds_to_next = seconds_until_next_interval(local, interval_seconds);
    return now + pdMS_TO_TICKS(seconds_to_next * kMsPerSecond);
}
} // namespace

static bool hourly_slot_key(int index, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    if (index < 0 || index >= kHourlyHistoryCount) {
        out[0] = '\0';
        ESP_LOGW(TAG, "hourly sensor slot key index invalid: %d", index);
        return false;
    }
    int written = snprintf(out, out_len, kHourlySlotKeyFormat, index);
    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        ESP_LOGW(TAG, "hourly sensor slot key truncated index=%d", index);
        return false;
    }
    return true;
}

int boot_sync_remaining_ms()
{
    if (g_boot_sync_deadline_us <= 0) {
        return INT32_MAX;
    }
    int64_t remaining_us = g_boot_sync_deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    int64_t remaining_ms = remaining_us / 1000;
    return remaining_ms > INT32_MAX ? INT32_MAX : (int)remaining_ms;
}

bool is_system_time_plausible(struct tm *local_out)
{
    time_t now;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    int year = local.tm_year + 1900;
    if (local_out) {
        *local_out = local;
    }
    return year >= kMinValidYear && year <= kMaxValidYear;
}

bool is_tm_plausible(const struct tm &local)
{
    int year = local.tm_year + 1900;
    return year >= kMinValidYear && year <= kMaxValidYear;
}

bool is_night_slow_window(const struct tm &local)
{
    return local.tm_hour >= 22 || local.tm_hour < 6;
}

int periodic_sample_minutes(const struct tm &local, int day_minutes, int night_minutes)
{
    return is_night_slow_window(local) ? night_minutes : day_minutes;
}

time_t hour_start_from_time(time_t value)
{
    struct tm local = {};
    localtime_r(&value, &local);
    local.tm_min = 0;
    local.tm_sec = 0;
    return mktime(&local);
}

void reset_hourly_sensor_history()
{
    memset(&g_hourly_history, 0, sizeof(g_hourly_history));
    g_hourly_history.magic = kHourlyHistoryMagic;
    g_hourly_history.version = 1;
    g_hourly_history.count = kHourlyHistoryCount;
    g_last_hourly_saved_at = 0;
    ++g_hourly_history_version;
}

void load_hourly_sensor_history()
{
    reset_hourly_sensor_history();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kSensorNvsNamespace, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return;
    }
    HourlySensorHistoryMeta meta = {};
    size_t meta_len = sizeof(meta);
    err = nvs_get_blob(nvs, kHourlyHistoryMetaKey, &meta, &meta_len);
    if (err == ESP_OK &&
        meta_len == sizeof(meta) &&
        meta.magic == kHourlyHistoryMagic &&
        meta.version == 2 &&
        meta.count == kHourlyHistoryCount) {
        int loaded = 0;
        int64_t newest_slot = 0;
        for (int i = 0; i < kHourlyHistoryCount; ++i) {
            HourlySensorSample sample = {};
            size_t sample_len = sizeof(sample);
            char key[kHourlySlotKeyBufferSize];
            if (!hourly_slot_key(i, key, sizeof(key))) {
                continue;
            }
            if (nvs_get_blob(nvs, key, &sample, &sample_len) == ESP_OK &&
                sample_len == sizeof(sample)) {
                g_hourly_history.samples[i] = sample;
                if (sample.valid && sample.timestamp > newest_slot) {
                    newest_slot = sample.timestamp;
                }
                ++loaded;
            }
        }
        if (loaded > 0) {
            g_last_hourly_saved_at = newest_slot;
            if (meta.last_saved_at > g_last_hourly_saved_at) {
                g_last_hourly_saved_at = meta.last_saved_at;
            }
            nvs_close(nvs);
            ++g_hourly_history_version;
            return;
        }
    }

    LegacyHourlySensorHistoryBlob legacy = {};
    size_t legacy_len = sizeof(legacy);
    err = nvs_get_blob(nvs, kLegacyHourlyHistoryKey, &legacy, &legacy_len);
    nvs_close(nvs);
    if (err != ESP_OK || legacy_len != sizeof(legacy) ||
        legacy.magic != kHourlyHistoryMagic ||
        legacy.version != 1 ||
        legacy.count != kLegacyHourlyHistoryCount) {
        return;
    }
    for (int i = 0; i < kLegacyHourlyHistoryCount; ++i) {
        const HourlySensorSample &sample = legacy.samples[i];
        g_hourly_history.samples[i] = sample;
        if (sample.valid && sample.timestamp > g_last_hourly_saved_at) {
            g_last_hourly_saved_at = sample.timestamp;
        }
    }
    ++g_hourly_history_version;
}

static bool save_hourly_sensor_slot(int index)
{
    if (index < 0 || index >= kHourlyHistoryCount) {
        ESP_LOGW(TAG, "hourly sensor slot index invalid: %d", index);
        return false;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kSensorNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open sensor nvs failed: %s", esp_err_to_name(err));
        return false;
    }
    HourlySensorHistoryMeta meta = {};
    meta.last_saved_at = g_last_hourly_saved_at;
    err = nvs_set_blob(nvs, kHourlyHistoryMetaKey, &meta, sizeof(meta));
    if (err == ESP_OK) {
        char key[kHourlySlotKeyBufferSize];
        if (hourly_slot_key(index, key, sizeof(key))) {
            err = nvs_set_blob(nvs, key, &g_hourly_history.samples[index], sizeof(g_hourly_history.samples[index]));
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save hourly sensor slot failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void record_hourly_sensor_sample(float temp, float humi)
{
    struct tm local = {};
    if (!is_system_time_plausible(&local)) {
        return;
    }
    time_t now = mktime(&local);
    time_t hour_start = hour_start_from_time(now);
    if (hour_start <= 0 || hour_start == g_last_hourly_saved_at) {
        return;
    }
    int index = (int)((hour_start / kSecondsPerHour) % kHourlyHistoryCount);
    if (index < 0) {
        index += kHourlyHistoryCount;
    }
    g_hourly_history.samples[index].timestamp = hour_start;
    g_hourly_history.samples[index].temperature = temp;
    g_hourly_history.samples[index].humidity = humi;
    g_hourly_history.samples[index].valid = 1;
    g_last_hourly_saved_at = hour_start;
    ++g_hourly_history_version;
    save_hourly_sensor_slot(index);
    notify_ui_task();
}

time_t next_weather_sync_time(time_t from)
{
    struct tm candidate = {};
    localtime_r(&from, &candidate);
    if (!is_tm_plausible(candidate)) {
        return from + kWeatherSyncFallbackSeconds;
    }
    candidate.tm_sec = 0;
    candidate.tm_min = 0;
    candidate.tm_hour += 1;
    time_t next = mktime(&candidate);
    for (int i = 0; i < kWeatherSyncSearchHours; ++i) {
        struct tm local = {};
        localtime_r(&next, &local);
        if (!is_night_slow_window(local) || (local.tm_hour % 2 == 0)) {
            return next;
        }
        local.tm_hour += 1;
        next = mktime(&local);
    }
    return from + kWeatherSyncFallbackSeconds;
}

void update_sensor_history(float temp, float humi)
{
    g_sensor_history[g_sensor_history_next].temperature = temp;
    g_sensor_history[g_sensor_history_next].humidity = humi;
    g_sensor_history_next = (g_sensor_history_next + 1) % kSensorHistoryMinutes;
    if (g_sensor_history_count < kSensorHistoryMinutes) {
        ++g_sensor_history_count;
    }

    float temp_sum = 0.0f;
    float humi_sum = 0.0f;
    for (int i = 0; i < g_sensor_history_count; ++i) {
        temp_sum += g_sensor_history[i].temperature;
        humi_sum += g_sensor_history[i].humidity;
    }
    float temp_avg = temp_sum / g_sensor_history_count;
    float humi_avg = humi_sum / g_sensor_history_count;
    if (g_sensor_average_valid && g_sensor_history_count >= 2) {
        float temp_delta = temp_avg - g_last_temp_average;
        float humi_delta = humi_avg - g_last_humi_average;
        g_temp_trend = temp_delta > kTrendEpsilon ? 1 : (temp_delta < -kTrendEpsilon ? -1 : 0);
        g_humi_trend = humi_delta > kTrendEpsilon ? 1 : (humi_delta < -kTrendEpsilon ? -1 : 0);
    } else {
        g_temp_trend = 0;
        g_humi_trend = 0;
    }
    g_last_temp_average = temp_avg;
    g_last_humi_average = humi_avg;
    g_sensor_average_valid = true;
}

void sample_sensor()
{
    float temp = 0.0f;
    float humi = 0.0f;
    g_sensor_ok = g_shtc3 && g_shtc3->Shtc3_ReadTempHumi(&temp, &humi) == 0;
    if (g_sensor_ok) {
        g_temperature = temp;
        g_humidity = humi;
        update_sensor_history(temp, humi);
        record_hourly_sensor_sample(temp, humi);
    }
}

TickType_t next_sensor_sample_tick(TickType_t now)
{
    return next_periodic_sample_tick(now,
                                     kSensorSampleDayMinutes,
                                     kSensorSampleNightMinutes,
                                     kUnknownTimeSensorSampleMs);
}

TickType_t next_battery_sample_tick(TickType_t now)
{
    return next_periodic_sample_tick(now,
                                     kBatterySampleDayMinutes,
                                     kBatterySampleNightMinutes,
                                     kBatterySampleUnknownTimeMinutes * kSecondsPerMinute * kMsPerSecond);
}
