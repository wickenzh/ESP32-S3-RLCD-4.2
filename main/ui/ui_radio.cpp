// 构建并刷新网络电台工作页，展示电台名称、状态和播放动画。
#include "ui_views.h"

#include "radio_services.h"
#include "ui_battery.h"

namespace {

constexpr int kTopLineX = 18;
constexpr int kTopLineY = 54;
constexpr int kTopLineW = 364;
constexpr int kTopLineH = 4;

// 电台信息面板
constexpr int kInfoPanelX = 18;
constexpr int kInfoPanelY = 70;
constexpr int kInfoPanelW = 364;
constexpr int kInfoPanelH = 210;
constexpr int kInfoPanelRadius = 18;

// 电台名称
constexpr int kStationNameX = 36;
constexpr int kStationNameY = 86;
constexpr int kStationNameW = 328;
constexpr int kStationNameH = 40;

// 状态文本
constexpr int kStatusX = 36;
constexpr int kStatusY = 140;
constexpr int kStatusW = 328;
constexpr int kStatusH = 28;

// 播放时长
constexpr int kUptimeX = 36;
constexpr int kUptimeY = 176;
constexpr int kUptimeW = 160;
constexpr int kUptimeH = 24;

// 电台序号
constexpr int kIndexX = 236;
constexpr int kIndexY = 176;
constexpr int kIndexW = 128;
constexpr int kIndexH = 24;

// 提示文字
constexpr int kHintX = 36;
constexpr int kHintY = 220;
constexpr int kHintW = 328;
constexpr int kHintH = 24;

// 动画点间隔
constexpr uint32_t kAnimDotsIntervalMs = 500;

// 上一次显示的状态
RadioState s_last_state = kRadioIdle;
char s_last_station_name[32] = {};
char s_last_status[64] = {};
uint32_t s_last_uptime = 0xFFFFFFFF;
int s_last_station_index = -1;

// 预设的动画符号
constexpr const char *kAnimSymbols[] = {"|/", "-\\\\"};
constexpr int kAnimSymbolCount = 2;

} // namespace

void build_radio_page()
{
    if (g_radio_root) {
        return;
    }
    g_radio_root = create_page_root();
    if (!g_radio_root) {
        return;
    }
    lv_obj_add_flag(g_radio_root, LV_OBJ_FLAG_HIDDEN);

    build_battery_icon(g_radio_root, g_radio_battery_segments);
    build_work_page_status_bar(g_radio_root,
                               kWorkPageRadio,
                               &g_radio_date_label,
                               nullptr,
                               nullptr,
                               false);

    // 顶部分隔线
    lv_obj_t *top_line = make_bar(g_radio_root, kTopLineX, kTopLineY, kTopLineW, kTopLineH);
    set_obj_black(top_line, true);
    build_work_page_day_progress(g_radio_root, kWorkPageRadio);

    // 电台信息面板（深色背景）
    lv_obj_t *panel = make_bar(g_radio_root,
                                kInfoPanelX,
                                kInfoPanelY,
                                kInfoPanelW,
                                kInfoPanelH);
    set_obj_black(panel, true);
    if (panel) {
        lv_obj_set_style_radius(panel, kInfoPanelRadius, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(panel, true, LV_PART_MAIN);
    }

    // 电台名称
    g_radio_station_label = make_label_with_font(g_radio_root,
                                                   kStationNameX,
                                                   kStationNameY,
                                                   kStationNameW,
                                                   kStationNameH,
                                                   "网络电台",
                                                   &zh_font_16);
    if (g_radio_station_label) {
        lv_obj_set_style_text_color(g_radio_station_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(g_radio_station_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    }

    // 粗体叠加
    g_radio_station_bold_label = make_label_with_font(g_radio_root,
                                                        kStationNameX + 1,
                                                        kStationNameY,
                                                        kStationNameW,
                                                        kStationNameH,
                                                        "网络电台",
                                                        &zh_font_16);
    if (g_radio_station_bold_label) {
        lv_obj_set_style_text_color(g_radio_station_bold_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(g_radio_station_bold_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    }

    // 状态文本
    g_radio_status_label = make_label_with_font(g_radio_root,
                                                  kStatusX,
                                                  kStatusY,
                                                  kStatusW,
                                                  kStatusH,
                                                  "等待连接",
                                                  &zh_font_16);
    if (g_radio_status_label) {
        lv_obj_set_style_text_color(g_radio_status_label, lv_color_white(), LV_PART_MAIN);
    }

    // 播放时长
    g_radio_uptime_label = make_label_with_font(g_radio_root,
                                                   kUptimeX,
                                                   kUptimeY,
                                                   kUptimeW,
                                                   kUptimeH,
                                                   "",
                                                   &lv_font_montserrat_16);
    if (g_radio_uptime_label) {
        lv_obj_set_style_text_color(g_radio_uptime_label, lv_color_make(0x99, 0x99, 0x99), LV_PART_MAIN);
    }

    // 电台序号
    g_radio_index_label = make_label_with_font(g_radio_root,
                                                 kIndexX,
                                                 kIndexY,
                                                 kIndexW,
                                                 kIndexH,
                                                 "",
                                                 &lv_font_montserrat_16);
    if (g_radio_index_label) {
        lv_obj_set_style_text_color(g_radio_index_label, lv_color_make(0x99, 0x99, 0x99), LV_PART_MAIN);
        lv_obj_set_style_text_align(g_radio_index_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    }

    // 操作提示
    g_radio_hint_label = make_label_with_font(g_radio_root,
                                                kHintX,
                                                kHintY,
                                                kHintW,
                                                kHintH,
                                                "KEY键切台 / 10秒后自动播放",
                                                &zh_font_16);
    if (g_radio_hint_label) {
        lv_obj_set_style_text_color(g_radio_hint_label, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    }

    update_battery_segments(g_radio_battery_segments, battery_percent_load());
}

bool update_radio_page(const struct tm &local)
{
    if (!g_radio_root) {
        build_radio_page();
    }

    RadioSnapshot snapshot = {};
    radio_get_snapshot(&snapshot);

    bool changed = false;

    // 更新电台名称
    if (strcmp(s_last_station_name, snapshot.station_name) != 0) {
        strlcpy(s_last_station_name, snapshot.station_name, sizeof(s_last_station_name));
        changed |= set_label_text_if_changed(g_radio_station_label, snapshot.station_name);
        changed |= set_label_text_if_changed(g_radio_station_bold_label, snapshot.station_name);
    }

    // 更新状态文本（带动画点）
    char status_buf[80] = {};
    if (snapshot.state == kRadioConnecting || snapshot.state == kRadioPlaying) {
        // 添加动画点
        int dots = 1 + static_cast<int>((lv_tick_get() / kAnimDotsIntervalMs) % 3U);
        snprintf(status_buf, sizeof(status_buf), "%s%.*s",
                 snapshot.status_text, dots, "...");
        changed |= set_label_text_if_changed(g_radio_status_label, status_buf);
        // 如果正在播放，也触发刷新以更新动画点
        if (snapshot.state == kRadioPlaying || snapshot.state == kRadioConnecting) {
            changed = true;
        }
    } else {
        changed |= set_label_text_if_changed(g_radio_status_label, snapshot.status_text);
    }

    // 更新播放时长
    if (snapshot.uptime_sec != s_last_uptime) {
        s_last_uptime = snapshot.uptime_sec;
        if (snapshot.state == kRadioPlaying && snapshot.uptime_sec > 0) {
            uint32_t mins = snapshot.uptime_sec / 60;
            uint32_t secs = snapshot.uptime_sec % 60;
            char uptime_str[32] = {};
            snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d", (int)mins, (int)secs);
            changed |= set_label_text_if_changed(g_radio_uptime_label, uptime_str);
        } else {
            changed |= set_label_text_if_changed(g_radio_uptime_label, "");
        }
    }

    // 更新电台序号
    int cur_idx = radio_current_station_index();
    if (cur_idx != s_last_station_index) {
        s_last_station_index = cur_idx;
        char idx_str[32] = {};
        snprintf(idx_str, sizeof(idx_str), "%d / %d", cur_idx + 1, radio_station_count());
        changed |= set_label_text_if_changed(g_radio_index_label, idx_str);
    }

    // 更新状态图标
    changed |= update_work_page_status_icons(kWorkPageRadio);

    s_last_state = snapshot.state;
    return changed;
}
