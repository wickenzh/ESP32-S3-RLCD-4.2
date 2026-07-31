// 构建并刷新设置页及其菜单交互。
#include "ui_views.h"

#include "alarm_services.h"
#include "app_constexpr.h"
#include "app_tick_time.h"

#include "ota_runtime_state.h"
#include "ota_services.h"
#include "ui_settings_content.h"
#include "ui_settings_ota_panel.h"

namespace {
int collect_visible_work_page_order(int *indices, uint8_t *pages, size_t capacity)
{
    if (!indices || !pages || capacity == 0) {
        return 0;
    }
    uint8_t order[kWorkPageCount] = {};
    if (!work_page_order_copy(order, sizeof(order))) {
        return 0;
    }
    int count = 0;
    for (int order_index = 0; order_index < kWorkPageCount && (size_t)count < capacity; ++order_index) {
        if (is_work_page_enabled(order[order_index])) {
            indices[count] = order_index;
            pages[count] = order[order_index];
            ++count;
        }
    }
    return count;
}

#define SETTINGS_PRIMARY_LABEL_CREATE_FAILED_FORMAT "settings primary label create failed index=%d"
#define SETTINGS_SECONDARY_LABEL_CREATE_FAILED_FORMAT "settings secondary label create failed index=%d"
#define SETTINGS_SWITCH_DOT_CREATE_FAILED_FORMAT "settings switch dot create failed index=%d"

constexpr int kSettingsPrimaryX = 12;
constexpr int kSettingsPrimaryW = 112;
constexpr int kSettingsSecondaryX = 150;
constexpr int kSettingsSecondaryW = 228;
constexpr int kSettingsSecondaryH = 30;
constexpr int kSettingsSwitchDotX = 362;
constexpr int kSettingsSwitchDotYOffset = 8;
constexpr int kSettingsSwitchDotSize = 12;
constexpr int kSettingsListRowY[] = {66, 105, 144, 183, 222, 222, 222};
constexpr int kSettingsGridRowY[] = {66, 105, 144, 183};
constexpr size_t kSettingsListRowCount = array_count(kSettingsListRowY);
constexpr size_t kSettingsGridRowCount = array_count(kSettingsGridRowY);
constexpr int kSettingsGridColumns = 2;
constexpr int kSettingsGridLeftX = 150;
constexpr int kSettingsGridRightX = 267;
constexpr int kSettingsGridColW = 111;
constexpr int kSettingsGridSwitchDotXOffset = 92;
constexpr int kSettingsGridSwitchDotYOffset = 9;
constexpr int kSettingsSystemLongItemY = 144;
constexpr int kSettingsDisplayLongItemY = 183;
constexpr const char *kSettingsPrimaryItems[kSettingsPrimaryCount] = {"网络", "声音", "显示", "系统"};
constexpr const char *kSettingsPageOrderEntryFormat = "%d %s";
#define SETTINGS_SWITCH_SLOT_INDEX_OUT_OF_RANGE_FORMAT "settings switch slot index out of range: %d"
constexpr const char *kSettingsLabelPlaceholder = "--";
constexpr const char *kSettingsFixedTexts[] = {
    kSettingsLabelPlaceholder,
};
constexpr const char *kBootSettingsLogTexts[] = {
    SETTINGS_PRIMARY_LABEL_CREATE_FAILED_FORMAT,
    SETTINGS_SECONDARY_LABEL_CREATE_FAILED_FORMAT,
    SETTINGS_SWITCH_DOT_CREATE_FAILED_FORMAT,
    SETTINGS_SWITCH_SLOT_INDEX_OUT_OF_RANGE_FORMAT,
};

struct SettingsGridCell {
    int x;
    int y;
};

enum SettingsMenuColumn {
    kSettingsMenuPrimaryColumn,
    kSettingsMenuSecondaryColumn,
};

SettingsGridCell settings_grid_cell(int index)
{
    int col = index % kSettingsGridColumns;
    int row = index / kSettingsGridColumns;
    return {
        col == 0 ? kSettingsGridLeftX : kSettingsGridRightX,
        kSettingsGridRowY[row],
    };
}

int settings_long_item_y(int primary)
{
    return primary == kSettingsPrimarySystem ? kSettingsSystemLongItemY : kSettingsDisplayLongItemY;
}

static_assert(kSettingsListRowCount == kSettingsSecondaryMaxCount,
              "settings list rows must match secondary slot count");
static_assert(kSettingsGridRowCount * kSettingsGridColumns >= kWorkPageCount,
              "settings grid capacity must cover all work pages");
static_assert(kSettingsGridRowCount * kSettingsGridColumns >= kSystemSettingsGridItemCount,
              "settings grid capacity must cover system grid items");
static_assert(array_count(kSettingsPrimaryItems) == kSettingsPrimaryCount,
              "settings primary item table must match primary count");
static_assert(array_count(g_settings_labels) == kSettingsLabelCount,
              "settings label storage must match configured label count");
static_assert(array_count(g_settings_switch_dots) == kSettingsSecondaryMaxCount,
              "settings switch dot storage must match secondary slot count");
static_assert(array_count(kSettingsFixedTexts) > 0, "settings fixed text registry must not be empty");
static_assert(array_count(kBootSettingsLogTexts) > 0, "boot/settings log registry must not be empty");
static_assert(cstr_array_nonempty(kSettingsPrimaryItems), "settings primary menu texts must be non-empty");
static_assert(cstr_array_nonempty(kSettingsFixedTexts), "settings fixed texts must be non-empty");
static_assert(cstr_array_nonempty(kBootSettingsLogTexts), "boot/settings log texts must be non-empty");
static_assert(kSettingsGridColumns > 0, "settings grid must have columns");
static_assert(kSettingsGridColW > 0 && kSettingsSecondaryH > 0,
              "settings grid item size must be positive");
static_assert(kSettingsSystemLongItemY >= 0 && kSettingsDisplayLongItemY >= 0,
              "settings long item y positions must be non-negative");
static_assert(kSettingsListRowCount >= kSettingsPrimaryCount,
              "settings list rows must fit primary menu items");

void hide_settings_switch_slot(int index)
{
    if (!settings_secondary_index_valid(index)) {
        ESP_LOGW(TAG, SETTINGS_SWITCH_SLOT_INDEX_OUT_OF_RANGE_FORMAT, index);
        return;
    }
    if (g_settings_switch_dots[index]) {
        set_obj_visible(g_settings_switch_dots[index], false);
    }
}

lv_obj_t *build_settings_menu_label(lv_obj_t *screen,
                                    int x,
                                    int y,
                                    int width,
                                    int index,
                                    SettingsMenuColumn column)
{
    lv_obj_t *label = make_label(screen,
                                 x,
                                 y,
                                 width,
                                 kSettingsSecondaryH,
                                 kSettingsLabelPlaceholder);
    if (!label) {
        if (column == kSettingsMenuPrimaryColumn) {
            ESP_LOGW(TAG, SETTINGS_PRIMARY_LABEL_CREATE_FAILED_FORMAT, index);
        } else {
            ESP_LOGW(TAG, SETTINGS_SECONDARY_LABEL_CREATE_FAILED_FORMAT, index);
        }
        return nullptr;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    center_align_label(label);
    return label;
}

}

void style_settings_item(lv_obj_t *label, bool selected)
{
    lv_obj_set_style_bg_color(label, selected ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(label, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(label, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(label, 5, LV_PART_MAIN);
}

static void style_settings_switch_dot(lv_obj_t *dot, bool on, bool selected)
{
    if (!dot) {
        return;
    }
    lv_color_t fg = selected ? lv_color_white() : lv_color_black();
    lv_color_t bg = selected ? lv_color_black() : lv_color_white();
    lv_obj_set_style_bg_color(dot, on ? fg : bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dot, fg, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
}

void build_settings_page()
{
    if (g_settings_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    g_settings_root = screen;
    lv_obj_add_flag(g_settings_root, LV_OBJ_FLAG_HIDDEN);

    make_centered_label(screen,
                        24,
                        18,
                        352,
                        28,
                        "设置",
                        "settings title create failed");
    make_black_bar(screen, 24, 52, 352, 3);

    make_black_bar(screen, 136, 62, 2, 174);

    for (int i = 0; i < kSettingsPrimaryCount; ++i) {
        g_settings_labels[i] = build_settings_menu_label(screen,
                                                         kSettingsPrimaryX,
                                                         kSettingsListRowY[i],
                                                         kSettingsPrimaryW,
                                                         i,
                                                         kSettingsMenuPrimaryColumn);
    }
    for (int i = 0; i < kSettingsSecondaryMaxCount; ++i) {
        int slot = kSettingsPrimaryCount + i;
        g_settings_labels[slot] = build_settings_menu_label(screen,
                                                            kSettingsSecondaryX,
                                                            kSettingsListRowY[i],
                                                            kSettingsSecondaryW,
                                                            i,
                                                            kSettingsMenuSecondaryColumn);
        g_settings_switch_dots[i] = lv_obj_create(screen);
        if (g_settings_switch_dots[i]) {
            lv_obj_clear_flag(g_settings_switch_dots[i], LV_OBJ_FLAG_SCROLLABLE);
            set_obj_box(g_settings_switch_dots[i],
                        kSettingsSwitchDotX,
                        kSettingsListRowY[i] + kSettingsSwitchDotYOffset,
                        kSettingsSwitchDotSize,
                        kSettingsSwitchDotSize);
            style_settings_switch_dot(g_settings_switch_dots[i], false, false);
            lv_obj_add_flag(g_settings_switch_dots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGW(TAG, SETTINGS_SWITCH_DOT_CREATE_FAILED_FORMAT, i);
        }
    }
    build_settings_ota_panel(screen, kSettingsSecondaryX, kSettingsSecondaryW);

    g_settings_feedback_label = make_centered_label(screen,
                                                    24,
                                                    246,
                                                    352,
                                                    20,
                                                    "",
                                                    "settings feedback label create failed");

    make_centered_label(screen,
                        24,
                        270,
                        352,
                        22,
                        "KEY选择  长按返回  BOOT确认",
                        "settings hint label create failed");
}

namespace {
bool settings_render_selection_changed(int primary, int selected)
{
    static lv_obj_t *last_settings_root = nullptr;
    static int last_primary = -1;
    static int last_selected = -1;
    static bool last_focus_secondary = false;
    static int last_ota_state = -1;
    static int last_ota_progress = -2;
    static int last_ota_speed = -2;
    static bool last_page_order_mode = false;
    static bool last_page_toggle_mode = false;
    static int last_page_order_selection = -1;
    OtaRuntimeSnapshot ota;
    ota_runtime_snapshot_load(&ota);
    bool selection_changed = g_settings_root != last_settings_root ||
                             selected != last_selected ||
                             primary != last_primary ||
                             g_settings_focus_secondary != last_focus_secondary ||
                             g_settings_page_toggle_mode != last_page_toggle_mode ||
                             g_settings_page_order_mode != last_page_order_mode ||
                             g_settings_page_order_selection != last_page_order_selection ||
                             ota.state != last_ota_state ||
                             ota.progress != last_ota_progress ||
                             ota.speed_kbps != last_ota_speed;
    if (selection_changed) {
        last_settings_root = g_settings_root;
        last_selected = selected;
        last_primary = primary;
        last_focus_secondary = g_settings_focus_secondary;
        last_page_toggle_mode = g_settings_page_toggle_mode;
        last_page_order_mode = g_settings_page_order_mode;
        last_page_order_selection = g_settings_page_order_selection;
        last_ota_state = ota.state;
        last_ota_progress = ota.progress;
        last_ota_speed = ota.speed_kbps;
    }
    return selection_changed;
}

bool update_settings_primary_items(int primary, bool selection_changed)
{
    bool changed = false;
    for (int i = 0; i < kSettingsPrimaryCount; ++i) {
        if (g_settings_labels[i]) {
            changed |= set_label_text_if_changed(g_settings_labels[i], kSettingsPrimaryItems[i]);
            if (selection_changed) {
                style_settings_item(g_settings_labels[i], i == primary);
            }
        }
    }
    return changed;
}

bool layout_settings_secondary_slot(
    int index,
    int primary,
    int visible_order_count,
    const uint8_t *visible_order_pages,
    char secondary_items[][kSettingsSecondaryTextSize])
{
    int slot = kSettingsPrimaryCount + index;
    if (g_settings_page_order_mode || g_settings_page_toggle_mode) {
        int manager_item_count = g_settings_page_order_mode ? visible_order_count : kWorkPageCount;
        if (index >= manager_item_count) {
            set_obj_visible(g_settings_labels[slot], false);
            hide_settings_switch_slot(index);
            return false;
        }
        SettingsGridCell cell = settings_grid_cell(index);
        lv_obj_set_pos(g_settings_labels[slot], cell.x, cell.y);
        lv_obj_set_size(g_settings_labels[slot], kSettingsGridColW, kSettingsSecondaryH);
        if (g_settings_page_order_mode) {
            format_secondary_text(secondary_items,
                                  index,
                                  kSettingsPageOrderEntryFormat,
                                  index + 1,
                                  work_page_name(visible_order_pages[index]));
            hide_settings_switch_slot(index);
        } else {
            set_secondary_text(secondary_items, index, work_page_name(index));
            if (g_settings_switch_dots[index]) {
                lv_obj_set_pos(g_settings_switch_dots[index],
                               cell.x + kSettingsGridSwitchDotXOffset,
                               cell.y + kSettingsGridSwitchDotYOffset);
            }
        }
    } else if (primary == kSettingsPrimarySystem) {
        bool grid_item = index < kSystemSettingsGridItemCount;
        if (grid_item) {
            SettingsGridCell cell = settings_grid_cell(index);
            lv_obj_set_pos(g_settings_labels[slot], cell.x, cell.y);
            lv_obj_set_size(g_settings_labels[slot], kSettingsGridColW, kSettingsSecondaryH);
            if (g_settings_switch_dots[index]) {
                lv_obj_set_pos(g_settings_switch_dots[index],
                               cell.x + kSettingsGridSwitchDotXOffset,
                               cell.y + kSettingsGridSwitchDotYOffset);
            }
        } else {
            lv_obj_set_pos(g_settings_labels[slot],
                           kSettingsSecondaryX,
                           settings_long_item_y(primary));
            lv_obj_set_size(g_settings_labels[slot], kSettingsSecondaryW, kSettingsSecondaryH);
            hide_settings_switch_slot(index);
        }
    } else {
        lv_obj_set_pos(g_settings_labels[slot], kSettingsSecondaryX, kSettingsListRowY[index]);
        lv_obj_set_size(g_settings_labels[slot], kSettingsSecondaryW, kSettingsSecondaryH);
        if (g_settings_switch_dots[index]) {
            lv_obj_set_pos(g_settings_switch_dots[index],
                           kSettingsSwitchDotX,
                           kSettingsListRowY[index] + kSettingsSwitchDotYOffset);
        }
    }
    return true;
}

void update_settings_switch_slot(int index, int primary, int selected, bool visible)
{
    bool dot_visible = false;
    bool dot_on = false;
    if (visible && primary == kSettingsPrimarySound) {
        if (index >= kSoundSettingsHourlyItem) {
            dot_visible = true;
            dot_on = index == kSoundSettingsHourlyItem ? g_hourly_chime_enabled : g_hourly_chime_all_day;
        }
    } else if (visible &&
               primary == kSettingsPrimaryDisplay &&
               g_settings_page_toggle_mode) {
        dot_visible = true;
        dot_on = is_work_page_enabled(index);
    } else if (visible &&
               primary == kSettingsPrimaryDisplay &&
               !g_settings_page_toggle_mode &&
               !g_settings_page_order_mode &&
               (index == kDisplaySettingsAlarmItem ||
                index == kDisplaySettingsXiaozhiAutoReturnItem)) {
        dot_visible = true;
        dot_on = index == kDisplaySettingsAlarmItem
                     ? alarm_is_enabled()
                     : g_xiaozhi_auto_return_enabled.load(std::memory_order_acquire);
    }
    if (g_settings_switch_dots[index]) {
        set_obj_visible(g_settings_switch_dots[index], dot_visible);
        if (dot_visible) {
            style_settings_switch_dot(g_settings_switch_dots[index],
                                      dot_on,
                                      g_settings_focus_secondary && index == selected);
        }
    }
}

bool update_settings_secondary_items(
    int primary,
    int selected,
    bool selection_changed,
    char secondary_items[][kSettingsSecondaryTextSize])
{
    bool changed = false;
    int secondary_count = settings_secondary_count(primary);
    int visible_order_indices[kWorkPageCount] = {};
    uint8_t visible_order_pages[kWorkPageCount] = {};
    int visible_order_count = g_settings_page_order_mode
                                  ? collect_visible_work_page_order(visible_order_indices,
                                                                    visible_order_pages,
                                                                    array_count(visible_order_indices))
                                  : 0;
    for (int i = 0; i < kSettingsSecondaryMaxCount; ++i) {
        int slot = kSettingsPrimaryCount + i;
        if (!g_settings_labels[slot]) {
            continue;
        }
        if (!layout_settings_secondary_slot(i,
                                            primary,
                                            visible_order_count,
                                            visible_order_pages,
                                            secondary_items)) {
            continue;
        }
        bool visible = i < secondary_count;
        if (g_settings_page_order_mode) {
            visible = i < visible_order_count;
        } else if (g_settings_page_toggle_mode) {
            visible = i < kWorkPageCount;
        }
        set_obj_visible(g_settings_labels[slot], visible);
        if (visible) {
            changed |= set_label_text_if_changed(g_settings_labels[slot], secondary_items[i]);
            if (selection_changed) {
                int order_index = g_settings_page_order_mode ? visible_order_indices[i] : -1;
                bool selected_item = g_settings_page_order_mode ? order_index == g_settings_page_order_selection :
                                     g_settings_page_toggle_mode ? i == selected :
                                     (g_settings_focus_secondary && i == selected);
                style_settings_item(g_settings_labels[slot], selected_item);
                if (primary == kSettingsPrimarySystem && i < kSystemSettingsGridItemCount) {
                    lv_obj_set_style_pad_left(g_settings_labels[slot], 4, LV_PART_MAIN);
                    lv_obj_set_style_pad_right(g_settings_labels[slot], 4, LV_PART_MAIN);
                }
            }
        }
        update_settings_switch_slot(i, primary, selected, visible);
    }
    return changed;
}

bool update_settings_feedback_label()
{
    if (!g_settings_feedback_label) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    char feedback[kSettingsFeedbackTextLen] = {};
    if (settings_feedback_copy_active(now, feedback, sizeof(feedback))) {
        return set_label_text_if_changed(g_settings_feedback_label, feedback);
    }
    return set_label_text_if_changed(g_settings_feedback_label, "");
}
} // namespace

bool update_settings_page()
{
    ota_reset_status_if_idle();
    char secondary_items[kSettingsSecondaryMaxCount][kSettingsSecondaryTextSize] = {};
    int primary = clamp_settings_primary(g_settings_primary_selection);
    int selected = clamp_settings_selection_for_mode(primary,
                                                     g_settings_selection,
                                                     g_settings_page_toggle_mode);
    g_settings_primary_selection = primary;
    g_settings_selection = selected;
    if (g_settings_page_order_mode) {
        normalize_work_page_order();
        g_settings_page_order_selection =
            valid_enabled_work_page_order_index(g_settings_page_order_selection);
    }

    populate_settings_secondary_items(primary, secondary_items);
    bool selection_changed = settings_render_selection_changed(primary, selected);
    bool changed = selection_changed;
    changed |= update_settings_primary_items(primary, selection_changed);
    changed |= update_settings_secondary_items(primary,
                                               selected,
                                               selection_changed,
                                               secondary_items);
    bool ota_panel_visible = primary == kSettingsPrimarySystem && selected == kSystemSettingsOtaItem;
    changed |= update_settings_ota_panel(ota_panel_visible);
    changed |= update_settings_feedback_label();
    return changed;
}
