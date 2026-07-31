// 维护设置页 KEY 导航、返回状态和二次确认清理逻辑。
#include "ui_settings_navigation.h"

#include "app_tick_time.h"
#include "network_services.h"
#include "ui_settings_activity_state.h"
#include "ui_settings_confirmation_state.h"
#include "ui_settings_feedback.h"
#include "ui_views.h"

namespace {
TickType_t s_settings_primary_exit_block_until = 0;

constexpr uint32_t kSettingsPrimaryExitBlockMs = 800;
constexpr uint32_t kSettingsOrderExitFeedbackMs = 2500;
constexpr const char *kSettingsOrderExitSavedFeedback = "页面顺序已保存";
constexpr const char *kSettingsOrderExitSaveFailedFeedback = "保存失败";

constexpr int clamp_selection_to_count(int selected, int count)
{
    return count > 0 && selected >= 0 && selected < count ? selected : 0;
}

static_assert(kSettingsPrimaryExitBlockMs > 0, "settings primary exit block duration must be positive");
static_assert(kSettingsOrderExitFeedbackMs > 0, "settings order exit feedback duration must be positive");
static_assert(kSettingsOrderExitSavedFeedback[0] != '\0', "settings order saved feedback must not be empty");
static_assert(kSettingsOrderExitSaveFailedFeedback[0] != '\0',
              "settings order save failed feedback must not be empty");
static_assert(clamp_selection_to_count(kWorkPageCalendar, kWorkPageCount) == kWorkPageCalendar &&
                  clamp_selection_to_count(kWorkPageHistory, kWorkPageCount) == kWorkPageHistory &&
                  clamp_selection_to_count(kWorkPageXiaozhiAI, kWorkPageCount) == kWorkPageXiaozhiAI,
              "high work page indices must remain selectable in page toggle mode");
} // namespace

int settings_secondary_count(int primary)
{
    switch (primary) {
    case kSettingsPrimaryNetwork:
        return kNetworkSettingsSecondaryCount;
    case kSettingsPrimarySound:
        return kSoundSettingsSecondaryCount;
    case kSettingsPrimaryDisplay:
        return kDisplaySettingsSecondaryCount;
    case kSettingsPrimarySystem:
        return kSystemSettingsSecondaryCount;
    default:
        return 0;
    }
}

void reset_settings_confirmation()
{
    settings_confirmation_clear_all();
}

void reset_settings_navigation_state()
{
    g_settings_focus_secondary = false;
    g_settings_page_toggle_mode = false;
    g_settings_page_order_mode = false;
    reset_settings_confirmation();
}

void enter_settings_primary_navigation()
{
    g_settings_page_toggle_mode.store(false, std::memory_order_relaxed);
    g_settings_page_order_mode.store(false, std::memory_order_relaxed);
    g_settings_primary_selection.store(kSettingsPrimaryNetwork, std::memory_order_relaxed);
    g_settings_selection.store(0, std::memory_order_relaxed);
    g_settings_page_order_selection.store(0, std::memory_order_relaxed);
    g_settings_focus_secondary.store(false, std::memory_order_release);
}

void enter_settings_system_item_navigation(int selection)
{
    const int selected = clamp_settings_secondary(kSettingsPrimarySystem, selection);
    g_settings_page_toggle_mode.store(false, std::memory_order_relaxed);
    g_settings_page_order_mode.store(false, std::memory_order_relaxed);
    g_settings_primary_selection.store(kSettingsPrimarySystem, std::memory_order_relaxed);
    g_settings_selection.store(selected, std::memory_order_relaxed);
    g_settings_page_order_selection.store(0, std::memory_order_relaxed);
    g_settings_focus_secondary.store(true, std::memory_order_release);
}

int clamp_settings_primary(int primary)
{
    if (primary < 0 || primary >= kSettingsPrimaryCount) {
        return kSettingsPrimaryNetwork;
    }
    return primary;
}

int clamp_settings_secondary(int primary, int selected)
{
    int count = settings_secondary_count(primary);
    return clamp_selection_to_count(selected, count);
}

int clamp_settings_selection_for_mode(int primary, int selected, bool page_toggle_mode)
{
    if (!page_toggle_mode) {
        return clamp_settings_secondary(primary, selected);
    }
    return clamp_selection_to_count(selected, kWorkPageCount);
}

void handle_settings_key_short()
{
    settings_activity_record(xTaskGetTickCount());
    int primary = clamp_settings_primary(g_settings_primary_selection);
    if (g_settings_page_order_mode) {
        g_settings_page_order_selection = next_enabled_work_page_order_index(g_settings_page_order_selection);
    } else if (g_settings_page_toggle_mode) {
        g_settings_selection = (g_settings_selection + 1) % kWorkPageCount;
    } else if (g_settings_focus_secondary) {
        int count = settings_secondary_count(primary);
        if (count > 0) {
            g_settings_selection = (clamp_settings_secondary(primary, g_settings_selection) + 1) % count;
        }
    } else {
        g_settings_primary_selection = (primary + 1) % kSettingsPrimaryCount;
        g_settings_selection = 0;
    }
    reset_settings_confirmation();
    clear_settings_feedback();
    notify_ui_task();
}

void handle_settings_key_long()
{
    settings_activity_record(xTaskGetTickCount());
    if (g_settings_page_order_mode) {
        g_settings_page_order_mode = false;
        g_settings_focus_secondary = true;
        g_settings_primary_selection = kSettingsPrimaryDisplay;
        g_settings_selection = kDisplaySettingsOrderItem;
        if (save_work_page_order()) {
            active_work_page_store(first_enabled_work_page());
            set_settings_feedback(kSettingsOrderExitSavedFeedback, kSettingsOrderExitFeedbackMs);
        } else {
            set_settings_feedback(kSettingsOrderExitSaveFailedFeedback, kSettingsOrderExitFeedbackMs);
        }
        reset_settings_confirmation();
        notify_ui_task();
        return;
    } else if (g_settings_page_toggle_mode) {
        g_settings_page_toggle_mode = false;
        g_settings_focus_secondary = true;
        g_settings_primary_selection = kSettingsPrimaryDisplay;
        g_settings_selection = kDisplaySettingsPageSwitchItem;
        reset_settings_confirmation();
        clear_settings_feedback();
        notify_ui_task();
        return;
    } else if (g_settings_focus_secondary) {
        g_settings_focus_secondary = false;
        g_settings_selection = 0;
        s_settings_primary_exit_block_until = xTaskGetTickCount() + pdMS_TO_TICKS(kSettingsPrimaryExitBlockMs);
    } else {
        TickType_t now = xTaskGetTickCount();
        if (s_settings_primary_exit_block_until != 0 &&
            app_tick_deadline_pending(now, s_settings_primary_exit_block_until)) {
            settings_activity_record(now);
            notify_ui_task();
            return;
        }
        s_settings_primary_exit_block_until = 0;
        settings_page_clear();
        reset_settings_navigation_state();
        clear_settings_feedback();
        notify_ui_task();
        return;
    }
    reset_settings_confirmation();
    clear_settings_feedback();
    notify_ui_task();
}
