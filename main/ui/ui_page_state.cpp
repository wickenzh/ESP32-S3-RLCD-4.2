// 管理页面根对象、可见性、工作页顺序和低电量显示状态。
#include "ui_views.h"

#include "network_services.h"

lv_obj_t *create_page_root()
{
    lv_obj_t *root = lv_obj_create(lv_scr_act());
    if (!root) {
        ESP_LOGW(TAG, "page root create failed");
        return nullptr;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, kDisplayWidth, kDisplayHeight);
    lv_obj_set_style_bg_color(root, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    return root;
}

void set_page_visible(lv_obj_t *page, bool visible)
{
    if (!page) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page);
    } else {
        lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    }
}

void show_page(lv_obj_t *page)
{
    set_page_visible(g_clock_root, page == g_clock_root);
    set_page_visible(g_history_root, page == g_history_root);
    set_page_visible(g_gallery_root, page == g_gallery_root);
    set_page_visible(g_calendar_root, page == g_calendar_root);
    set_page_visible(g_weather_board_root, page == g_weather_board_root);
    set_page_visible(g_flip_clock_root, page == g_flip_clock_root);
    set_page_visible(g_info_root, page == g_info_root);
    set_page_visible(g_network_diag_root, page == g_network_diag_root);
    set_page_visible(g_settings_root, page == g_settings_root);
}

lv_obj_t *active_work_page_root()
{
    if (g_low_battery_mode || g_setup_portal_active) {
        g_active_work_page = kWorkPageWeatherClock;
    }
    ensure_active_work_page_enabled();
    if (g_active_work_page == kWorkPageWeatherClock) {
        build_clock_ui();
        return g_clock_root;
    }
    if (g_active_work_page == kWorkPageHistory) {
        build_history_page();
        return g_history_root ? g_history_root : g_clock_root;
    }
    if (g_active_work_page == kWorkPageGallery) {
        build_gallery_page();
        return g_gallery_root ? g_gallery_root : g_clock_root;
    }
    if (g_active_work_page == kWorkPageCalendar) {
        build_calendar_page();
        return g_calendar_root ? g_calendar_root : g_clock_root;
    }
    if (g_active_work_page == kWorkPageWeatherBoard) {
        build_weather_board_page();
        return g_weather_board_root ? g_weather_board_root : g_clock_root;
    }
    if (g_active_work_page == kWorkPageFlipClock) {
        build_flip_clock_page();
        return g_flip_clock_root ? g_flip_clock_root : g_clock_root;
    }
    return g_clock_root;
}

void show_active_work_page()
{
    show_page(active_work_page_root());
}

bool is_work_page_enabled(int page)
{
    if (page <= 0) {
        return true;
    }
    if (page >= kWorkPageCount) {
        return false;
    }
    return (g_work_page_enabled_mask & (1U << page)) != 0;
}

const char *work_page_name(int page)
{
    static const char *kPageNames[kWorkPageCount] = {
        "天气时钟",
        "温度历史",
        "图片时钟",
        "日历",
        "天气看板",
        "翻页时钟",
    };
    constexpr size_t kPageNameCount = sizeof(kPageNames) / sizeof(kPageNames[0]);
    static_assert(kPageNameCount == kWorkPageCount, "work page names must cover every work page");
    if (page < 0 || page >= kWorkPageCount) {
        return "未知页面";
    }
    return kPageNames[page];
}

int display_settings_item_work_page(int item)
{
    static const int kDisplaySettingPages[kDisplaySettingsPageItemCount] = {
        kWorkPageWeatherClock,
        kWorkPageGallery,
        kWorkPageHistory,
        kWorkPageCalendar,
        kWorkPageWeatherBoard,
        kWorkPageFlipClock,
    };
    constexpr int kDisplaySettingPageCount = sizeof(kDisplaySettingPages) / sizeof(kDisplaySettingPages[0]);
    static_assert(kDisplaySettingPageCount == kDisplaySettingsPageItemCount,
                  "display setting page mapping must match the settings item count");
    if (item < 0 || item >= kDisplaySettingPageCount) {
        return -1;
    }
    return kDisplaySettingPages[item];
}

int first_enabled_work_page()
{
    normalize_work_page_order();
    for (int i = 0; i < kWorkPageCount; ++i) {
        int candidate = g_work_page_order[i];
        if (is_work_page_enabled(candidate)) {
            return candidate;
        }
    }
    return 0;
}

void reset_work_page_order()
{
    static const uint8_t kDefaultOrder[kWorkPageCount] = {
        kWorkPageWeatherClock,
        kWorkPageFlipClock,
        kWorkPageGallery,
        kWorkPageHistory,
        kWorkPageCalendar,
        kWorkPageWeatherBoard,
    };
    constexpr size_t kDefaultOrderCount = sizeof(kDefaultOrder) / sizeof(kDefaultOrder[0]);
    static_assert(kDefaultOrderCount == kWorkPageCount, "default work page order must cover every work page");
    static_assert(sizeof(kDefaultOrder) == sizeof(g_work_page_order),
                  "default work page order storage must match runtime order storage");
    memcpy(g_work_page_order, kDefaultOrder, sizeof(g_work_page_order));
}

void normalize_work_page_order()
{
    bool seen[kWorkPageCount] = {};
    for (int i = 0; i < kWorkPageCount; ++i) {
        uint8_t page = g_work_page_order[i];
        if (page >= kWorkPageCount || seen[page]) {
            reset_work_page_order();
            return;
        }
        seen[page] = true;
    }
    for (bool present : seen) {
        if (!present) {
            reset_work_page_order();
            return;
        }
    }
}

int next_enabled_work_page(int current_page)
{
    if (current_page < 0 || current_page >= kWorkPageCount) {
        current_page = 0;
    }
    normalize_work_page_order();
    int current_index = -1;
    for (int i = 0; i < kWorkPageCount; ++i) {
        if (g_work_page_order[i] == current_page) {
            current_index = i;
            break;
        }
    }
    for (int step = 1; step <= kWorkPageCount; ++step) {
        int candidate = g_work_page_order[(current_index + step + kWorkPageCount) % kWorkPageCount];
        if (is_work_page_enabled(candidate)) {
            return candidate;
        }
    }
    return 0;
}

void ensure_active_work_page_enabled()
{
    if (!is_work_page_enabled(g_active_work_page)) {
        g_active_work_page = first_enabled_work_page();
    }
}

void clear_clock_object_refs()
{
    g_clock_root = nullptr;
    g_history_root = nullptr;
    g_gallery_root = nullptr;
    g_calendar_root = nullptr;
    g_weather_board_root = nullptr;
    g_flip_clock_root = nullptr;
    g_date_label = nullptr;
    g_clock_summary_label = nullptr;
    g_history_date_label = nullptr;
    g_gallery_date_label = nullptr;
    g_calendar_date_label = nullptr;
    g_weather_board_date_label = nullptr;
    g_flip_clock_date_label = nullptr;
    g_history_summary_label = nullptr;
    g_gallery_summary_label = nullptr;
    g_calendar_summary_label = nullptr;
    g_weather_board_summary_label = nullptr;
    g_flip_clock_summary_label = nullptr;
    g_history_status_time_label = nullptr;
    g_gallery_status_time_label = nullptr;
    g_calendar_status_time_label = nullptr;
    g_weather_board_status_time_label = nullptr;
    g_flip_clock_status_time_label = nullptr;
    for (lv_obj_t *&canvas : g_work_status_chime_icon_canvas) {
        canvas = nullptr;
    }
    for (lv_obj_t *&canvas : g_work_status_wifi_icon_canvas) {
        canvas = nullptr;
    }
    g_gallery_time_label = nullptr;
    g_gallery_hour_label = nullptr;
    g_gallery_minute_label = nullptr;
    g_gallery_image_canvas = nullptr;
    g_gallery_time_canvas = nullptr;
    g_gallery_saying_label = nullptr;
    g_calendar_month_label = nullptr;
    g_calendar_canvas = nullptr;
    g_temp_icon_canvas = nullptr;
    g_humi_icon_canvas = nullptr;
    g_temp_label = nullptr;
    g_humi_label = nullptr;
    g_temp_trend_canvas = nullptr;
    g_humi_trend_canvas = nullptr;
    g_weather_city_label = nullptr;
    g_weather_info_label = nullptr;
    g_weather_icon_label = nullptr;
    g_weather_temp_label = nullptr;
    g_weather_humi_label = nullptr;
    g_alert_pill = nullptr;
    g_alert_icon_canvas = nullptr;
    g_alert_label = nullptr;
    g_chime_status_icon_canvas = nullptr;
    g_wifi_status_icon_canvas = nullptr;
    g_low_battery_icon_canvas = nullptr;
    g_panel_sep_a = nullptr;
    g_panel_sep_b = nullptr;
    g_time_canvas = nullptr;
    g_second_canvas = nullptr;
    g_status_gif_canvas = nullptr;
    g_day_progress_canvas = nullptr;
    g_second_progress_canvas = nullptr;
    for (lv_obj_t *&canvas : g_flip_clock_card_canvas) {
        canvas = nullptr;
    }
    g_flip_clock_sensor_label = nullptr;
    g_flip_clock_day_progress_canvas = nullptr;
    g_flip_clock_second_progress_canvas = nullptr;
    for (lv_obj_t *&segment : g_battery_segments) {
        segment = nullptr;
    }
    for (lv_obj_t *&segment : g_history_battery_segments) {
        segment = nullptr;
    }
    for (lv_obj_t *&segment : g_gallery_battery_segments) {
        segment = nullptr;
    }
    for (lv_obj_t *&segment : g_calendar_battery_segments) {
        segment = nullptr;
    }
    for (lv_obj_t *&segment : g_weather_board_battery_segments) {
        segment = nullptr;
    }
    for (lv_obj_t *&segment : g_flip_clock_battery_segments) {
        segment = nullptr;
    }
    g_history_chart_canvas = nullptr;
    g_history_temp_max_label = nullptr;
    g_history_temp_min_label = nullptr;
    g_history_humi_max_label = nullptr;
    g_history_humi_min_label = nullptr;
    for (lv_obj_t *&label : g_history_time_labels) {
        label = nullptr;
    }
    for (lv_obj_t *&label : g_history_temp_axis_labels) {
        label = nullptr;
    }
    for (lv_obj_t *&label : g_history_humi_axis_labels) {
        label = nullptr;
    }
    for (lv_obj_t *&obj : g_lower_panel_objects) {
        obj = nullptr;
    }
    for (lv_obj_t *&label : g_setup_status_labels) {
        label = nullptr;
    }
    g_last_ui_second = -1;
    g_last_ui_minute = -1;
    g_last_ui_date_key = -1;
    g_last_ui_date_page = -1;
    g_last_day_progress_filled = -1;
    g_last_second_progress_filled = -1;
    g_last_status_gif_frame = -1;
    g_last_flip_clock_hour = -1;
    g_last_flip_clock_minute = -1;
    g_last_flip_clock_second = -1;
    g_last_flip_day_progress_filled = -1;
    g_last_flip_second_progress_filled = -1;
    g_last_flip_sensor_minute = -1;
    g_last_temp_trend_drawn = 99;
    g_last_humi_trend_drawn = 99;
    g_last_history_drawn_version = (uint32_t)-1;
    g_last_history_drawn_hour = -1;
}

void clear_info_object_refs()
{
    g_info_root = nullptr;
    g_network_diag_root = nullptr;
    g_settings_root = nullptr;
    for (lv_obj_t *&label : g_info_labels) {
        label = nullptr;
    }
    for (lv_obj_t *&label : g_network_diag_labels) {
        label = nullptr;
    }
    g_network_diag_summary_label = nullptr;
    g_network_diag_hint_label = nullptr;
    g_info_ota_label = nullptr;
    g_info_ota_hint_label = nullptr;
    g_info_ota_bar_frame = nullptr;
    g_info_ota_bar_fill = nullptr;
    for (lv_obj_t *&label : g_settings_labels) {
        label = nullptr;
    }
    for (lv_obj_t *&dot : g_settings_switch_dots) {
        dot = nullptr;
    }
    for (lv_obj_t *&text : g_settings_switch_texts) {
        text = nullptr;
    }
    g_settings_feedback_label = nullptr;
    g_settings_ota_status_label = nullptr;
    g_settings_ota_hint_label = nullptr;
    g_settings_ota_bar_frame = nullptr;
    g_settings_ota_bar_fill = nullptr;
}

void remember_lower_panel_object(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    for (lv_obj_t *&slot : g_lower_panel_objects) {
        if (!slot) {
            slot = obj;
            return;
        }
    }
}

void set_lower_panel_visible(bool visible)
{
    for (lv_obj_t *obj : g_lower_panel_objects) {
        if (!obj) {
            continue;
        }
        if (visible) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void set_setup_panel_visible(bool visible)
{
    for (lv_obj_t *label : g_setup_status_labels) {
        if (!label) {
            continue;
        }
        if (visible) {
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void set_obj_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

bool update_low_battery_state()
{
    bool previous = g_low_battery_mode;
    if (g_battery_percent >= 0) {
        if (!g_low_battery_mode && g_battery_percent < kLowBatteryEnterPercent) {
            g_low_battery_mode = true;
        } else if (g_low_battery_mode && g_battery_percent >= kLowBatteryExitPercent) {
            g_low_battery_mode = false;
        }
    }
    return previous != g_low_battery_mode;
}

void apply_clock_mode_visibility(bool setup_active)
{
    bool low = g_low_battery_mode;
    set_obj_visible(g_second_canvas, !low);
    set_obj_visible(g_day_progress_canvas, !low);
    set_obj_visible(g_second_progress_canvas, !low);
    set_obj_visible(g_low_battery_icon_canvas, low);
    set_lower_panel_visible(!setup_active && !low);
    set_setup_panel_visible(setup_active && !low);
    set_obj_visible(g_panel_sep_a, !setup_active || low);
    set_obj_visible(g_panel_sep_b, !setup_active || low);
    if (low || setup_active) {
        set_obj_visible(g_alert_pill, false);
        set_obj_visible(g_chime_status_icon_canvas, false);
        set_obj_visible(g_wifi_status_icon_canvas, false);
    }
}

void update_alert_pill(bool show, int alert_index)
{
    WeatherAlertData alert = {};
    get_weather_snapshot(nullptr, &alert);
    bool visible = show &&
                   !g_low_battery_mode &&
                   alert.active &&
                   alert.count > 0;
    set_obj_visible(g_alert_pill, visible);
    update_top_status_icons(visible);
    if (visible) {
        if (alert_index < 0) {
            alert_index = 0;
        }
        alert_index %= alert.count;
        set_label_text_if_changed(g_alert_label, alert.titles[alert_index]);
    }
}

void update_top_status_icons(bool alert_visible)
{
    bool allow = !alert_visible && !g_low_battery_mode && !g_setup_portal_active;
    set_obj_visible(g_chime_status_icon_canvas, allow && (g_hourly_chime_enabled || g_hourly_chime_all_day));
    set_obj_visible(g_wifi_status_icon_canvas, allow && g_wifi_radio_on);
}
