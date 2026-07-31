// 处理设置页网络、声音、显示和系统操作，不承担页面绘制。
#include "ui_views.h"

#include "alarm_services.h"
#include "app_constexpr.h"
#include "audio_services.h"
#include "chime_settings.h"
#include "manual_weather_city_state.h"
#include "network_diagnostics_state.h"
#include "network_services.h"
#include "ota_services.h"
#include "pomodoro_services.h"
#include "ui_info_page_state.h"
#include "ui_settings_activity_state.h"
#include "ui_settings_confirmation_state.h"
#include "ui_text_format.h"

#include <cstdarg>

namespace {
constexpr uint8_t kAllWorkPageMask = static_cast<uint8_t>((1U << kWorkPageCount) - 1);
constexpr int kSettingsFeedbackDefaultMs = 2500;
constexpr int kSettingsFeedbackBusyMs = 2000;
constexpr int kSettingsFeedbackSavedMs = 1800;
constexpr int kSettingsFeedbackInstructionMs = 3500;
constexpr const char *kSettingsSaveFailedFeedback = "保存失败";
constexpr const char *kSettingsOrderSavedFeedback = "页面顺序已保存";
constexpr const char *kSettingsSyncBusyFeedback = "请等待同步完成";
constexpr const char *kSettingsOfflineEnabledFeedback = "离线模式已开启";
constexpr const char *kSettingsOfflineDisabledFeedback = "离线模式已关闭";
constexpr const char *kOfflinePageUnavailableFeedback = "当前处于离线模式";
constexpr const char *kManualWeatherCityEditFeedback = "请进入配网页修改";
constexpr const char *kManualWeatherCityClearConfirmFeedback = "再次确认清除";
constexpr const char *kManualWeatherCityAutoFeedback = "已恢复自动定位";
constexpr const char *kManualNtpSyncFeedback = "正在同步时间...";
constexpr const char *kManualWeatherSyncFeedback = "正在同步天气...";
constexpr const char *kManualSayingSyncFeedback = "正在更新一言...";
constexpr const char *kSoundVolumeFeedbackFormat = "音量 %d%%";
constexpr const char *kSoundIndexFeedbackFormat = "声音 %d";
constexpr const char *kHourlyChimeEnabledFeedback = "整点提醒已开启";
constexpr const char *kHourlyChimeDisabledFeedback = "整点提醒已关闭";
constexpr const char *kAllDayChimeEnabledFeedback = "全天提醒已开启";
constexpr const char *kAllDayChimeDisabledFeedback = "全天提醒已关闭";
constexpr const char *kPageOrderInstructionFeedback = "BOOT交换并保存";
constexpr const char *kPageSwitchInstructionFeedback = "页面开关：BOOT切换";
constexpr const char *kLastWorkPageFeedback = "至少保留一个页面";
constexpr const char *kXiaozhiNeedsHomeFeedback = "请至少保留一个非小智页面";
constexpr const char *kXiaozhiHomeBlockedFeedback = "小智AI不能设为主页";
constexpr const char *kPomodoroRunningFeedback = "请先取消番茄钟";
constexpr const char *kXiaozhiAutoReturnEnabledFeedback = "小智AI自动返回已开启";
constexpr const char *kXiaozhiAutoReturnDisabledFeedback = "小智AI自动返回已关闭";
constexpr const char *kAlarmDisabledFeedback = "闹钟已关闭";
constexpr const char *kAlarmSetByXiaozhiFeedback = "请通过小智AI设置";
constexpr const char *kWorkPageFeedbackFormat = "%s%s";
constexpr const char *kWorkPageEnabledSuffix = "已开启";
constexpr const char *kWorkPageDisabledSuffix = "已关闭";
constexpr const char *kOfflineSetupConfirmFeedback = "再次确认进入配网";
constexpr const char *kSetupStartFailedFeedback = "配网启动失败";
constexpr const char *kOfflineSetupInstructionFeedback = "请完成配网后关闭";
constexpr const char *kNetworkDiagSyncFeedback = "正在网络检测...";
constexpr const char *kFactoryResetConfirmFeedback = "再次按 BOOT 确认";
constexpr const char *kFactoryResetFailedFeedback = "恢复失败";
constexpr size_t kSettingsFeedbackTextSize = 32;
#define HOURLY_CHIME_SETTING_LOG_FORMAT "hourly chime %s"
#define ALL_DAY_CHIME_SETTING_LOG_FORMAT "hourly chime all-day %s"
#define CHIME_SETTING_ENABLED_LOG_VALUE "enabled"
#define CHIME_SETTING_DISABLED_LOG_VALUE "disabled"
#define MANUAL_WEATHER_CITY_CLEARED_SYNC_LOG "manual weather city cleared, requesting weather sync"
#define MANUAL_NTP_SYNC_REQUESTED_LOG "manual ntp sync requested"
#define MANUAL_WEATHER_SYNC_REQUESTED_LOG "manual weather sync requested"
#define MANUAL_SAYING_SYNC_REQUESTED_LOG "manual daily saying sync requested"
#define MANUAL_NETWORK_DIAG_REQUESTED_LOG "manual network diagnostics requested"
#define FACTORY_RESET_CONFIRM_REQUESTED_LOG "factory reset confirmation requested"
#define FACTORY_RESET_REQUESTED_LOG "factory reset requested from settings"
#define SYSTEM_INFO_REQUESTED_LOG "system info requested from settings"

constexpr const char *kSettingsActionTexts[] = {
    kSettingsSaveFailedFeedback,
    kSettingsOrderSavedFeedback,
    kSettingsSyncBusyFeedback,
    kSettingsOfflineEnabledFeedback,
    kSettingsOfflineDisabledFeedback,
    kOfflinePageUnavailableFeedback,
    kManualWeatherCityEditFeedback,
    kManualWeatherCityClearConfirmFeedback,
    kManualWeatherCityAutoFeedback,
    kManualNtpSyncFeedback,
    kManualWeatherSyncFeedback,
    kManualSayingSyncFeedback,
    kSoundVolumeFeedbackFormat,
    kSoundIndexFeedbackFormat,
    kHourlyChimeEnabledFeedback,
    kHourlyChimeDisabledFeedback,
    kAllDayChimeEnabledFeedback,
    kAllDayChimeDisabledFeedback,
    kPageOrderInstructionFeedback,
    kPageSwitchInstructionFeedback,
    kLastWorkPageFeedback,
    kXiaozhiNeedsHomeFeedback,
    kXiaozhiHomeBlockedFeedback,
    kPomodoroRunningFeedback,
    kXiaozhiAutoReturnEnabledFeedback,
    kXiaozhiAutoReturnDisabledFeedback,
    kAlarmDisabledFeedback,
    kAlarmSetByXiaozhiFeedback,
    kWorkPageFeedbackFormat,
    kWorkPageEnabledSuffix,
    kWorkPageDisabledSuffix,
    kOfflineSetupConfirmFeedback,
    kSetupStartFailedFeedback,
    kOfflineSetupInstructionFeedback,
    kNetworkDiagSyncFeedback,
    kFactoryResetConfirmFeedback,
    kFactoryResetFailedFeedback,
    HOURLY_CHIME_SETTING_LOG_FORMAT,
    ALL_DAY_CHIME_SETTING_LOG_FORMAT,
    CHIME_SETTING_ENABLED_LOG_VALUE,
    CHIME_SETTING_DISABLED_LOG_VALUE,
    MANUAL_WEATHER_CITY_CLEARED_SYNC_LOG,
    MANUAL_NTP_SYNC_REQUESTED_LOG,
    MANUAL_WEATHER_SYNC_REQUESTED_LOG,
    MANUAL_SAYING_SYNC_REQUESTED_LOG,
    MANUAL_NETWORK_DIAG_REQUESTED_LOG,
    FACTORY_RESET_CONFIRM_REQUESTED_LOG,
    FACTORY_RESET_REQUESTED_LOG,
    SYSTEM_INFO_REQUESTED_LOG,
};

constexpr bool work_page_index_valid(int page)
{
    return page >= 0 && page < kWorkPageCount;
}

uint8_t toggled_work_page_mask(uint8_t current_mask, int page)
{
    if (!work_page_index_valid(page)) {
        return static_cast<uint8_t>(current_mask & kAllWorkPageMask);
    }
    uint8_t page_mask = static_cast<uint8_t>(1U << page);
    return static_cast<uint8_t>((current_mask ^ page_mask) & kAllWorkPageMask);
}

void set_formatted_settings_feedback(const char *format, ...)
{
    if (!format) {
        set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
        return;
    }
    char feedback[kSettingsFeedbackTextSize] = {};
    va_list args;
    va_start(args, format);
    int written = vsnprintf(feedback, sizeof(feedback), format, args);
    va_end(args);
    if (ui_text::format_failed(written, sizeof(feedback))) {
        set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
        return;
    }
    set_settings_feedback(feedback, kSettingsFeedbackDefaultMs);
}

template <typename Setting, typename Value>
bool save_chime_setting_or_restore(Setting &setting, Value previous)
{
    if (save_hourly_chime_setting()) {
        return true;
    }
    setting = previous;
    set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
    return false;
}

void queue_manual_settings_sync(SettingsSyncOp op,
                                const char *feedback,
                                const char *log_message,
                                EventBits_t event_bit)
{
    begin_settings_sync(op, feedback);
    ESP_LOGI(TAG, "%s", log_message);
    xEventGroupSetBits(g_app_events, event_bit);
}

void clear_inactive_settings_confirmation(int primary, int selected)
{
    if (!(primary == kSettingsPrimarySystem && selected == kSystemSettingsFactoryResetItem)) {
        settings_confirmation_clear(SettingsConfirmation::kFactoryReset);
    }
    if (!(primary == kSettingsPrimarySystem && selected == kSystemSettingsOfflineItem)) {
        settings_confirmation_clear(SettingsConfirmation::kOfflineDisable);
    }
    if (!(primary == kSettingsPrimaryNetwork && selected == kNetworkSettingsWeatherCityItem)) {
        settings_confirmation_clear(SettingsConfirmation::kWeatherCityClear);
    }
}

static_assert(kWorkPageCount > 0 && kWorkPageCount <= 8,
              "work page mask in settings UI is stored as uint8_t");
static_assert(kAllWorkPageMask != 0, "settings UI must have at least one work page bit");
static_assert(array_count(kSettingsActionTexts) > 0, "settings action text registry must not be empty");
static_assert(cstr_array_nonempty(kSettingsActionTexts), "settings action texts must be non-empty");
} // namespace

namespace {
void handle_page_order_settings_action()
{
    normalize_work_page_order();
    int current = valid_enabled_work_page_order_index(g_settings_page_order_selection);
    int next = next_enabled_work_page_order_index(current);
    if (!swap_work_page_order_entries_preserving_home(current, next)) {
        set_settings_feedback(kXiaozhiHomeBlockedFeedback, kSettingsFeedbackDefaultMs);
        return;
    }
    g_settings_page_order_selection = next;
    if (save_work_page_order()) {
        active_work_page_store(first_enabled_work_page());
        set_settings_feedback(kSettingsOrderSavedFeedback, kSettingsFeedbackSavedMs);
    } else {
        set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
    }
}

void handle_network_settings_action(int selected)
{
    if (selected == kNetworkSettingsWeatherCityItem) {
        if (!manual_weather_city_is_configured()) {
            set_settings_feedback(kManualWeatherCityEditFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        if (!settings_confirmation_pending(SettingsConfirmation::kWeatherCityClear)) {
            settings_confirmation_request(SettingsConfirmation::kWeatherCityClear);
            set_settings_feedback(kManualWeatherCityClearConfirmFeedback, kSettingsTimeoutMs);
            return;
        }
        if (!clear_manual_weather_city()) {
            set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        settings_confirmation_clear(SettingsConfirmation::kWeatherCityClear);
        if (g_offline_mode_ui_enabled) {
            set_settings_feedback(kManualWeatherCityAutoFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        queue_manual_settings_sync(kSettingsSyncWeather,
                                   kManualWeatherSyncFeedback,
                                   MANUAL_WEATHER_CITY_CLEARED_SYNC_LOG,
                                   kManualWeatherSyncBit);
        return;
    }
    if (g_offline_mode_ui_enabled) {
        set_settings_feedback(kSettingsOfflineEnabledFeedback, kSettingsFeedbackDefaultMs);
        return;
    }
    if (selected == kNetworkSettingsNtpItem) {
        queue_manual_settings_sync(kSettingsSyncNtp,
                                   kManualNtpSyncFeedback,
                                   MANUAL_NTP_SYNC_REQUESTED_LOG,
                                   kManualNtpSyncBit);
    } else if (selected == kNetworkSettingsWeatherItem) {
        queue_manual_settings_sync(kSettingsSyncWeather,
                                   kManualWeatherSyncFeedback,
                                   MANUAL_WEATHER_SYNC_REQUESTED_LOG,
                                   kManualWeatherSyncBit);
    } else if (selected == kNetworkSettingsSayingItem) {
        queue_manual_settings_sync(kSettingsSyncSaying,
                                   kManualSayingSyncFeedback,
                                   MANUAL_SAYING_SYNC_REQUESTED_LOG,
                                   kManualSayingSyncBit);
    }
}

void handle_sound_settings_action(int selected)
{
    if (selected == kSoundSettingsVolumeItem) {
        const int previous = g_chime_volume_percent.load(std::memory_order_acquire);
        const int next = chime_settings::next_volume_percent(previous);
        g_chime_volume_percent = next;
        if (!save_chime_setting_or_restore(g_chime_volume_percent, previous)) {
            return;
        }
        set_formatted_settings_feedback(kSoundVolumeFeedbackFormat, next);
        request_settings_confirmation_chime();
    } else if (selected == kSoundSettingsSoundItem) {
        const int previous = g_chime_sound_index.load(std::memory_order_acquire);
        const int next = (previous + 1) % kChimeSoundCount;
        g_chime_sound_index = next;
        if (!save_chime_setting_or_restore(g_chime_sound_index, previous)) {
            return;
        }
        set_formatted_settings_feedback(kSoundIndexFeedbackFormat, next + 1);
        request_settings_confirmation_chime();
    } else if (selected == kSoundSettingsHourlyItem) {
        const bool previous = g_hourly_chime_enabled.load(std::memory_order_acquire);
        const bool enabled = !previous;
        g_hourly_chime_enabled = enabled;
        if (!save_chime_setting_or_restore(g_hourly_chime_enabled, previous)) {
            return;
        }
        set_settings_feedback(enabled ? kHourlyChimeEnabledFeedback : kHourlyChimeDisabledFeedback,
                              kSettingsFeedbackDefaultMs);
        ESP_LOGI(TAG,
                 HOURLY_CHIME_SETTING_LOG_FORMAT,
                 enabled ? CHIME_SETTING_ENABLED_LOG_VALUE : CHIME_SETTING_DISABLED_LOG_VALUE);
        if (enabled) {
            request_settings_confirmation_chime();
        }
    } else if (selected == kSoundSettingsAllDayItem) {
        const bool previous = g_hourly_chime_all_day.load(std::memory_order_acquire);
        const bool enabled = !previous;
        g_hourly_chime_all_day = enabled;
        if (!save_chime_setting_or_restore(g_hourly_chime_all_day, previous)) {
            return;
        }
        set_settings_feedback(enabled ? kAllDayChimeEnabledFeedback : kAllDayChimeDisabledFeedback,
                              kSettingsFeedbackDefaultMs);
        ESP_LOGI(TAG,
                 ALL_DAY_CHIME_SETTING_LOG_FORMAT,
                 enabled ? CHIME_SETTING_ENABLED_LOG_VALUE : CHIME_SETTING_DISABLED_LOG_VALUE);
        if (enabled) {
            request_settings_confirmation_chime();
        }
    }
}

void handle_display_settings_action(int selected)
{
    if (g_settings_page_toggle_mode) {
        int page = g_settings_selection;
        if (!work_page_index_valid(page)) {
            page = kWorkPageWeatherClock;
        }
        if (g_offline_mode_ui_enabled &&
            !is_work_page_enabled(page) &&
            work_page_requires_network(page)) {
            set_settings_feedback(kOfflinePageUnavailableFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        uint8_t next_mask = toggled_work_page_mask(work_page_enabled_mask_load(), page);
        if (next_mask == 0) {
            set_settings_feedback(kLastWorkPageFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        if (!work_page_mask_has_valid_home(next_mask)) {
            set_settings_feedback(kXiaozhiNeedsHomeFeedback, kSettingsFeedbackInstructionMs);
            return;
        }
        if (page == kWorkPageXiaozhiAI &&
            is_work_page_enabled(page) &&
            pomodoro_is_running()) {
            set_settings_feedback(kPomodoroRunningFeedback, kSettingsFeedbackInstructionMs);
            return;
        }
        uint8_t previous = work_page_enabled_mask_load();
        work_page_enabled_mask_store(next_mask);
        if (!save_work_page_settings()) {
            work_page_enabled_mask_store(previous);
            set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        normalize_work_page_order();
        ensure_active_work_page_enabled();
        set_formatted_settings_feedback(kWorkPageFeedbackFormat,
                                        work_page_name(page),
                                        is_work_page_enabled(page) ? kWorkPageEnabledSuffix : kWorkPageDisabledSuffix);
        return;
    }
    if (selected == kDisplaySettingsPageSwitchItem) {
        g_settings_page_order_mode = false;
        g_settings_page_toggle_mode = true;
        g_settings_selection = 0;
        set_settings_feedback(kPageSwitchInstructionFeedback, kSettingsFeedbackInstructionMs);
        return;
    }
    if (selected == kDisplaySettingsOrderItem) {
        g_settings_page_toggle_mode = false;
        g_settings_page_order_mode = true;
        normalize_work_page_order();
        g_settings_page_order_selection = first_enabled_work_page_order_index();
        set_settings_feedback(kPageOrderInstructionFeedback, kSettingsFeedbackInstructionMs);
        return;
    }
    if (selected == kDisplaySettingsAlarmItem) {
        if (!alarm_is_enabled()) {
            set_settings_feedback(kAlarmSetByXiaozhiFeedback, kSettingsFeedbackInstructionMs);
            return;
        }
        if (!alarm_disable()) {
            set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        set_settings_feedback(kAlarmDisabledFeedback, kSettingsFeedbackDefaultMs);
        return;
    }
    if (selected == kDisplaySettingsXiaozhiAutoReturnItem) {
        bool previous = g_xiaozhi_auto_return_enabled;
        g_xiaozhi_auto_return_enabled = !g_xiaozhi_auto_return_enabled;
        if (!save_xiaozhi_auto_return_setting()) {
            g_xiaozhi_auto_return_enabled = previous;
            set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        set_settings_feedback(g_xiaozhi_auto_return_enabled
                                  ? kXiaozhiAutoReturnEnabledFeedback
                                  : kXiaozhiAutoReturnDisabledFeedback,
                              kSettingsFeedbackDefaultMs);
        return;
    }
    set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
}

void handle_system_settings_action(int selected)
{
    if (selected == kSystemSettingsOfflineItem) {
        if (!g_offline_mode_ui_enabled) {
            if (!set_offline_mode_enabled(true)) {
                set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            settings_confirmation_clear(SettingsConfirmation::kOfflineDisable);
            set_settings_feedback(kSettingsOfflineEnabledFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        if (can_leave_offline_mode_without_setup()) {
            if (!set_offline_mode_enabled(false)) {
                set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            settings_confirmation_clear(SettingsConfirmation::kOfflineDisable);
            set_settings_feedback(kSettingsOfflineDisabledFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        if (!settings_confirmation_pending(SettingsConfirmation::kOfflineDisable)) {
            settings_confirmation_request(SettingsConfirmation::kOfflineDisable);
            set_settings_feedback(kOfflineSetupConfirmFeedback, kSettingsTimeoutMs);
            return;
        }
        if (!start_wifi_radio(true)) {
            set_settings_feedback(kSetupStartFailedFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        settings_confirmation_clear(SettingsConfirmation::kOfflineDisable);
        set_settings_feedback(kOfflineSetupInstructionFeedback, kSettingsFeedbackInstructionMs);
    } else if (selected == kSystemSettingsNetworkDiagItem) {
        if (g_offline_mode_ui_enabled) {
            set_settings_feedback(kSettingsOfflineEnabledFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        begin_settings_sync(kSettingsSyncNetworkDiag, kNetworkDiagSyncFeedback);
        ESP_LOGI(TAG, "%s", MANUAL_NETWORK_DIAG_REQUESTED_LOG);
        network_diag_reset();
        settings_page_clear();
        network_diag_page_request();
        g_settings_focus_secondary = true;
        g_settings_primary_selection = kSettingsPrimarySystem;
        g_settings_selection = 0;
        info_page_hold_until_store(0);
        xEventGroupSetBits(g_app_events, kNetworkDiagBit);
    } else if (selected == kSystemSettingsFactoryResetItem) {
        if (!settings_confirmation_pending(SettingsConfirmation::kFactoryReset)) {
            settings_confirmation_request(SettingsConfirmation::kFactoryReset);
            set_settings_feedback(kFactoryResetConfirmFeedback, kSettingsTimeoutMs);
            ESP_LOGW(TAG, "%s", FACTORY_RESET_CONFIRM_REQUESTED_LOG);
            return;
        }
        ESP_LOGW(TAG, "%s", FACTORY_RESET_REQUESTED_LOG);
        if (!clear_saved_config()) {
            set_settings_feedback(kFactoryResetFailedFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        if (!start_wifi_radio(true)) {
            set_settings_feedback(kSetupStartFailedFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        settings_page_clear();
        g_settings_page_toggle_mode = false;
        g_settings_page_order_mode = false;
        settings_confirmation_clear(SettingsConfirmation::kFactoryReset);
        settings_confirmation_clear(SettingsConfirmation::kOfflineDisable);
    } else if (selected == kSystemSettingsInfoItem) {
        settings_page_clear();
        g_settings_page_toggle_mode = false;
        g_settings_page_order_mode = false;
        settings_confirmation_clear(SettingsConfirmation::kFactoryReset);
        info_page_request(xTaskGetTickCount() + pdMS_TO_TICKS(kSettingsTimeoutMs));
        ESP_LOGI(TAG, "%s", SYSTEM_INFO_REQUESTED_LOG);
    } else if (selected == kSystemSettingsOtaItem) {
        if (g_offline_mode_ui_enabled) {
            set_settings_feedback(kSettingsOfflineEnabledFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        ota_handle_info_key();
    }
}
} // namespace

void handle_settings_action()
{
    int primary = g_settings_primary_selection;
    if (primary < 0 || primary >= kSettingsPrimaryCount) {
        primary = kSettingsPrimaryNetwork;
    }
    int selected = clamp_settings_selection_for_mode(primary,
                                                     g_settings_selection,
                                                     g_settings_page_toggle_mode);
    g_settings_primary_selection = primary;
    g_settings_selection = selected;
    settings_activity_record(xTaskGetTickCount());
    if (g_settings_page_order_mode) {
        handle_page_order_settings_action();
        return;
    }
    if (!g_settings_focus_secondary) {
        g_settings_focus_secondary = true;
        g_settings_selection = 0;
        reset_settings_confirmation();
        clear_settings_feedback();
        return;
    }
    if (is_settings_sync_busy()) {
        set_settings_feedback(kSettingsSyncBusyFeedback, kSettingsFeedbackBusyMs);
        return;
    }
    clear_inactive_settings_confirmation(primary, selected);
    if (primary == kSettingsPrimaryNetwork) {
        handle_network_settings_action(selected);
    } else if (primary == kSettingsPrimarySound) {
        handle_sound_settings_action(selected);
    } else if (primary == kSettingsPrimaryDisplay) {
        handle_display_settings_action(selected);
    } else if (primary == kSettingsPrimarySystem) {
        handle_system_settings_action(selected);
    }
}
