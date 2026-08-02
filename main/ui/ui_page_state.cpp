// 管理页面根对象、可见性、工作页顺序和低电量显示状态。
#include "ui_views.h"

#include "alarm_services.h"
#include "app_constexpr.h"
#include "network_services.h"

namespace {
#define PAGE_ROOT_CREATE_FAILED_LOG "page root create failed"
#define LOWER_PANEL_OBJECT_LIST_FULL_LOG "lower panel object list full"
constexpr int kFallbackWorkPage = kWorkPageWeatherClock;
constexpr size_t kAuxPageRootCount = 3; // System info, network diagnostics and settings.
constexpr int kPageRootX = 0;
constexpr int kPageRootY = 0;
constexpr int kPageRootW = kDisplayWidth;
constexpr int kPageRootH = kDisplayHeight;
struct PageRootList {
    lv_obj_t *items[kWorkPageCount + kAuxPageRootCount];
};

lv_obj_t *work_page_root_or_fallback(lv_obj_t *root)
{
    return root ? root : g_clock_root;
}

PageRootList current_page_roots()
{
    return {{
        g_clock_root,
        g_history_root,
        g_gallery_root,
        g_calendar_root,
        g_weather_board_root,
        g_radio_root,
        g_xiaozhi_root,
        g_info_root,
        g_network_diag_root,
        g_settings_root,
    }};
}

lv_obj_t *build_work_page_root(int page)
{
    switch (page) {
    case kWorkPageWeatherClock:
        build_clock_ui();
        return g_clock_root;
    case kWorkPageHistory:
        build_history_page();
        return work_page_root_or_fallback(g_history_root);
    case kWorkPageGallery:
        build_gallery_page();
        return work_page_root_or_fallback(g_gallery_root);
    case kWorkPageCalendar:
        build_calendar_page();
        return work_page_root_or_fallback(g_calendar_root);
    case kWorkPageWeatherBoard:
        build_weather_board_page();
        return work_page_root_or_fallback(g_weather_board_root);
    case kWorkPageRadio:
        build_radio_page();
        return work_page_root_or_fallback(g_radio_root);
    case kWorkPageXiaozhiAI:
        build_xiaozhi_page();
        return work_page_root_or_fallback(g_xiaozhi_root);
    default:
        return g_clock_root;
    }
}

static_assert(kFallbackWorkPage == kWorkPageWeatherClock, "special-mode fallback page must remain weather clock");
static_assert(kAuxPageRootCount == 3, "auxiliary roots are info, network diagnostics and settings");
static_assert(kPageRootW > 0 && kPageRootH > 0, "page root size must be positive");
} // namespace

static void configure_page_root(lv_obj_t *root)
{
    if (!root) {
        return;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root, kPageRootX, kPageRootY);
    lv_obj_set_size(root, kPageRootW, kPageRootH);
    lv_obj_set_style_bg_color(root, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
}

lv_obj_t *create_page_root()
{
    lv_obj_t *root = lv_obj_create(lv_scr_act());
    if (!root) {
        ESP_LOGW(TAG, "%s", PAGE_ROOT_CREATE_FAILED_LOG);
        return nullptr;
    }
    configure_page_root(root);
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
    PageRootList roots = current_page_roots();
    constexpr size_t kPageRootCount = array_count(roots.items);
    static_assert(kPageRootCount == kWorkPageCount + kAuxPageRootCount,
                  "page root visibility list must cover all work pages and auxiliary pages");
    for (lv_obj_t *root : roots.items) {
        set_page_visible(root, page == root);
    }
}

lv_obj_t *active_work_page_root()
{
    if (battery_low_mode_load() || setup_portal_active_load()) {
        active_work_page_store(kFallbackWorkPage);
        return build_work_page_root(kFallbackWorkPage);
    }
    ensure_active_work_page_enabled();
    return build_work_page_root(active_work_page_load());
}

void show_active_work_page()
{
    show_page(active_work_page_root());
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
    ESP_LOGW(TAG, "%s", LOWER_PANEL_OBJECT_LIST_FULL_LOG);
}

bool set_obj_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) {
        return false;
    }
    bool already_visible = !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (already_visible == visible) {
        return false;
    }
    if (visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

void set_lower_panel_visible(bool visible)
{
    for (lv_obj_t *obj : g_lower_panel_objects) {
        set_obj_visible(obj, visible);
    }
}

void set_setup_panel_visible(bool visible)
{
    for (lv_obj_t *label : g_setup_status_labels) {
        set_obj_visible(label, visible);
    }
}

bool update_low_battery_state()
{
    return battery_runtime_low_mode_update(kLowBatteryEnterPercent,
                                           kLowBatteryExitPercent);
}

void apply_clock_mode_visibility(bool setup_active)
{
    bool low = battery_low_mode_load();
    set_obj_visible(g_second_canvas, !low);
    set_work_page_day_progress_visible(kWorkPageWeatherClock, !low);
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
        set_obj_visible(g_alarm_status_icon_canvas, false);
    }
}

void update_alert_pill(bool show, int alert_index)
{
    WeatherAlertData alert = {};
    get_weather_snapshot(nullptr, &alert);
    bool visible = show &&
                   !battery_low_mode_load() &&
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

bool update_top_status_icons(bool alert_visible)
{
    bool allow = !alert_visible && !battery_low_mode_load() && !setup_portal_active_load();
    bool changed = false;
    changed |= set_obj_visible(g_chime_status_icon_canvas,
                               allow && (g_hourly_chime_enabled || g_hourly_chime_all_day));
    changed |= set_obj_visible(g_wifi_status_icon_canvas,
                               allow && wifi_radio_on_for_status_icon());
    changed |= set_obj_visible(g_alarm_status_icon_canvas,
                               allow && alarm_is_enabled());
    return changed;
}
