// 构建设置页网络、声音、显示、系统和页面排序 SDL 静态预览。
#include "sdl_preview_settings.h"
#include "ui_settings_visual_style.h"

#include "sdl_preview_widgets.h"
#include "ui_settings_layout.h"

#include <string.h>

namespace {
namespace settings_layout = ui_settings_layout;

using sdl_preview_widgets::make_black_bar;
using sdl_preview_widgets::make_label;

bool settings_preview_mode_is(const char *mode, const char *expected)
{
    return mode && expected && strcmp(mode, expected) == 0;
}

void style_settings_item(lv_obj_t *label, bool selected, bool primary=false)
{
    settings_visual_style(label, selected, primary);
}

lv_obj_t *make_settings_item(lv_obj_t *screen,
                             int x,
                             int y,
                             int w,
                             int h,
                             const char *text,
                             bool selected)
{
    lv_obj_t *label = make_label(screen, x, y, w, h, text);
    if (!label) {
        return nullptr;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    style_settings_item(label, selected, x==settings_layout::kSettingsPrimaryX);
    if(x==settings_layout::kSettingsPrimaryX) settings_attach_category_marker(label);
    return label;
}

void make_settings_switch_text(lv_obj_t *screen, int x, int y, const char *text, bool selected)
{
    lv_obj_t *label = make_label(screen, x, y, 28, 18, text);
    if (!label) {
        return;
    }
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), LV_PART_MAIN);
}

void make_settings_switch_dot(lv_obj_t *screen, int x, int y, bool on, bool selected)
{
    lv_obj_t *dot = lv_obj_create(screen);
    if (!dot) {
        return;
    }
    lv_color_t foreground = selected ? lv_color_white() : lv_color_black();
    lv_color_t background = selected ? lv_color_black() : lv_color_white();
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot,
                    settings_layout::kSettingsSwitchDotSize,
                    settings_layout::kSettingsSwitchDotSize);
    lv_obj_set_style_bg_color(dot, on ? foreground : background, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dot, foreground, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
}

void make_settings_grid_item(lv_obj_t *screen,
                             int x,
                             int y,
                             const char *text,
                             bool selected,
                             const char *switch_text = nullptr,
                             bool reserve_switch_dot = false)
{
    lv_obj_t *label = make_settings_item(screen,
                                         x,
                                         y,
                                         settings_layout::kSettingsGridColW,
                                         settings_layout::kSettingsSecondaryH,
                                         text,
                                         selected);
    if (label) {
        lv_obj_set_style_pad_left(
            label,
            reserve_switch_dot
                ? settings_layout::kSettingsGridSwitchLabelLeftPadding
                : settings_layout::kSettingsGridLabelPadding,
            LV_PART_MAIN);
        lv_obj_set_style_pad_right(
            label,
            reserve_switch_dot
                ? settings_layout::kSettingsGridSwitchLabelRightPadding
                : settings_layout::kSettingsGridLabelPadding,
            LV_PART_MAIN);
    }
    if (switch_text) {
        make_settings_switch_text(screen,
                                  x + settings_layout::kSettingsGridSwitchTextXOffset,
                                  y + settings_layout::kSettingsGridSwitchTextYOffset,
                                  switch_text,
                                  selected);
    }
}
} // namespace

void build_settings_preview_page(const char *mode)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(screen, 24, 18, 352, 28, "设置");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    make_black_bar(screen, 24, 52, 352, 3);
    make_black_bar(screen, 136, 62, 2, 174);

    int primary = 0;
    if (settings_preview_mode_is(mode, "settings_sound")) {
        primary = 1;
    } else if (settings_preview_mode_is(mode, "settings_display") ||
               settings_preview_mode_is(mode, "settings_pages") ||
               settings_preview_mode_is(mode, "settings_order")) {
        primary = 2;
    } else if (settings_preview_mode_is(mode, "settings_system")) {
        primary = 3;
    }

    static const char *primary_items[] = {"网络", "声音", "显示", "系统"};
    for (int i = 0; i < 4; ++i) {
        make_settings_item(screen,
                           settings_layout::kSettingsPrimaryX,
                           settings_layout::kSettingsListRowY[i],
                           settings_layout::kSettingsPrimaryW,
                           settings_layout::kSettingsSecondaryH,
                           primary_items[i],
                           i == primary);
    }

    lv_obj_t *feedback_label = nullptr;
    if (settings_preview_mode_is(mode, "settings_order")) {
        static const char *order_items[] = {
            "1 天气时钟", "2 图片时钟", "3 天气看板",
            "4 温湿时钟", "5 日历", "6 温湿历史", "7 小智AI", "8 聚合时钟",
        };
        for (int i = 0; i < 8; ++i) {
            settings_layout::GridCell cell = settings_layout::settings_grid_cell(i);
            make_settings_grid_item(screen, cell.x, cell.y, order_items[i], i == 3);
        }
        feedback_label = make_label(screen, 24, 246, 352, 20, "长按 KEY 保存返回");
    } else if (primary == 0) {
        static const char *network_items[] = {"同步时间", "同步天气", "更新一言", "天气城市 已设置"};
        for (int i = 0; i < 4; ++i) {
            make_settings_item(screen,
                               settings_layout::kSettingsSecondaryX,
                               settings_layout::kSettingsListRowY[i],
                               settings_layout::kSettingsSecondaryW,
                               settings_layout::kSettingsSecondaryH,
                               network_items[i],
                               i == 3 && !settings_preview_mode_is(mode,"settings"));
        }
        feedback_label = make_label(screen, 24, 246, 352, 20, "手动城市优先，BOOT 清除");
    } else if (primary == 1) {
        static const char *sound_items[] = {"音量 80%", "声音选择 1", "整点提醒 7:00 - 22:00", "全天提醒 0:00 - 24:00"};
        for (int i = 0; i < 4; ++i) {
            make_settings_item(screen,
                               settings_layout::kSettingsSecondaryX,
                               settings_layout::kSettingsListRowY[i],
                               settings_layout::kSettingsSecondaryW,
                               settings_layout::kSettingsSecondaryH,
                               sound_items[i],
                               i == 0);
            if (i >= 2) {
                make_settings_switch_text(screen,
                                          352,
                                          settings_layout::kSettingsListRowY[i] + 6,
                                          i == 2 ? "开" : "关",
                                          i == 0);
            }
        }
        feedback_label = make_label(screen, 24, 246, 352, 20, "BOOT 调整并试听");
    } else if (settings_preview_mode_is(mode, "settings_pages")) {
        static const char *display_items[] = {
            "天气时钟", "图片时钟", "天气看板",
            "温湿时钟", "日历", "温湿历史", "小智AI", "聚合时钟",
        };
        for (int i = 0; i < 8; ++i) {
            settings_layout::GridCell cell = settings_layout::settings_grid_cell(i);
            make_settings_grid_item(screen, cell.x, cell.y, display_items[i], i == 2, "开");
        }
        feedback_label = make_label(screen, 24, 246, 352, 20, "页面可开关，也可排序");
    } else if (primary == 2) {
        static const char *display_items[] = {
            "页面开关", "页面顺序", "小智节能", "闹钟 07:30", "图片切换 6h",
        };
        for (int i = 0; i < 5; ++i) {
            settings_layout::GridCell cell = settings_layout::settings_grid_cell(i);
            make_settings_grid_item(screen,
                                    cell.x,
                                    cell.y,
                                    display_items[i],
                                    i == 4,
                                    nullptr,
                                    i == 3);
        }
        settings_layout::GridCell auto_return_cell = settings_layout::settings_grid_cell(2);
        settings_layout::GridCell alarm_cell = settings_layout::settings_grid_cell(3);
        make_settings_switch_dot(screen,
                                 alarm_cell.x + settings_layout::kSettingsGridSwitchDotXOffset,
                                 alarm_cell.y + settings_layout::kSettingsGridSwitchDotYOffset,
                                 true,
                                 false);
        make_settings_switch_dot(screen,
                                 auto_return_cell.x + settings_layout::kSettingsGridSwitchDotXOffset,
                                 auto_return_cell.y + settings_layout::kSettingsGridSwitchDotYOffset,
                                 true,
                                 false);
        feedback_label = make_label(screen, 24, 246, 352, 20, "仅自定义图片可调整切换时间");
    } else {
        settings_layout::GridCell offline_cell = settings_layout::settings_grid_cell(0);
        settings_layout::GridCell diagnostic_cell = settings_layout::settings_grid_cell(1);
        settings_layout::GridCell reset_cell = settings_layout::settings_grid_cell(2);
        settings_layout::GridCell info_cell = settings_layout::settings_grid_cell(3);
        make_settings_grid_item(screen, offline_cell.x, offline_cell.y, "离线模式", false, "关");
        make_settings_grid_item(screen, diagnostic_cell.x, diagnostic_cell.y, "网络检测", false);
        make_settings_grid_item(screen, reset_cell.x, reset_cell.y, "恢复出厂设置", false);
        make_settings_grid_item(screen, info_cell.x, info_cell.y, "关于本机", true);
        make_settings_item(screen,
                           settings_layout::kSettingsSecondaryX,
                           settings_layout::kSettingsSystemLongItemY,
                           settings_layout::kSettingsSecondaryW,
                           settings_layout::kSettingsSecondaryH,
                           "检查更新",
                           false);
        lv_obj_t *ota = make_label(screen,
                                   settings_layout::kSettingsSecondaryX,
                                   176,
                                   settings_layout::kSettingsSecondaryW,
                                   22,
                                   "当前版本 v1.4.52");
        lv_obj_set_style_text_align(ota, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_t *hint_line = make_label(screen,
                                         settings_layout::kSettingsSecondaryX,
                                         218,
                                         settings_layout::kSettingsSecondaryW,
                                         20,
                                         "BOOT开始检查");
        lv_obj_set_style_text_align(hint_line, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        feedback_label = make_label(screen, 24, 246, 352, 20, "");
    }
    lv_obj_set_style_text_align(feedback_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *hint = make_label(screen, 24, 270, 352, 22, "KEY选择  长按返回  BOOT确认");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}
