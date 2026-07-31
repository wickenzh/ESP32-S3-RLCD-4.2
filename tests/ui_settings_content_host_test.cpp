// 验证设置页四类二级菜单文案及安全索引边界。
#include "ui_settings_content.h"

#include "alarm_services.h"
#include "app_state.h"
#include "manual_weather_city_state.h"
#include "ui_settings_confirmation_state.h"

#include <assert.h>
#include <string.h>

const char *const TAG = "test";
bool g_offline_mode_ui_enabled = false;
int g_chime_volume_percent = 60;
int g_chime_sound_index = 2;

namespace {
AlarmSnapshot s_alarm = {};
char s_manual_weather_city[kManualWeatherCityLen] = {};

void expect_text(char items[][kSettingsSecondaryTextSize], int index, const char *expected)
{
    assert(strcmp(items[index], expected) == 0);
}
} // namespace

bool manual_weather_city_snapshot(char *out, size_t out_len)
{
    if (!out || out_len < sizeof(s_manual_weather_city)) {
        return false;
    }
    memcpy(out, s_manual_weather_city, sizeof(s_manual_weather_city));
    return s_manual_weather_city[0] != '\0';
}

void manual_weather_city_store(const char *city)
{
    strlcpy(s_manual_weather_city, city ? city : "", sizeof(s_manual_weather_city));
}

bool manual_weather_city_is_configured()
{
    return s_manual_weather_city[0] != '\0';
}

void alarm_get_snapshot(AlarmSnapshot *out)
{
    if (out) {
        *out = s_alarm;
    }
}

int main()
{
    char items[kSettingsSecondaryMaxCount][kSettingsSecondaryTextSize] = {};

    assert(!settings_secondary_index_valid(-1));
    assert(settings_secondary_index_valid(0));
    assert(settings_secondary_index_valid(kSettingsSecondaryMaxCount - 1));
    assert(!settings_secondary_index_valid(kSettingsSecondaryMaxCount));

    populate_settings_secondary_items(kSettingsPrimaryNetwork, items);
    expect_text(items, kNetworkSettingsNtpItem, "同步时间");
    expect_text(items, kNetworkSettingsWeatherItem, "同步天气");
    expect_text(items, kNetworkSettingsSayingItem, "更新一言");
    expect_text(items, kNetworkSettingsWeatherCityItem, "天气城市 自动");

    manual_weather_city_store("杭州");
    memset(items, 0, sizeof(items));
    populate_settings_secondary_items(kSettingsPrimaryNetwork, items);
    expect_text(items, kNetworkSettingsWeatherCityItem, "天气城市 杭州");

    memset(items, 0, sizeof(items));
    populate_settings_secondary_items(kSettingsPrimarySound, items);
    expect_text(items, kSoundSettingsVolumeItem, "音量 60%");
    expect_text(items, kSoundSettingsSoundItem, "声音选择 3");
    expect_text(items, kSoundSettingsHourlyItem, "整点提醒 7:00 - 22:00");
    expect_text(items, kSoundSettingsAllDayItem, "全天提醒 0:00 - 24:00");

    memset(items, 0, sizeof(items));
    populate_settings_secondary_items(kSettingsPrimaryDisplay, items);
    expect_text(items, kDisplaySettingsPageSwitchItem, "页面开关");
    expect_text(items, kDisplaySettingsOrderItem, "页面顺序");
    expect_text(items, kDisplaySettingsAlarmItem, "闹钟 --:--");
    expect_text(items, kDisplaySettingsXiaozhiAutoReturnItem, "小智AI自动返回");

    s_alarm.enabled = true;
    s_alarm.hour = 6;
    s_alarm.minute = 30;
    memset(items, 0, sizeof(items));
    populate_settings_secondary_items(kSettingsPrimaryDisplay, items);
    expect_text(items, kDisplaySettingsAlarmItem, "闹钟 06:30");

    g_offline_mode_ui_enabled = true;
    settings_confirmation_request(SettingsConfirmation::kFactoryReset);
    memset(items, 0, sizeof(items));
    populate_settings_secondary_items(kSettingsPrimarySystem, items);
    expect_text(items, kSystemSettingsOfflineItem, "离线模式 开");
    expect_text(items, kSystemSettingsNetworkDiagItem, "网络检测");
    expect_text(items, kSystemSettingsFactoryResetItem, "确认恢复");
    expect_text(items, kSystemSettingsInfoItem, "关于本机");
    expect_text(items, kSystemSettingsOtaItem, "检查更新");

    return 0;
}
