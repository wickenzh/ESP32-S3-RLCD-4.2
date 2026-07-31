// 声明天气时钟全局状态、常量、数据结构和跨模块共享对象。
#pragma once
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>

#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "miniz.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

#include "display_bsp.h"
#include "active_work_page_state.h"
#include "battery_runtime_state.h"
#include "i2c_bsp.h"
#include "i2c_equipment.h"
#include "lvgl_bsp.h"
#include "dseg_digits.h"
#include "network_diagnostics_catalog.h"
#include "ota_flow_policy.h"
#include "status_gif_60.h"
#include "ui_icons.h"

LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_font_16);
LV_FONT_DECLARE(zh_flip_lunar_22);
LV_FONT_DECLARE(zh_pomodoro_title_24);

extern const char *const TAG;
extern const char *const APP_VERSION;
extern const char *const APP_BUILD_DATE;

inline constexpr int kDisplayWidth = 400;
inline constexpr int kDisplayHeight = 300;
inline constexpr int kWifiConnectedBit = BIT0;
inline constexpr int kTimeSyncedBit = BIT1;
inline constexpr int kWeatherReadyBit = BIT2;
inline constexpr int kProvisioningSyncBit = BIT3;
inline constexpr int kManualNtpSyncBit = BIT4;
inline constexpr int kManualWeatherSyncBit = BIT5;
inline constexpr int kOtaCheckBit = BIT6;
inline constexpr int kOtaInstallBit = BIT7;
inline constexpr int kManualSayingSyncBit = BIT8;
inline constexpr int kBootSyncDoneBit = BIT9;
inline constexpr int kBootAnimDoneBit = BIT10;
inline constexpr int kNetworkDiagBit = BIT11;
// Wakes the network task when runtime configuration changes. This is not a
// sync request and must never be cleared with the request-bit group.
inline constexpr int kNetworkStateChangedBit = BIT12;
inline constexpr gpio_num_t kBootButtonGpio = GPIO_NUM_0;
inline constexpr gpio_num_t kKeyButtonGpio = GPIO_NUM_18;
inline constexpr const char *kSetupApPassword = "12345678";
inline constexpr const char *kSetupPortalIp = "192.168.4.1";
inline constexpr const char *kSetupPortalUrl = "http://192.168.4.1/";
inline constexpr int kAppMsPerSecond = 1000;
inline constexpr int kAppSecondsPerMinute = 60;
inline constexpr int kAppMsPerMinute = kAppSecondsPerMinute * kAppMsPerSecond;
inline constexpr int kSettingsTimeoutMs = 30 * kAppMsPerSecond;
inline constexpr int kXiaozhiAutoReturnTimeoutMs = 5 * kAppMsPerMinute;
inline constexpr int kSettingsPrimaryCount = 4;
inline constexpr int kSettingsSecondaryMaxCount = 7;
inline constexpr int kSettingsLabelCount = kSettingsPrimaryCount + kSettingsSecondaryMaxCount;
inline constexpr int kChimeSoundCount = 4;
inline constexpr int kNetworkSettingsNtpItem = 0;
inline constexpr int kNetworkSettingsWeatherItem = 1;
inline constexpr int kNetworkSettingsSayingItem = 2;
inline constexpr int kNetworkSettingsWeatherCityItem = 3;
inline constexpr int kNetworkSettingsSecondaryCount = kNetworkSettingsWeatherCityItem + 1;
inline constexpr int kSoundSettingsVolumeItem = 0;
inline constexpr int kSoundSettingsSoundItem = 1;
inline constexpr int kSoundSettingsHourlyItem = 2;
inline constexpr int kSoundSettingsAllDayItem = 3;
inline constexpr int kSoundSettingsSecondaryCount = kSoundSettingsAllDayItem + 1;
inline constexpr int kWorkPageWeatherClock = 0;
inline constexpr int kWorkPageGallery = 1;
inline constexpr int kWorkPageWeatherBoard = 2;
inline constexpr int kWorkPageFlipClock = 3;
inline constexpr int kWorkPageCalendar = 4;
inline constexpr int kWorkPageHistory = 5;
inline constexpr int kWorkPageXiaozhiAI = 6;
inline constexpr int kWorkPageCount = 7;
// 保留页面索引映射容量，供内部页面状态 helper 校验使用。
inline constexpr int kDisplaySettingsPageItemCount = kWorkPageCount;
inline constexpr int kDisplaySettingsPageSwitchItem = 0;
inline constexpr int kDisplaySettingsOrderItem = 1;
inline constexpr int kDisplaySettingsAlarmItem = 2;
inline constexpr int kDisplaySettingsXiaozhiAutoReturnItem = 3;
inline constexpr int kDisplaySettingsSecondaryCount = kDisplaySettingsXiaozhiAutoReturnItem + 1;
inline constexpr int kSystemSettingsOfflineItem = 0;
inline constexpr int kSystemSettingsNetworkDiagItem = 1;
inline constexpr int kSystemSettingsFactoryResetItem = 2;
inline constexpr int kSystemSettingsInfoItem = 3;
inline constexpr int kSystemSettingsOtaItem = 4;
inline constexpr int kSystemSettingsGridItemCount = 4;
inline constexpr int kSystemSettingsSecondaryCount = kSystemSettingsOtaItem + 1;
inline constexpr int kSettingsManualSyncTimeoutMs = kAppMsPerMinute;
inline constexpr int kWeatherClockAutoRetryMs = 2 * kAppMsPerMinute;
inline constexpr int kWeatherClockAutoSyncMaxAttempts = 3;
inline constexpr int kWeatherClockAutoBackoffMs = 30 * kAppMsPerMinute;
inline constexpr int kButtonIdlePollMs = 60;
inline constexpr int kButtonLowRefreshIdlePollMs = 50;
inline constexpr int kButtonActivePollMs = 50;
inline constexpr int kButtonPressedPollMs = 20;
inline constexpr int kBootAnimRunFrameMs = 50;
inline constexpr int kBootWifiConnectTimeoutMs = 5 * kAppMsPerSecond;
inline constexpr int kBootNtpRetries = 2;
inline constexpr int kBootStartupBudgetMs = 6 * kAppMsPerSecond;
inline constexpr int kHttpBootTimeoutMs = 2500;
inline constexpr int kMinValidYear = 2024;
inline constexpr int kMaxValidYear = 2035;
inline constexpr int kLowBatteryEnterPercent = 10;
inline constexpr int kLowBatteryExitPercent = 13;
inline constexpr int kDisplayPartialMaxWidth = (kDisplayWidth * 7) / 10;
inline constexpr int kMaxFlushRanges = 8;
inline constexpr int kFlushRangeMergeGap = 8;
inline constexpr int kDisplayFlushDiagIntervalMs = kAppMsPerMinute;
inline constexpr int kSensorHistoryMinutes = 240;
inline constexpr int kHourlyHistoryCount = 48;
inline constexpr int kLegacyHourlyHistoryCount = 24;
inline constexpr uint32_t kHourlyHistoryMagic = 0x48543234;
inline constexpr int kMaxWeatherAlerts = 6;
inline constexpr int kWeatherAlertTitleLen = 64;
inline constexpr int kWeatherForecastDays = 6;
inline constexpr int kWeatherAdviceLen = 96;
inline constexpr int kManualWeatherCityLen = 32;
inline constexpr int kDailySayingLen = 160;
inline constexpr float kTrendEpsilon = 0.01f;
inline constexpr const char *kDailySayingUrl = "https://uapis.cn/api/v1/saying";
#ifndef WEATHER_CLOCK_OTA_MANIFEST_URL
#if __has_include("ota_endpoint_local.h")
#include "ota_endpoint_local.h"
#else
#define WEATHER_CLOCK_OTA_MANIFEST_URL "https://example.invalid/firmware/latest.json"
#endif
#endif
inline constexpr const char *kOtaManifestUrl = WEATHER_CLOCK_OTA_MANIFEST_URL;
#ifndef WEATHER_CLOCK_OTA_BACKUP_MANIFEST_URL
#define WEATHER_CLOCK_OTA_BACKUP_MANIFEST_URL "https://example.invalid/firmware/latest.json"
#endif
inline constexpr const char *kOtaBackupManifestUrl = WEATHER_CLOCK_OTA_BACKUP_MANIFEST_URL;
inline constexpr int kOtaVersionLen = 24;
inline constexpr int kOtaUrlLen = 256;
inline constexpr int kOtaSha256Len = 65;
inline constexpr int kOtaHttpTimeoutMs = 8 * kAppMsPerSecond;
inline constexpr int kOtaNoProgressTimeoutMs = 45 * kAppMsPerSecond;
inline constexpr int kOtaMaxDownloadMs = 10 * kAppMsPerMinute;
inline constexpr int kOtaStatusMinIntervalMs = 3 * kAppMsPerSecond;
inline constexpr int kOtaAvailableConfirmTimeoutMs = kAppMsPerMinute;
inline constexpr int kOtaDownloadBufferSize = 4096;
inline constexpr int kOtaChunkDelayMs = 25;
inline constexpr float kBatteryChargingRiseVoltage = 0.035f;
inline constexpr float kBatteryChargingStopVoltage = 0.006f;
inline constexpr int kBatteryChargingRiseSamples = 1;
inline constexpr int kBatteryChargingStopSamples = 5;
inline constexpr int kBatteryChargingAnimationStopPercent = 96;
inline constexpr int kBatteryChargingAnimationIdleMs = 10 * kAppMsPerMinute;
inline constexpr int kBatteryChargingSampleMs = kAppMsPerSecond;

static_assert(kWorkPageCount == kWorkPageXiaozhiAI + 1, "work page count must match the last work page id");
static_assert(kXiaozhiAutoReturnTimeoutMs > 0, "Xiaozhi auto-return timeout must be positive");
static_assert(kDisplaySettingsPageItemCount == kWorkPageCount, "display page setting count must match work page count");
static_assert(kDisplaySettingsAlarmItem + 1 == kDisplaySettingsXiaozhiAutoReturnItem,
              "alarm item must remain immediately above Xiaozhi auto return");
static_assert(kDisplaySettingsSecondaryCount == kDisplaySettingsXiaozhiAutoReturnItem + 1,
              "display settings count must include the Xiaozhi auto-return item");

enum SettingsSyncOp {
    kSettingsSyncNone = 0,
    kSettingsSyncNtp = 1,
    kSettingsSyncWeather = 2,
    kSettingsSyncSaying = 3,
    kSettingsSyncNetworkDiag = 4,
};

enum NetworkDiagState {
    kNetworkDiagIdle = 0,
    kNetworkDiagRunning = 1,
    kNetworkDiagDone = 2,
};

enum SettingsPrimaryMenu {
    kSettingsPrimaryNetwork = 0,
    kSettingsPrimarySound = 1,
    kSettingsPrimaryDisplay = 2,
    kSettingsPrimarySystem = 3,
};

extern DisplayPort g_display;
extern I2cMasterBus g_i2c;
extern Shtc3Port *g_shtc3;
extern EventGroupHandle_t g_app_events;
extern char g_ap_ssid[33];
extern std::atomic<bool> g_hourly_chime_enabled;
extern std::atomic<bool> g_hourly_chime_all_day;
extern std::atomic<bool> g_offline_mode_ui_enabled;
extern std::atomic<bool> g_xiaozhi_auto_return_enabled;
extern std::atomic<int> g_chime_volume_percent;
extern std::atomic<int> g_chime_sound_index;
extern std::atomic<bool> g_settings_focus_secondary;
extern std::atomic<bool> g_settings_page_toggle_mode;
extern std::atomic<bool> g_settings_page_order_mode;
extern std::atomic<int> g_settings_primary_selection;
extern std::atomic<int> g_settings_selection;
extern std::atomic<int> g_settings_page_order_selection;
struct WeatherData {
    char city[32] = {};
    char text[32] = {};
    char icon[8] = {};
    char temp[8] = {};
    char humidity[8] = {};
    char lat[16] = {};
    char lon[16] = {};
};

struct WeatherAlertData {
    bool active = false;
    int count = 0;
    char titles[kMaxWeatherAlerts][kWeatherAlertTitleLen] = {};
    int ranks[kMaxWeatherAlerts] = {};
    time_t updated_at = 0;
};

struct WeatherForecastDay {
    bool valid = false;
    char date[12] = {};
    char text[24] = {};
    char icon[8] = {};
    char temp_max[8] = {};
    char temp_min[8] = {};
    char humidity[8] = {};
    char wind_dir[16] = {};
    char wind_scale[8] = {};
    char sunrise[8] = {};
    char sunset[8] = {};
};

struct WeatherForecastData {
    bool ready = false;
    int count = 0;
    WeatherForecastDay days[kWeatherForecastDays] = {};
    char advice[kWeatherAdviceLen] = {};
    time_t updated_at = 0;
};

struct WeatherAirData {
    bool ready = false;
    char aqi[8] = {};
    char category[16] = {};
    char primary[16] = {};
    char pm2p5[8] = {};
    time_t updated_at = 0;
};

struct SensorSample {
    int64_t sampled_at_ms = 0;
    float temperature = 0.0f;
    float humidity = 0.0f;
};

struct HourlySensorSample {
    int64_t timestamp = 0;
    float temperature = 0.0f;
    float humidity = 0.0f;
    uint8_t valid = 0;
    uint8_t reserved[7] = {};
};

struct HourlySensorHistoryBlob {
    uint32_t magic = kHourlyHistoryMagic;
    uint16_t version = 1;
    uint16_t count = kHourlyHistoryCount;
    HourlySensorSample samples[kHourlyHistoryCount] = {};
};

extern lv_obj_t *g_clock_root;
extern lv_obj_t *g_history_root;
extern lv_obj_t *g_gallery_root;
extern lv_obj_t *g_calendar_root;
extern lv_obj_t *g_weather_board_root;
extern lv_obj_t *g_flip_clock_root;
extern lv_obj_t *g_xiaozhi_root;
extern lv_obj_t *g_info_root;
extern lv_obj_t *g_network_diag_root;
extern lv_obj_t *g_settings_root;
extern lv_obj_t *g_date_label;
extern lv_obj_t *g_history_date_label;
extern lv_obj_t *g_gallery_date_label;
extern lv_obj_t *g_calendar_date_label;
extern lv_obj_t *g_weather_board_date_label;
extern lv_obj_t *g_flip_clock_date_label;
extern lv_obj_t *g_xiaozhi_date_label;
extern lv_obj_t *g_history_summary_label;
extern lv_obj_t *g_gallery_summary_label;
extern lv_obj_t *g_calendar_summary_label;
extern lv_obj_t *g_weather_board_summary_label;
extern lv_obj_t *g_xiaozhi_summary_label;
extern lv_obj_t *g_history_status_time_label;
extern lv_obj_t *g_calendar_status_time_label;
extern lv_obj_t *g_weather_board_status_time_label;
extern lv_obj_t *g_xiaozhi_status_time_label;
extern lv_obj_t *g_xiaozhi_state_label;
extern lv_obj_t *g_xiaozhi_detail_label;
extern lv_obj_t *g_xiaozhi_wave_canvas;
extern lv_obj_t *g_work_status_chime_icon_canvas[kWorkPageCount];
extern lv_obj_t *g_work_status_wifi_icon_canvas[kWorkPageCount];
extern lv_obj_t *g_work_status_alarm_icon_canvas[kWorkPageCount];
extern lv_obj_t *g_gallery_image_canvas;
extern lv_obj_t *g_gallery_time_canvas;
extern lv_obj_t *g_gallery_saying_label;
extern lv_obj_t *g_temp_icon_canvas;
extern lv_obj_t *g_humi_icon_canvas;
extern lv_obj_t *g_temp_label;
extern lv_obj_t *g_humi_label;
extern lv_obj_t *g_temp_trend_canvas;
extern lv_obj_t *g_humi_trend_canvas;
extern lv_obj_t *g_weather_city_label;
extern lv_obj_t *g_weather_info_label;
extern lv_obj_t *g_weather_icon_label;
extern lv_obj_t *g_weather_temp_label;
extern lv_obj_t *g_weather_humi_label;
extern lv_obj_t *g_alert_pill;
extern lv_obj_t *g_alert_icon_canvas;
extern lv_obj_t *g_alert_label;
extern lv_obj_t *g_chime_status_icon_canvas;
extern lv_obj_t *g_wifi_status_icon_canvas;
extern lv_obj_t *g_alarm_status_icon_canvas;
extern lv_obj_t *g_low_battery_icon_canvas;
extern lv_obj_t *g_panel_sep_a;
extern lv_obj_t *g_panel_sep_b;
extern lv_obj_t *g_battery_segments[5];
extern lv_obj_t *g_history_battery_segments[5];
extern lv_obj_t *g_gallery_battery_segments[5];
extern lv_obj_t *g_calendar_battery_segments[5];
extern lv_obj_t *g_weather_board_battery_segments[5];
extern lv_obj_t *g_flip_clock_battery_segments[5];
extern lv_obj_t *g_xiaozhi_battery_segments[5];
extern lv_obj_t *g_calendar_canvas;
extern lv_obj_t *g_history_chart_canvas;
extern lv_obj_t *g_history_temp_max_label;
extern lv_obj_t *g_history_temp_min_label;
extern lv_obj_t *g_history_humi_max_label;
extern lv_obj_t *g_history_humi_min_label;
extern lv_obj_t *g_history_time_labels[5];
extern lv_obj_t *g_history_temp_axis_labels[3];
extern lv_obj_t *g_history_humi_axis_labels[3];
extern lv_obj_t *g_time_canvas;
extern lv_obj_t *g_second_canvas;
extern lv_obj_t *g_status_gif_canvas;
extern lv_obj_t *g_second_progress_canvas;
extern lv_obj_t *g_flip_clock_card_canvas[3];
extern lv_obj_t *g_flip_clock_sensor_label;
extern lv_obj_t *g_flip_clock_sensor_bold_label;
extern lv_obj_t *g_flip_clock_sensor_bold_y_label;
extern lv_obj_t *g_flip_clock_humidity_label;
extern lv_obj_t *g_flip_clock_humidity_bold_label;
extern lv_obj_t *g_flip_clock_humidity_bold_y_label;
extern lv_obj_t *g_flip_clock_temp_mood_canvas;
extern lv_obj_t *g_flip_clock_humi_mood_canvas;
extern lv_obj_t *g_flip_clock_temp_trend_canvas;
extern lv_obj_t *g_flip_clock_humi_trend_canvas;
extern lv_obj_t *g_flip_clock_day_label;
extern lv_obj_t *g_flip_clock_day_bold_label;
extern lv_obj_t *g_flip_clock_day_bold_y_label;
extern lv_obj_t *g_flip_clock_lunar_label;
extern lv_obj_t *g_flip_clock_lunar_bold_x_label;
extern lv_obj_t *g_flip_clock_lunar_bold_y_label;
extern lv_obj_t *g_flip_clock_lunar_bold_xy_label;
extern lv_obj_t *g_lower_panel_objects[13];
extern lv_obj_t *g_setup_status_labels[6];
extern lv_obj_t *g_info_labels[6];
extern lv_obj_t *g_network_diag_labels[kNetworkDiagLineCount];
extern lv_obj_t *g_network_diag_summary_label;
extern lv_obj_t *g_network_diag_hint_label;
extern lv_obj_t *g_settings_labels[kSettingsLabelCount];
extern lv_obj_t *g_settings_switch_dots[kSettingsSecondaryMaxCount];
extern lv_obj_t *g_settings_feedback_label;
extern lv_obj_t *g_settings_ota_status_label;
extern lv_obj_t *g_settings_ota_hint_label;
extern lv_obj_t *g_settings_ota_bar_frame;
extern lv_obj_t *g_settings_ota_bar_fill;
