// 生成网络、声音、显示和系统设置的二级菜单动态文案。
#include "ui_settings_content.h"

#include "alarm_services.h"
#include "app_constexpr.h"
#include "app_state.h"
#include "manual_weather_city_state.h"
#include "ui_settings_confirmation_state.h"
#include "ui_text_format.h"

#include <stdarg.h>

namespace {
#define SETTINGS_SECONDARY_FORMAT_FAILED_FORMAT "settings secondary text format failed index=%d"
#define SETTINGS_SECONDARY_INDEX_OUT_OF_RANGE_FORMAT "settings secondary text index out of range: %d"

constexpr const char *kSettingsNetworkSyncTimeText = "同步时间";
constexpr const char *kSettingsNetworkSyncWeatherText = "同步天气";
constexpr const char *kSettingsNetworkSayingText = "更新一言";
constexpr const char *kSettingsWeatherCityManualFormat = "天气城市 %s";
constexpr const char *kSettingsWeatherCityAutoText = "天气城市 自动";
constexpr const char *kSettingsSoundVolumeFormat = "音量 %d%%";
constexpr const char *kSettingsSoundChoiceFormat = "声音选择 %d";
constexpr const char *kSettingsHourlyText = "整点提醒 7:00 - 22:00";
constexpr const char *kSettingsAllDayText = "全天提醒 0:00 - 24:00";
constexpr const char *kSettingsPageSwitchText = "页面开关";
constexpr const char *kSettingsPageOrderText = "页面顺序";
constexpr const char *kSettingsAlarmOffText = "闹钟 --:--";
constexpr const char *kSettingsAlarmOnFormat = "闹钟 %02d:%02d";
constexpr const char *kSettingsXiaozhiAutoReturnText = "小智AI自动返回";
constexpr const char *kSettingsOfflineFormat = "离线模式 %s";
constexpr const char *kSettingsOfflineOnText = "开";
constexpr const char *kSettingsOfflineOffText = "关";
constexpr const char *kSettingsNetworkDiagText = "网络检测";
constexpr const char *kSettingsFactoryResetConfirmText = "确认恢复";
constexpr const char *kSettingsFactoryResetText = "恢复出厂设置";
constexpr const char *kSettingsSystemInfoText = "关于本机";
constexpr const char *kSettingsCheckUpdateText = "检查更新";
constexpr const char *kSettingsSecondaryTexts[] = {
    kSettingsNetworkSyncTimeText,
    kSettingsNetworkSyncWeatherText,
    kSettingsNetworkSayingText,
    kSettingsWeatherCityManualFormat,
    kSettingsWeatherCityAutoText,
    kSettingsSoundVolumeFormat,
    kSettingsSoundChoiceFormat,
    kSettingsHourlyText,
    kSettingsAllDayText,
    kSettingsPageSwitchText,
    kSettingsPageOrderText,
    kSettingsAlarmOffText,
    kSettingsAlarmOnFormat,
    kSettingsXiaozhiAutoReturnText,
    kSettingsOfflineFormat,
    kSettingsOfflineOnText,
    kSettingsOfflineOffText,
    kSettingsNetworkDiagText,
    kSettingsFactoryResetConfirmText,
    kSettingsFactoryResetText,
    kSettingsSystemInfoText,
    kSettingsCheckUpdateText,
};
constexpr const char *kSettingsContentLogTexts[] = {
    SETTINGS_SECONDARY_FORMAT_FAILED_FORMAT,
    SETTINGS_SECONDARY_INDEX_OUT_OF_RANGE_FORMAT,
};

static_assert(kSettingsSecondaryTextSize > 1,
              "settings secondary text buffer must fit text and NUL");
static_assert(array_count(kSettingsSecondaryTexts) > 0,
              "settings secondary text registry must not be empty");
static_assert(array_count(kSettingsContentLogTexts) > 0,
              "settings content log registry must not be empty");
static_assert(cstr_array_nonempty(kSettingsSecondaryTexts),
              "settings secondary menu texts must be non-empty");
static_assert(cstr_array_nonempty(kSettingsContentLogTexts),
              "settings content logs must be non-empty");
} // namespace

void set_secondary_text(char items[][kSettingsSecondaryTextSize],
                        int index,
                        const char *text)
{
    if (!settings_secondary_index_valid(index)) {
        ESP_LOGW(TAG, SETTINGS_SECONDARY_INDEX_OUT_OF_RANGE_FORMAT, index);
        return;
    }
    ui_text::copy(items[index], kSettingsSecondaryTextSize, text);
}

void format_secondary_text(char items[][kSettingsSecondaryTextSize],
                           int index,
                           const char *format,
                           ...)
{
    if (!settings_secondary_index_valid(index)) {
        ESP_LOGW(TAG, SETTINGS_SECONDARY_INDEX_OUT_OF_RANGE_FORMAT, index);
        return;
    }
    items[index][0] = '\0';
    va_list args;
    va_start(args, format);
    int written = vsnprintf(items[index],
                            kSettingsSecondaryTextSize,
                            format ? format : "",
                            args);
    va_end(args);
    if (ui_text::format_failed(written, kSettingsSecondaryTextSize)) {
        items[index][0] = '\0';
        ESP_LOGW(TAG, SETTINGS_SECONDARY_FORMAT_FAILED_FORMAT, index);
    }
}

bool settings_secondary_index_valid(int index)
{
    return index >= 0 && index < kSettingsSecondaryMaxCount;
}

void populate_settings_secondary_items(
    int primary,
    char secondary_items[][kSettingsSecondaryTextSize])
{
    if (primary == kSettingsPrimaryNetwork) {
        set_secondary_text(secondary_items, kNetworkSettingsNtpItem, kSettingsNetworkSyncTimeText);
        set_secondary_text(secondary_items, kNetworkSettingsWeatherItem, kSettingsNetworkSyncWeatherText);
        set_secondary_text(secondary_items, kNetworkSettingsSayingItem, kSettingsNetworkSayingText);
        char weather_city[kManualWeatherCityLen] = {};
        if (manual_weather_city_snapshot(weather_city, sizeof(weather_city))) {
            format_secondary_text(secondary_items,
                                  kNetworkSettingsWeatherCityItem,
                                  kSettingsWeatherCityManualFormat,
                                  weather_city);
        } else {
            set_secondary_text(secondary_items,
                               kNetworkSettingsWeatherCityItem,
                               kSettingsWeatherCityAutoText);
        }
    } else if (primary == kSettingsPrimarySound) {
        const int volume_percent = static_cast<int>(g_chime_volume_percent);
        const int sound_index = static_cast<int>(g_chime_sound_index);
        format_secondary_text(secondary_items,
                              kSoundSettingsVolumeItem,
                              kSettingsSoundVolumeFormat,
                              volume_percent);
        format_secondary_text(secondary_items,
                              kSoundSettingsSoundItem,
                              kSettingsSoundChoiceFormat,
                              sound_index + 1);
        set_secondary_text(secondary_items, kSoundSettingsHourlyItem, kSettingsHourlyText);
        set_secondary_text(secondary_items, kSoundSettingsAllDayItem, kSettingsAllDayText);
    } else if (primary == kSettingsPrimaryDisplay) {
        set_secondary_text(secondary_items,
                           kDisplaySettingsPageSwitchItem,
                           kSettingsPageSwitchText);
        set_secondary_text(secondary_items,
                           kDisplaySettingsOrderItem,
                           kSettingsPageOrderText);
        AlarmSnapshot alarm = {};
        alarm_get_snapshot(&alarm);
        if (alarm.enabled) {
            format_secondary_text(secondary_items,
                                  kDisplaySettingsAlarmItem,
                                  kSettingsAlarmOnFormat,
                                  alarm.hour,
                                  alarm.minute);
        } else {
            set_secondary_text(secondary_items,
                               kDisplaySettingsAlarmItem,
                               kSettingsAlarmOffText);
        }
        set_secondary_text(secondary_items,
                           kDisplaySettingsXiaozhiAutoReturnItem,
                           kSettingsXiaozhiAutoReturnText);
    } else {
        format_secondary_text(secondary_items,
                              kSystemSettingsOfflineItem,
                              kSettingsOfflineFormat,
                              g_offline_mode_ui_enabled ? kSettingsOfflineOnText
                                                        : kSettingsOfflineOffText);
        set_secondary_text(secondary_items,
                           kSystemSettingsNetworkDiagItem,
                           kSettingsNetworkDiagText);
        set_secondary_text(secondary_items,
                           kSystemSettingsFactoryResetItem,
                           settings_confirmation_pending(SettingsConfirmation::kFactoryReset)
                               ? kSettingsFactoryResetConfirmText
                               : kSettingsFactoryResetText);
        set_secondary_text(secondary_items,
                           kSystemSettingsInfoItem,
                           kSettingsSystemInfoText);
        set_secondary_text(secondary_items,
                           kSystemSettingsOtaItem,
                           kSettingsCheckUpdateText);
    }
}
