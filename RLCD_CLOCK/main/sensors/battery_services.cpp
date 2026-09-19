// 负责电池电压采样、电量估算和充电状态判断。
#include "sensor_services_internal.h"

#include "app_metadata.h"
#include "app_time_constants.h"
#include "battery_charging_state.h"
#include "battery_policy.h"
#include "battery_adc_filter.h"
#include "battery_runtime_state_internal.h"
#include "network_runtime_events.h"
#include "scoped_nvs_handle.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include <time.h>
#include <atomic>

#define BATTERY_ADC_CALIBRATION_RELEASE_FAILED_LOG_FORMAT "battery adc calibration release failed: %s"
#define BATTERY_ADC_UNIT_RELEASE_FAILED_LOG_FORMAT "battery adc unit release failed: %s"
#define BATTERY_ADC_READY_WITHOUT_HANDLE_LOG_FORMAT "battery adc marked ready without handle, resetting"
#define BATTERY_ADC_CALIBRATION_READY_WITHOUT_HANDLE_LOG "battery adc calibration marked ready without handle, disabling calibration"
#define BATTERY_ADC_INIT_FAILED_LOG_FORMAT "battery adc init failed: %s"
#define BATTERY_ADC_CHANNEL_CONFIG_FAILED_LOG_FORMAT "battery adc channel config failed: %s"
#define BATTERY_ADC_CALIBRATION_UNAVAILABLE_LOG_FORMAT "battery adc calibration unavailable: %s"
#define BATTERY_PERCENT_OUTPUT_NULL_LOG_FORMAT "battery percent output is null"
#define BATTERY_ADC_READ_FAILED_LOG_FORMAT "battery adc read failed: %s"
#define BATTERY_ADC_CALIBRATION_READ_FAILED_LOG_FORMAT "battery adc calibration read failed: %s"
#define BATTERY_ADC_SAMPLE_LOG_FORMAT "battery adc raw=%d adc_mv=%d battery=%.3fV soc=%d%%"
#define BATTERY_CHARGING_STARTED_LOG_FORMAT "battery charging detected voltage=%.3fV soc=%d%%"
#define BATTERY_CHARGING_ANIMATION_COMPLETED_LOG_FORMAT "battery charging animation completed voltage=%.3fV soc=%d%%"
#define BATTERY_CHARGING_STOPPED_LOG_FORMAT "battery charging cleared voltage=%.3fV soc=%d%%"
#define BATTERY_CHARGING_EVIDENCE_LOG_FORMAT \
    "battery charge estimate: previous=%.3fV delta=%.3fV peak_before=%.3fV stop_before=%d"
#define BATTERY_FULL_CHARGE_RECORDED_LOG_FORMAT "battery full charge recorded voltage=%.3fV soc=%d%%"
#define BATTERY_FULL_CHARGE_NVS_OPEN_FAILED_LOG_FORMAT "open battery full-charge history nvs failed: %s"
#define BATTERY_FULL_CHARGE_NVS_READ_FAILED_LOG_FORMAT "read battery full-charge history failed: %s"
#define BATTERY_FULL_CHARGE_NVS_INVALID_LOG_FORMAT "battery full-charge history contains invalid timestamp"
#define BATTERY_FULL_CHARGE_NVS_SAVE_FAILED_LOG_FORMAT "save battery full-charge history failed: %s"
#define BATTERY_CHARGING_ADC_RETRY_LOG_FORMAT "battery ADC failed while charging, preserving state for up to %d retries"
#define BATTERY_CHARGING_ADC_RETRY_EXHAUSTED_LOG_FORMAT "battery ADC charging retry grace exhausted after %d failures"

static adc_oneshot_unit_handle_t s_battery_adc = nullptr;
static adc_cali_handle_t s_battery_adc_cali = nullptr;
static bool s_battery_adc_ready = false;
static bool s_battery_adc_channel_ready = false;
static bool s_battery_adc_cali_ready = false;
static std::atomic<bool> s_charge_confirmation_pending{false};

bool battery_charge_confirmation_pending()
{
    return s_charge_confirmation_pending.load(std::memory_order_relaxed);
}
static constexpr float kBatteryVoltageDivider = 3.0f;
static constexpr float kBatteryMillivoltsToVolts = 0.001f;
static constexpr int kBatteryChargingStopAdcSteps = 2;
static constexpr float kBatteryEmptyVoltage = 3.00f;
static constexpr float kBatteryFullVoltage = 4.12f;
static constexpr float kBatteryVoltageRange = kBatteryFullVoltage - kBatteryEmptyVoltage;
static constexpr float kBatteryPercentScale = 100.0f;
static constexpr float kBatteryPercentRoundOffset = 0.5f;
static constexpr float kBatteryValidPreviousVoltageMin = kBatteryEmptyVoltage;
static constexpr adc_unit_t kBatteryAdcUnit = ADC_UNIT_1;
static constexpr adc_channel_t kBatteryAdcChannel = ADC_CHANNEL_3;
static constexpr adc_bitwidth_t kBatteryAdcBitwidth = ADC_BITWIDTH_12;
static constexpr adc_atten_t kBatteryAdcAtten = ADC_ATTEN_DB_12;
static constexpr int kBatteryAdcReferenceMv = 3300;
static constexpr int kBatteryAdcRawMax = 4095;
static constexpr int kBatteryPercentMin = 0;
static constexpr int kBatteryPercentMax = 100;
static constexpr int kBatteryPercentUnknown = -1;
static constexpr int kBatteryMinValidYear = 2023;
static constexpr int kBatteryMinValidTmYear = kBatteryMinValidYear - kTmYearOffset;
static constexpr const char *kBatteryNvsNamespace = "sensor";
static constexpr const char *kBatteryLastFullChargeKey = "batfull1";
static constexpr uint32_t kBatteryChargingAnimationIdleTicks =
    pdMS_TO_TICKS(kBatteryChargingAnimationIdleMs);
static constexpr uint32_t kBatteryChargeHistoryMinSessionTicks =
    pdMS_TO_TICKS(kBatteryChargeHistoryMinSessionMs);
static constexpr BatteryChargingPolicy kBatteryChargingPolicy = {
    kBatteryValidPreviousVoltageMin,
    kBatteryChargingRiseVoltage,
    kBatteryChargingStopVoltage,
    kBatteryChargingRiseSamples,
    kBatteryChargingStopSamples,
    kBatteryChargingAnimationStopPercent,
    kBatteryChargingAnimationIdleTicks,
    kBatteryChargeHistoryMinSessionTicks,
    pdMS_TO_TICKS(kBatteryChargingConfirmMs),
    pdMS_TO_TICKS(kBatteryChargingConfirmTimeoutMs),
};

struct BatteryReading {
    int percent = kBatteryPercentUnknown;
    float voltage = 0.0f;
};

static_assert(kBatteryVoltageDivider > 0.0f, "battery voltage divider must be positive");
static_assert(kBatteryMillivoltsToVolts > 0.0f, "millivolts-to-volts scale must be positive");
static_assert(kBatteryFullVoltage > kBatteryEmptyVoltage, "battery voltage range must be positive");
static_assert(kBatteryPercentScale > 0.0f, "battery percent scale must be positive");
static_assert(kBatteryPercentRoundOffset >= 0.0f && kBatteryPercentRoundOffset < 1.0f,
              "battery percent rounding offset must stay within one percent step");
static_assert(kBatteryValidPreviousVoltageMin >= kBatteryEmptyVoltage &&
                  kBatteryValidPreviousVoltageMin < kBatteryFullVoltage,
              "battery previous voltage must stay within plausible battery range");
static_assert(kBatteryAdcReferenceMv > 0, "battery ADC reference voltage must be positive");
static_assert(kBatteryAdcRawMax == (1 << 12) - 1, "12-bit battery ADC raw max must stay 4095");
static_assert(kBatteryPercentMin < kBatteryPercentMax, "battery percent range must be ordered");
static_assert(kBatteryPercentUnknown < kBatteryPercentMin, "unknown battery percent must be below valid range");
static_assert(kBatteryMinValidYear > kTmYearOffset, "battery valid year must exceed tm year offset");
static_assert(kBatteryChargingRiseSamples > 0, "charging detection must require at least one rising sample");
static_assert(kBatteryChargingStopSamples > 0,
              "charging stop detection must require at least one confirming sample");
static_assert(kBatteryChargingAnimationStopPercent > kBatteryPercentMin &&
                  kBatteryChargingAnimationStopPercent <= kBatteryPercentMax,
              "charging animation stop percent must stay within the battery range");
static_assert(kBatteryChargingAnimationIdleTicks > 0,
              "charging animation idle tick timeout must be positive");
static_assert(kBatteryChargeHistoryMinSessionTicks >
                  pdMS_TO_TICKS(kBatteryChargingSampleMs),
              "charge history must require multiple fast samples");
static_assert(sizeof(TickType_t) == sizeof(uint32_t),
              "battery charging tracker expects 32-bit FreeRTOS ticks");
static_assert(sizeof(time_t) <= sizeof(int64_t),
              "battery full-charge timestamp must fit NVS int64 storage");
static_assert(kBatteryChargingRiseVoltage > kBatteryChargingStopVoltage,
              "charging rise threshold must stay above stop threshold");
static_assert(kBatteryChargingStopVoltage >=
                  kBatteryVoltageDivider * kBatteryMillivoltsToVolts * kBatteryChargingStopAdcSteps,
              "charging stop threshold must cover at least two scaled ADC millivolt steps");

static int clamp_battery_percent(int percent)
{
    if (percent < kBatteryPercentMin) {
        return kBatteryPercentMin;
    }
    if (percent > kBatteryPercentMax) {
        return kBatteryPercentMax;
    }
    return percent;
}

static bool battery_time_valid(time_t value)
{
    if (value <= 0) {
        return false;
    }
    struct tm local = {};
    return localtime_r(&value, &local) && local.tm_year >= kBatteryMinValidTmYear;
}

static bool current_full_charge_time(time_t *out)
{
    if (!out) {
        return false;
    }
    time_t now = 0;
    time(&now);
    if (!battery_time_valid(now)) {
        return false;
    }
    *out = now;
    return true;
}

static bool save_last_full_charge_time(time_t value)
{
    if (!battery_time_valid(value)) {
        return false;
    }
    app_storage::ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(kBatteryNvsNamespace, NVS_READWRITE);
    if (err == ESP_OK) {
        err = nvs_set_i64(nvs.get(),
                          kBatteryLastFullChargeKey,
                          static_cast<int64_t>(value));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs.get());
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 BATTERY_FULL_CHARGE_NVS_SAVE_FAILED_LOG_FORMAT,
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

void load_battery_charge_history()
{
    app_storage::ScopedNvsHandle nvs;
    esp_err_t err = nvs.open(kBatteryNvsNamespace, NVS_READONLY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 BATTERY_FULL_CHARGE_NVS_OPEN_FAILED_LOG_FORMAT,
                 esp_err_to_name(err));
        return;
    }

    int64_t stored = 0;
    err = nvs_get_i64(nvs.get(), kBatteryLastFullChargeKey, &stored);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 BATTERY_FULL_CHARGE_NVS_READ_FAILED_LOG_FORMAT,
                 esp_err_to_name(err));
        return;
    }
    const time_t loaded = static_cast<time_t>(stored);
    if (!battery_time_valid(loaded)) {
        ESP_LOGW(TAG, "%s", BATTERY_FULL_CHARGE_NVS_INVALID_LOG_FORMAT);
        return;
    }

    BatteryRuntimeSnapshot snapshot;
    if (!battery_runtime_snapshot_load(&snapshot)) {
        return;
    }
    snapshot.last_full_charge_time = loaded;
    battery_runtime_snapshot_store(snapshot);
}

static void release_battery_gauge()
{
    if (s_battery_adc_cali) {
        esp_err_t err = adc_cali_delete_scheme_curve_fitting(s_battery_adc_cali);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, BATTERY_ADC_CALIBRATION_RELEASE_FAILED_LOG_FORMAT, esp_err_to_name(err));
            s_battery_adc_cali_ready = true;
            return;
        }
    }
    s_battery_adc_cali = nullptr;
    s_battery_adc_cali_ready = false;

    if (s_battery_adc) {
        esp_err_t err = adc_oneshot_del_unit(s_battery_adc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, BATTERY_ADC_UNIT_RELEASE_FAILED_LOG_FORMAT, esp_err_to_name(err));
            s_battery_adc_ready = true;
            return;
        }
    }
    s_battery_adc = nullptr;
    s_battery_adc_ready = false;
    s_battery_adc_channel_ready = false;
}

static bool configure_battery_adc_channel()
{
    if (!s_battery_adc) {
        return false;
    }
    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.bitwidth = kBatteryAdcBitwidth;
    chan_config.atten = kBatteryAdcAtten;
    esp_err_t err = adc_oneshot_config_channel(s_battery_adc,
                                                kBatteryAdcChannel,
                                                &chan_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, BATTERY_ADC_CHANNEL_CONFIG_FAILED_LOG_FORMAT, esp_err_to_name(err));
        return false;
    }
    s_battery_adc_channel_ready = true;
    return true;
}

static bool init_battery_gauge()
{
    if (s_battery_adc_ready) {
        if (!s_battery_adc) {
            ESP_LOGW(TAG, BATTERY_ADC_READY_WITHOUT_HANDLE_LOG_FORMAT);
            release_battery_gauge();
            return false;
        }
        if (!s_battery_adc_channel_ready) {
            if (!configure_battery_adc_channel()) {
                release_battery_gauge();
                return false;
            }
        }
        if (s_battery_adc_cali_ready && !s_battery_adc_cali) {
            ESP_LOGW(TAG, "%s", BATTERY_ADC_CALIBRATION_READY_WITHOUT_HANDLE_LOG);
            s_battery_adc_cali_ready = false;
        }
        return true;
    }

    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = kBatteryAdcUnit;
    esp_err_t err = adc_oneshot_new_unit(&init_config, &s_battery_adc);
    if (err != ESP_OK) {
        s_battery_adc = nullptr;
        ESP_LOGW(TAG, BATTERY_ADC_INIT_FAILED_LOG_FORMAT, esp_err_to_name(err));
        return false;
    }

    if (!configure_battery_adc_channel()) {
        release_battery_gauge();
        return false;
    }

    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = kBatteryAdcUnit;
    cali_config.chan = kBatteryAdcChannel;
    cali_config.atten = kBatteryAdcAtten;
    cali_config.bitwidth = kBatteryAdcBitwidth;
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &s_battery_adc_cali);
    if (err == ESP_OK && s_battery_adc_cali) {
        s_battery_adc_cali_ready = true;
    } else {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "%s", BATTERY_ADC_CALIBRATION_READY_WITHOUT_HANDLE_LOG);
        } else {
            ESP_LOGW(TAG, BATTERY_ADC_CALIBRATION_UNAVAILABLE_LOG_FORMAT, esp_err_to_name(err));
        }
        s_battery_adc_cali = nullptr;
        s_battery_adc_cali_ready = false;
    }

    s_battery_adc_ready = true;
    return true;
}

static int battery_percent_from_voltage(float voltage)
{
    int percent = (int)(((voltage - kBatteryEmptyVoltage) * kBatteryPercentScale /
                         kBatteryVoltageRange) + kBatteryPercentRoundOffset);
    return clamp_battery_percent(percent);
}

static int battery_adc_raw_to_mv(int raw)
{
    return (raw * kBatteryAdcReferenceMv) / kBatteryAdcRawMax;
}

static float battery_voltage_from_adc_mv(int adc_mv)
{
    return adc_mv * kBatteryMillivoltsToVolts * kBatteryVoltageDivider;
}

static bool read_battery_reading(BatteryReading *reading)
{
    if (!reading) {
        ESP_LOGW(TAG, BATTERY_PERCENT_OUTPUT_NULL_LOG_FORMAT);
        return false;
    }
    if (!init_battery_gauge()) {
        return false;
    }

    int samples[5] = {};
    esp_err_t err = ESP_OK;
    for (int &sample : samples) {
        err = adc_oneshot_read(s_battery_adc, kBatteryAdcChannel, &sample);
        if (err != ESP_OK || sample < 0 || sample > kBatteryAdcRawMax) {
            if (err == ESP_OK) {
                err = ESP_ERR_INVALID_ARG;
            }
            ESP_LOGW(TAG, BATTERY_ADC_READ_FAILED_LOG_FORMAT, esp_err_to_name(err));
            release_battery_gauge();
            return false;
        }
    }
    const int raw = battery_adc_median(samples);

    int adc_mv = battery_adc_raw_to_mv(raw);
    if (s_battery_adc_cali_ready) {
        err = adc_cali_raw_to_voltage(s_battery_adc_cali, raw, &adc_mv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, BATTERY_ADC_CALIBRATION_READ_FAILED_LOG_FORMAT, esp_err_to_name(err));
        }
    }

    float voltage = battery_voltage_from_adc_mv(adc_mv);
    int soc = battery_percent_from_voltage(voltage);
    ESP_LOGD(TAG, BATTERY_ADC_SAMPLE_LOG_FORMAT, raw, adc_mv, voltage, soc);
    reading->percent = soc;
    reading->voltage = voltage;
    return true;
}

static BatteryChargingState derive_battery_charging_state(
    const BatteryReading &reading,
    float previous_voltage,
    const BatteryChargingState &previous_state,
    BatteryChargingTracker &tracker,
    uint32_t now_tick)
{
    BatteryChargingInput input = {
        previous_voltage,
        reading.voltage,
        reading.percent,
        now_tick,
    };
    BatteryChargingState state = previous_state;
    (void)update_battery_charging_state(input,
                                        kBatteryChargingPolicy,
                                        &tracker,
                                        &state);
    return state;
}

static void apply_battery_reading(const BatteryReading &reading,
                                  const BatteryChargingState &state,
                                  BatteryRuntimeSnapshot *snapshot)
{
    if (!snapshot) {
        return;
    }
    snapshot->percent = reading.percent;
    snapshot->voltage = reading.voltage;
    snapshot->charging = state.charging;
    snapshot->animation_complete = state.animation_complete;
}

static void reset_battery_state_after_sample_failure(BatteryChargingTracker &tracker,
                                                     BatteryRuntimeSnapshot *snapshot)
{
    if (snapshot) {
        snapshot->percent = kBatteryPercentUnknown;
        snapshot->voltage = 0.0f;
        snapshot->charging = false;
        snapshot->animation_complete = false;
    }
    reset_battery_charging_tracker(&tracker);
}

static bool battery_visible_state_changed(const BatteryRuntimeSnapshot &previous,
                                          const BatteryRuntimeSnapshot &next)
{
    return previous.percent != next.percent ||
           previous.charging != next.charging ||
           previous.animation_complete != next.animation_complete ||
           previous.low_battery_mode != next.low_battery_mode;
}

bool sample_battery()
{
    static BatteryChargingTracker charging_tracker;
    static int consecutive_read_failures = 0;
    static bool full_charge_time_pending = false;
    static bool full_charge_session_recorded = false;
    BatteryRuntimeSnapshot previous;
    if (!battery_runtime_snapshot_load(&previous)) {
        return false;
    }
    BatteryRuntimeSnapshot next = previous;
    BatteryReading reading;
    if (read_battery_reading(&reading)) {
        time_t full_charge_time_to_persist = 0;
        consecutive_read_failures = 0;
        uint32_t now_tick = static_cast<uint32_t>(xTaskGetTickCount());
        BatteryChargingState previous_state = {
            previous.charging,
            previous.animation_complete,
        };
        [[maybe_unused]] const float peak_before = charging_tracker.peak_voltage;
        [[maybe_unused]] const int stop_samples_before = charging_tracker.stop_samples;
        BatteryChargingState next_state = derive_battery_charging_state(
            reading,
            previous.voltage,
            previous_state,
            charging_tracker,
            now_tick);
        apply_battery_reading(reading, next_state, &next);
        s_charge_confirmation_pending.store(
            !next_state.charging && charging_tracker.rise_samples > 0,
            std::memory_order_relaxed);
        // Voltage trend is not a USB-present or charger-status signal.
        if (previous.charging != next.charging) {
            ESP_LOGI(TAG, BATTERY_CHARGING_EVIDENCE_LOG_FORMAT,
                     previous.voltage, reading.voltage - previous.voltage,
                     peak_before, stop_samples_before);
        }
        bool completed_charge_session_is_meaningful =
            next_state.charging &&
            battery_charging_session_elapsed(charging_tracker,
                                             now_tick,
                                             kBatteryChargeHistoryMinSessionTicks);
        if (!previous.charging && next.charging) {
            full_charge_time_pending = false;
            full_charge_session_recorded = false;
            ESP_LOGI(TAG, BATTERY_CHARGING_STARTED_LOG_FORMAT, next.voltage, next.percent);
        }
        if (!previous.animation_complete && next.animation_complete) {
            ESP_LOGI(TAG,
                     BATTERY_CHARGING_ANIMATION_COMPLETED_LOG_FORMAT,
                     next.voltage,
                     next.percent);
        }
        // Animation may stop on a plateau below full; that is not a full charge.
        if (battery_full_charge_history_should_update(
                    next.charging,
                    charging_tracker.session_started_below_full_threshold,
                    full_charge_session_recorded,
                    next.percent >= kBatteryChargingAnimationStopPercent,
                    completed_charge_session_is_meaningful)) {
            full_charge_time_pending = true;
            full_charge_session_recorded = true;
        }
        if (full_charge_time_pending && next.charging &&
            next.percent >= kBatteryChargingAnimationStopPercent) {
            time_t recorded_at = 0;
            if (current_full_charge_time(&recorded_at)) {
                next.last_full_charge_time = recorded_at;
                full_charge_time_to_persist = recorded_at;
                full_charge_time_pending = false;
                ESP_LOGI(TAG,
                         BATTERY_FULL_CHARGE_RECORDED_LOG_FORMAT,
                         next.voltage,
                         next.percent);
            }
        }
        if (previous.charging && !next.charging) {
            full_charge_time_pending = false;
            full_charge_session_recorded = false;
            ESP_LOGI(TAG, BATTERY_CHARGING_STOPPED_LOG_FORMAT, next.voltage, next.percent);
        }
        release_battery_gauge();
        if (full_charge_time_to_persist != 0) {
            (void)save_last_full_charge_time(full_charge_time_to_persist);
        }
    } else {
        // A failed ADC batch cannot confirm an earlier voltage excursion.
        s_charge_confirmation_pending.store(false, std::memory_order_relaxed);
        if (!previous.charging) {
            reset_battery_charging_tracker(&charging_tracker);
        }
        if (consecutive_read_failures <= kBatteryReadFailureMaxGraceSamples) {
            ++consecutive_read_failures;
        }
        if (battery_read_failure_within_grace(previous.charging,
                                              consecutive_read_failures)) {
            if (previous.charging && consecutive_read_failures == 1) {
                ESP_LOGI(TAG,
                         BATTERY_CHARGING_ADC_RETRY_LOG_FORMAT,
                         kBatteryChargingReadFailureGraceSamples);
            }
        } else {
            if (previous.charging) {
                ESP_LOGW(TAG,
                         BATTERY_CHARGING_ADC_RETRY_EXHAUSTED_LOG_FORMAT,
                         consecutive_read_failures);
            }
            reset_battery_state_after_sample_failure(charging_tracker, &next);
            full_charge_time_pending = false;
            consecutive_read_failures = 0;
        }
    }
    next.low_battery_mode = battery_low_mode_for_percent(previous.low_battery_mode,
                                                         next.percent,
                                                         kLowBatteryEnterPercent,
                                                         kLowBatteryExitPercent);
    bool state_changed = battery_visible_state_changed(previous, next);
    if (state_changed) {
        ++next.version;
    }
    battery_runtime_snapshot_store(next);
    if (previous.low_battery_mode != next.low_battery_mode) {
        notify_network_sync_runtime_state_changed();
    }
    return state_changed;
}
