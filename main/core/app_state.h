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

#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
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
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

#include "display_bsp.h"
#include "i2c_bsp.h"
#include "i2c_equipment.h"
#include "codec_bsp.h"
#include "lvgl_bsp.h"
#include "dseg_digits.h"
#include "boot_anim.h"
#include "status_gif_60.h"
#include "ui_icons.h"

LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_font_16);

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
inline constexpr gpio_num_t kBootButtonGpio = GPIO_NUM_0;
inline constexpr gpio_num_t kKeyButtonGpio = GPIO_NUM_18;
inline constexpr const char *kSetupApPassword = "12345678";
inline constexpr const char *kSetupPortalIp = "192.168.4.1";
inline constexpr const char *kSetupPortalUrl = "http://192.168.4.1/";
inline constexpr int kAppMsPerSecond = 1000;
inline constexpr int kAppSecondsPerMinute = 60;
inline constexpr int kAppMsPerMinute = kAppSecondsPerMinute * kAppMsPerSecond;
inline constexpr int kSettingsTimeoutMs = 30 * kAppMsPerSecond;
inline constexpr int kHistoryPageTimeoutMs = 5 * kAppMsPerMinute;
inline constexpr int kSettingsPrimaryCount = 4;
inline constexpr int kSettingsSecondaryMaxCount = 7;
inline constexpr int kSettingsLabelCount = kSettingsPrimaryCount + kSettingsSecondaryMaxCount;
inline constexpr int kChimeSoundCount = 5;
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
inline constexpr int kWorkPageHistory = 1;
inline constexpr int kWorkPageGallery = 2;
inline constexpr int kWorkPageCalendar = 3;
inline constexpr int kWorkPageWeatherBoard = 4;
inline constexpr int kWorkPageFlipClock = 5;
inline constexpr int kWorkPageCount = 6;
inline constexpr int kDisplaySettingsPageItemCount = kWorkPageCount;
inline constexpr int kDisplaySettingsOrderItem = kDisplaySettingsPageItemCount;
inline constexpr int kDisplaySettingsSecondaryCount = kDisplaySettingsOrderItem + 1;
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
inline constexpr int kButtonIdlePollMs = 250;
inline constexpr int kButtonLowRefreshIdlePollMs = 500;
inline constexpr int kButtonActivePollMs = 100;
inline constexpr int kButtonPressedPollMs = 50;
inline constexpr int kBootAnimRunFrameMs = 50;
inline constexpr int kBootWifiConnectTimeoutMs = 5 * kAppMsPerSecond;
inline constexpr int kBootNtpRetries = 2;
inline constexpr int kBootStartupBudgetMs = 6 * kAppMsPerSecond;
inline constexpr int kHttpDefaultTimeoutMs = 10 * kAppMsPerSecond;
inline constexpr int kHttpBootTimeoutMs = 2500;
inline constexpr int kMinValidYear = 2024;
inline constexpr int kMaxValidYear = 2035;
inline constexpr int kLowBatteryEnterPercent = 10;
inline constexpr int kLowBatteryExitPercent = 13;
inline constexpr int kDisplayPartialMaxWidth = (kDisplayWidth * 7) / 10;
inline constexpr int kMaxFlushRanges = 8;
inline constexpr int kFlushRangeMergeGap = 8;
inline constexpr int kDisplayFlushDiagIntervalMs = kAppMsPerMinute;
inline constexpr int kSensorHistoryMinutes = 120;
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
inline constexpr int kOtaStatusLen = 96;
inline constexpr int kOtaVersionLen = 24;
inline constexpr int kOtaUrlLen = 256;
inline constexpr int kOtaSha256Len = 65;
inline constexpr int kOtaNotesLen = 96;
inline constexpr int kOtaHttpTimeoutMs = 8 * kAppMsPerSecond;
inline constexpr int kOtaNoProgressTimeoutMs = 45 * kAppMsPerSecond;
inline constexpr int kOtaMaxDownloadMs = 10 * kAppMsPerMinute;
inline constexpr int kOtaStatusMinIntervalMs = 3 * kAppMsPerSecond;
inline constexpr int kOtaAvailableConfirmTimeoutMs = kAppMsPerMinute;
inline constexpr int kOtaDownloadBufferSize = 4096;
inline constexpr int kOtaChunkDelayMs = 25;
inline constexpr int kNetworkDiagLineCount = 9;
inline constexpr int kNetworkDiagLineLen = 48;
inline constexpr const char *kGiteaCheckUrl = "https://example.invalid/";
inline constexpr const char *kTypelessCheckUrl = "https://typeless.com/";
inline constexpr float kBatteryChargingRiseVoltage = 0.035f;
inline constexpr float kBatteryChargingStopVoltage = 0.008f;
inline constexpr int kBatteryChargingRiseSamples = 2;
inline constexpr int kBatteryChargingSampleMs = kAppMsPerMinute;
inline constexpr int kBatterySampleUnknownTimeMinutes = 10;
inline constexpr int kBatterySampleDayMinutes = 10;
inline constexpr int kBatterySampleNightMinutes = 20;

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

enum OtaUiState {
    kOtaIdle = 0,
    kOtaChecking = 1,
    kOtaAvailable = 2,
    kOtaUpdating = 3,
    kOtaSucceeded = 4,
    kOtaFailed = 5,
    kOtaNoUpdate = 6,
};

extern DisplayPort g_display;
extern I2cMasterBus g_i2c;
extern Shtc3Port *g_shtc3;
extern CodecPort *g_codec;
extern portMUX_TYPE g_audio_state_mux;
extern portMUX_TYPE g_weather_state_mux;
extern bool g_audio_playing;
extern volatile bool g_startup_screen_active;
extern volatile bool g_setup_prompt_pending;
#if CONFIG_PM_ENABLE
extern esp_pm_lock_handle_t g_network_pm_lock;
extern esp_pm_lock_handle_t g_audio_pm_lock;
extern int g_network_pm_lock_depth;
extern int g_audio_pm_lock_depth;
#endif
extern adc_oneshot_unit_handle_t g_battery_adc;
extern adc_cali_handle_t g_battery_adc_cali;
extern bool g_battery_adc_ready;
extern bool g_battery_adc_cali_ready;
extern EventGroupHandle_t g_app_events;
extern httpd_handle_t g_http_server;

extern char g_wifi_ssid[33];
extern char g_wifi_pass[65];
extern char g_weather_api_key[96];
extern char g_manual_weather_city[kManualWeatherCityLen];
extern char g_ap_ssid[33];
extern char g_sta_ip[16];
extern bool g_have_wifi_creds;
extern bool g_have_weather_key;
extern bool g_has_manual_weather_city;
extern bool g_hourly_chime_enabled;
extern bool g_hourly_chime_all_day;
extern bool g_offline_mode_ui_enabled;
extern int g_chime_volume_percent;
extern int g_chime_sound_index;
extern bool g_ntp_started;
extern bool g_wifi_radio_on;
extern bool g_wifi_stop_requested;
extern bool g_setup_portal_active;
extern int g_last_wifi_disconnect_reason;
extern int g_http_timeout_ms;
extern int64_t g_boot_sync_deadline_us;
extern float g_temperature;
extern float g_humidity;
extern bool g_sensor_ok;
extern int g_temp_trend;
extern int g_humi_trend;
extern int g_battery_percent;
extern float g_battery_voltage;
extern bool g_battery_charging;
extern uint32_t g_battery_version;
extern time_t g_last_ntp_sync_time;
extern time_t g_last_weather_sync_time;
extern volatile bool g_boot_info_requested;
extern volatile bool g_network_diag_page_requested;
extern volatile bool g_settings_requested;
extern volatile bool g_settings_focus_secondary;
extern volatile bool g_settings_page_order_mode;
extern volatile int g_settings_primary_selection;
extern volatile int g_settings_selection;
extern volatile int g_settings_page_order_selection;
extern volatile uint32_t g_settings_action_seq;
extern volatile TickType_t g_settings_last_activity_tick;
extern volatile int g_settings_sync_op;
extern volatile TickType_t g_settings_sync_deadline_tick;
extern volatile TickType_t g_info_page_until_tick;
extern TickType_t g_settings_feedback_until_tick;
extern bool g_factory_reset_confirm_pending;
extern bool g_offline_disable_confirm_pending;
extern bool g_weather_city_clear_confirm_pending;
extern char g_settings_feedback[48];
extern volatile int g_ota_state;
extern volatile int g_ota_progress;
extern volatile int g_ota_speed_kbps;
extern volatile TickType_t g_ota_status_until_tick;
extern volatile bool g_ota_reboot_pending;
extern char g_ota_status[kOtaStatusLen];
extern char g_ota_version[kOtaVersionLen];
extern char g_ota_url[kOtaUrlLen];
extern char g_ota_sha256[kOtaSha256Len];
extern char g_ota_notes[kOtaNotesLen];
extern int g_ota_size;
extern volatile int g_network_diag_state;
extern volatile int g_network_diag_step;
extern volatile int g_network_diag_passed;
extern volatile int g_network_diag_total;
extern char g_network_diag_lines[kNetworkDiagLineCount][kNetworkDiagLineLen];
extern char g_daily_saying[kDailySayingLen];
extern time_t g_last_saying_sync_time;

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

extern WeatherData g_weather;
extern WeatherAlertData g_weather_alert;
extern WeatherForecastData g_weather_forecast;
extern WeatherAirData g_weather_air;
extern volatile bool g_low_battery_mode;
extern SensorSample g_sensor_history[kSensorHistoryMinutes];
extern int g_sensor_history_next;
extern int g_sensor_history_count;
extern bool g_sensor_average_valid;
extern float g_last_temp_average;
extern float g_last_humi_average;
extern HourlySensorHistoryBlob g_hourly_history;
extern int64_t g_last_hourly_saved_at;
extern uint32_t g_hourly_history_version;
extern volatile int g_active_work_page;
extern uint8_t g_work_page_enabled_mask;
extern uint8_t g_work_page_order[kWorkPageCount];

extern lv_obj_t *g_clock_root;
extern lv_obj_t *g_history_root;
extern lv_obj_t *g_gallery_root;
extern lv_obj_t *g_calendar_root;
extern lv_obj_t *g_weather_board_root;
extern lv_obj_t *g_flip_clock_root;
extern lv_obj_t *g_info_root;
extern lv_obj_t *g_network_diag_root;
extern lv_obj_t *g_settings_root;
extern lv_obj_t *g_date_label;
extern lv_obj_t *g_clock_summary_label;
extern lv_obj_t *g_history_date_label;
extern lv_obj_t *g_gallery_date_label;
extern lv_obj_t *g_calendar_date_label;
extern lv_obj_t *g_weather_board_date_label;
extern lv_obj_t *g_flip_clock_date_label;
extern lv_obj_t *g_history_summary_label;
extern lv_obj_t *g_gallery_summary_label;
extern lv_obj_t *g_calendar_summary_label;
extern lv_obj_t *g_weather_board_summary_label;
extern lv_obj_t *g_flip_clock_summary_label;
extern lv_obj_t *g_history_status_time_label;
extern lv_obj_t *g_gallery_status_time_label;
extern lv_obj_t *g_calendar_status_time_label;
extern lv_obj_t *g_weather_board_status_time_label;
extern lv_obj_t *g_flip_clock_status_time_label;
extern lv_obj_t *g_work_status_chime_icon_canvas[kWorkPageCount];
extern lv_color_t *g_work_status_chime_icon_canvas_buf[kWorkPageCount];
extern lv_obj_t *g_work_status_wifi_icon_canvas[kWorkPageCount];
extern lv_color_t *g_work_status_wifi_icon_canvas_buf[kWorkPageCount];
extern lv_obj_t *g_gallery_time_label;
extern lv_obj_t *g_gallery_hour_label;
extern lv_obj_t *g_gallery_minute_label;
extern lv_obj_t *g_gallery_image_canvas;
extern lv_color_t *g_gallery_image_canvas_buf;
extern lv_obj_t *g_gallery_time_canvas;
extern lv_color_t *g_gallery_time_canvas_buf;
extern lv_obj_t *g_gallery_saying_label;
extern lv_obj_t *g_temp_icon_canvas;
extern lv_obj_t *g_humi_icon_canvas;
extern lv_color_t *g_temp_icon_canvas_buf;
extern lv_color_t *g_humi_icon_canvas_buf;
extern lv_obj_t *g_temp_label;
extern lv_obj_t *g_humi_label;
extern lv_obj_t *g_temp_trend_canvas;
extern lv_obj_t *g_humi_trend_canvas;
extern lv_color_t *g_temp_trend_canvas_buf;
extern lv_color_t *g_humi_trend_canvas_buf;
extern lv_obj_t *g_weather_city_label;
extern lv_obj_t *g_weather_info_label;
extern lv_obj_t *g_weather_icon_label;
extern lv_obj_t *g_weather_temp_label;
extern lv_obj_t *g_weather_humi_label;
extern lv_obj_t *g_alert_pill;
extern lv_obj_t *g_alert_icon_canvas;
extern lv_color_t *g_alert_icon_canvas_buf;
extern lv_obj_t *g_alert_label;
extern lv_obj_t *g_chime_status_icon_canvas;
extern lv_color_t *g_chime_status_icon_canvas_buf;
extern lv_obj_t *g_wifi_status_icon_canvas;
extern lv_color_t *g_wifi_status_icon_canvas_buf;
extern lv_obj_t *g_low_battery_icon_canvas;
extern lv_color_t *g_low_battery_icon_canvas_buf;
extern lv_obj_t *g_panel_sep_a;
extern lv_obj_t *g_panel_sep_b;
extern lv_obj_t *g_battery_segments[5];
extern lv_obj_t *g_history_battery_segments[5];
extern lv_obj_t *g_gallery_battery_segments[5];
extern lv_obj_t *g_calendar_battery_segments[5];
extern lv_obj_t *g_weather_board_battery_segments[5];
extern lv_obj_t *g_flip_clock_battery_segments[5];
extern lv_obj_t *g_calendar_month_label;
extern lv_obj_t *g_calendar_canvas;
extern lv_color_t *g_calendar_canvas_buf;
extern lv_obj_t *g_history_chart_canvas;
extern lv_color_t *g_history_chart_canvas_buf;
extern lv_obj_t *g_history_temp_max_label;
extern lv_obj_t *g_history_temp_min_label;
extern lv_obj_t *g_history_humi_max_label;
extern lv_obj_t *g_history_humi_min_label;
extern lv_obj_t *g_history_time_labels[5];
extern lv_obj_t *g_history_temp_axis_labels[3];
extern lv_obj_t *g_history_humi_axis_labels[3];
extern lv_obj_t *g_time_canvas;
extern lv_color_t *g_time_canvas_buf;
extern lv_obj_t *g_second_canvas;
extern lv_color_t *g_second_canvas_buf;
extern lv_obj_t *g_status_gif_canvas;
extern lv_color_t *g_status_gif_canvas_buf;
extern lv_obj_t *g_day_progress_canvas;
extern lv_color_t *g_day_progress_canvas_buf;
extern lv_obj_t *g_second_progress_canvas;
extern lv_color_t *g_second_progress_canvas_buf;
extern lv_obj_t *g_flip_clock_card_canvas[3];
extern lv_color_t *g_flip_clock_card_canvas_buf[3];
extern lv_obj_t *g_flip_clock_sensor_label;
extern lv_obj_t *g_flip_clock_humidity_label;
extern lv_obj_t *g_flip_clock_day_progress_canvas;
extern lv_color_t *g_flip_clock_day_progress_canvas_buf;
extern lv_obj_t *g_flip_clock_second_progress_canvas;
extern lv_color_t *g_flip_clock_second_progress_canvas_buf;
extern lv_obj_t *g_lower_panel_objects[13];
extern lv_obj_t *g_setup_status_labels[6];
extern lv_obj_t *g_boot_status_label;
extern lv_obj_t *g_boot_detail_label;
extern lv_obj_t *g_boot_anim_canvas;
extern lv_color_t *g_boot_anim_canvas_buf;
extern lv_obj_t *g_info_labels[6];
extern lv_obj_t *g_network_diag_labels[kNetworkDiagLineCount];
extern lv_obj_t *g_network_diag_summary_label;
extern lv_obj_t *g_network_diag_hint_label;
extern lv_obj_t *g_info_ota_label;
extern lv_obj_t *g_info_ota_hint_label;
extern lv_obj_t *g_info_ota_bar_frame;
extern lv_obj_t *g_info_ota_bar_fill;
extern lv_obj_t *g_settings_labels[kSettingsLabelCount];
extern lv_obj_t *g_settings_switch_dots[kSettingsSecondaryMaxCount];
extern lv_obj_t *g_settings_switch_texts[kSettingsSecondaryMaxCount];
extern lv_obj_t *g_settings_feedback_label;
extern lv_obj_t *g_settings_ota_status_label;
extern lv_obj_t *g_settings_ota_hint_label;
extern lv_obj_t *g_settings_ota_bar_frame;
extern lv_obj_t *g_settings_ota_bar_fill;
extern volatile bool g_boot_anim_running;
extern volatile int g_boot_anim_current_frame;
extern TaskHandle_t g_boot_anim_task_handle;
extern TaskHandle_t g_boot_sync_task_handle;
extern TaskHandle_t g_ui_task_handle;
extern int g_last_ui_second;
extern int g_last_ui_minute;
extern int g_last_ui_date_key;
extern int g_last_ui_date_page;
extern int g_last_day_progress_filled;
extern int g_last_second_progress_filled;
extern int g_last_status_gif_frame;
extern int g_last_flip_clock_hour;
extern int g_last_flip_clock_minute;
extern int g_last_flip_clock_second;
extern int g_last_flip_day_progress_filled;
extern int g_last_flip_second_progress_filled;
extern int g_last_flip_sensor_minute;
extern int g_last_temp_trend_drawn;
extern int g_last_humi_trend_drawn;
extern uint32_t g_last_history_drawn_version;
extern int g_last_history_drawn_hour;
extern int g_last_calendar_drawn_month;
extern int g_last_calendar_drawn_day;
