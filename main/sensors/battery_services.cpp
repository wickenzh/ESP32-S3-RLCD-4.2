// 负责电池电压采样、电量估算和充电状态判断。
#include "sensor_services.h"

#include "ui_views.h"

static constexpr float kBatteryVoltageDivider = 3.0f;
static constexpr float kBatteryMillivoltsToVolts = 0.001f;
static constexpr float kBatteryEmptyVoltage = 3.00f;
static constexpr float kBatteryFullVoltage = 4.12f;
static constexpr float kBatteryPercentScale = 100.0f;
static constexpr float kBatteryPercentRoundOffset = 0.5f;
static constexpr adc_unit_t kBatteryAdcUnit = ADC_UNIT_1;
static constexpr adc_channel_t kBatteryAdcChannel = ADC_CHANNEL_3;
static constexpr adc_bitwidth_t kBatteryAdcBitwidth = ADC_BITWIDTH_12;
static constexpr adc_atten_t kBatteryAdcAtten = ADC_ATTEN_DB_12;
static constexpr int kBatteryAdcReferenceMv = 3300;
static constexpr int kBatteryAdcRawMax = 4095;
static constexpr int kBatteryPercentMin = 0;
static constexpr int kBatteryPercentMax = 100;
static constexpr int kBatteryPercentUnknown = -1;

void release_battery_gauge()
{
    if (g_battery_adc_cali_ready && g_battery_adc_cali) {
        esp_err_t err = adc_cali_delete_scheme_curve_fitting(g_battery_adc_cali);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "battery adc calibration release failed: %s", esp_err_to_name(err));
        }
    }
    g_battery_adc_cali = nullptr;
    g_battery_adc_cali_ready = false;

    if (g_battery_adc) {
        esp_err_t err = adc_oneshot_del_unit(g_battery_adc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "battery adc unit release failed: %s", esp_err_to_name(err));
        }
    }
    g_battery_adc = nullptr;
    g_battery_adc_ready = false;
}

bool init_battery_gauge()
{
    if (g_battery_adc_ready) {
        return true;
    }

    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = kBatteryAdcUnit;
    esp_err_t err = adc_oneshot_new_unit(&init_config, &g_battery_adc);
    if (err != ESP_OK) {
        g_battery_adc = nullptr;
        ESP_LOGW(TAG, "battery adc init failed: %s", esp_err_to_name(err));
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.bitwidth = kBatteryAdcBitwidth;
    chan_config.atten = kBatteryAdcAtten;
    err = adc_oneshot_config_channel(g_battery_adc, kBatteryAdcChannel, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery adc channel config failed: %s", esp_err_to_name(err));
        release_battery_gauge();
        return false;
    }

    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = kBatteryAdcUnit;
    cali_config.chan = kBatteryAdcChannel;
    cali_config.atten = kBatteryAdcAtten;
    cali_config.bitwidth = kBatteryAdcBitwidth;
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &g_battery_adc_cali);
    if (err == ESP_OK) {
        g_battery_adc_cali_ready = true;
    } else {
        ESP_LOGW(TAG, "battery adc calibration unavailable: %s", esp_err_to_name(err));
    }

    g_battery_adc_ready = true;
    return true;
}

int battery_percent_from_voltage(float voltage)
{
    int percent = (int)(((voltage - kBatteryEmptyVoltage) * kBatteryPercentScale /
                         (kBatteryFullVoltage - kBatteryEmptyVoltage)) + kBatteryPercentRoundOffset);
    if (percent < kBatteryPercentMin) return kBatteryPercentMin;
    if (percent > kBatteryPercentMax) return kBatteryPercentMax;
    return percent;
}

bool read_battery_percent(int *percent)
{
    if (!percent) {
        ESP_LOGW(TAG, "battery percent output is null");
        return false;
    }
    if (!init_battery_gauge()) {
        return false;
    }

    int raw = 0;
    esp_err_t err = adc_oneshot_read(g_battery_adc, kBatteryAdcChannel, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery adc read failed: %s", esp_err_to_name(err));
        release_battery_gauge();
        return false;
    }

    int adc_mv = (raw * kBatteryAdcReferenceMv) / kBatteryAdcRawMax;
    if (g_battery_adc_cali_ready) {
        err = adc_cali_raw_to_voltage(g_battery_adc_cali, raw, &adc_mv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "battery adc calibration read failed: %s", esp_err_to_name(err));
        }
    }

    float voltage = adc_mv * kBatteryMillivoltsToVolts * kBatteryVoltageDivider;
    int soc = battery_percent_from_voltage(voltage);
    ESP_LOGI(TAG, "battery adc raw=%d adc_mv=%d battery=%.3fV soc=%d%%", raw, adc_mv, voltage, soc);
    *percent = soc;
    g_battery_voltage = voltage;
    return true;
}

void sample_battery()
{
    static int charging_rise_samples = 0;
    int percent = kBatteryPercentUnknown;
    int previous_percent = g_battery_percent;
    float previous_voltage = g_battery_voltage;
    if (read_battery_percent(&percent)) {
        g_battery_percent = percent;
        if (previous_voltage >= 0.0f) {
            float delta = g_battery_voltage - previous_voltage;
            if (delta >= kBatteryChargingRiseVoltage) {
                if (charging_rise_samples < kBatteryChargingRiseSamples) {
                    ++charging_rise_samples;
                }
            } else {
                charging_rise_samples = 0;
            }

            if (g_battery_charging) {
                if (delta <= kBatteryChargingStopVoltage ||
                    (previous_percent >= 0 && percent < previous_percent)) {
                    g_battery_charging = false;
                    charging_rise_samples = 0;
                }
            } else if (charging_rise_samples >= kBatteryChargingRiseSamples) {
                g_battery_charging = true;
            }
        } else {
            charging_rise_samples = 0;
        }
        if (percent >= kBatteryPercentMax) {
            g_battery_charging = false;
            charging_rise_samples = 0;
        }
    } else {
        g_battery_percent = kBatteryPercentUnknown;
        g_battery_charging = false;
        charging_rise_samples = 0;
    }
    update_low_battery_state();
    ++g_battery_version;
    notify_ui_task();
}
