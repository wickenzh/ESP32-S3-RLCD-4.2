// 构建启动页、System Info、设置页和配网状态页面。
#include "ui_views.h"

#include "audio_services.h"
#include "network_services.h"
#include "ota_services.h"
#include "sensor_services.h"

namespace {
TickType_t s_settings_primary_exit_block_until = 0;
}

void draw_boot_anim_frame_index(int frame)
{
    if (!g_boot_anim_canvas) {
        return;
    }
    if (frame < 0) {
        frame = 0;
    } else if (frame >= BOOT_ANIM_FRAME_COUNT) {
        frame = BOOT_ANIM_FRAME_COUNT - 1;
    }
    const uint8_t *pixels = boot_anim_frames[frame];
    uint32_t bit = 0;
    for (int y = 0; y < BOOT_ANIM_HEIGHT; ++y) {
        for (int x = 0; x < BOOT_ANIM_WIDTH; ++x, ++bit) {
            bool black = pixels[bit / 8] & (0x80 >> (bit & 7));
            lv_canvas_set_px_color(g_boot_anim_canvas, x, y, black ? lv_color_black() : lv_color_white());
        }
    }
    lv_obj_invalidate(g_boot_anim_canvas);
}

void boot_anim_task(void *)
{
    int frame = 0;
    while (g_boot_anim_running) {
        if (Lvgl_lock(100)) {
            draw_boot_anim_frame_index(frame);
            g_boot_anim_current_frame = frame;
            Lvgl_unlock();
        }
        frame = (frame + 1) % BOOT_ANIM_FRAME_COUNT;
        vTaskDelay(pdMS_TO_TICKS(kBootAnimRunFrameMs));
    }
    if (g_app_events) {
        xEventGroupSetBits(g_app_events, kBootAnimDoneBit);
    } else {
        ESP_LOGW(TAG, "boot anim done event skipped: app events unavailable");
    }
    g_boot_anim_task_handle = nullptr;
    vTaskDelete(nullptr);
}

void finish_boot_anim_to_last_frame()
{
    if (Lvgl_lock(200)) {
        draw_boot_anim_frame_index(BOOT_ANIM_FRAME_COUNT - 1);
        g_boot_anim_current_frame = BOOT_ANIM_FRAME_COUNT - 1;
        lv_refr_now(nullptr);
        Lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

void style_battery_part(lv_obj_t *obj, bool filled)
{
    lv_obj_set_style_bg_color(obj, filled ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

void style_battery_frame(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

void build_battery_icon(lv_obj_t *parent, lv_obj_t **segments)
{
    if (!parent || !segments) {
        ESP_LOGW(TAG, "battery icon invalid arg");
        return;
    }
    lv_obj_t *frame = lv_obj_create(parent);
    if (!frame) {
        ESP_LOGW(TAG, "battery frame create failed");
        return;
    }
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(frame, 20, 17);
    lv_obj_set_size(frame, 34, 16);
    style_battery_frame(frame);

    lv_obj_t *inner = lv_obj_create(frame);
    if (!inner) {
        ESP_LOGW(TAG, "battery inner create failed");
        return;
    }
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(inner, 2, 2);
    lv_obj_set_size(inner, 30, 12);
    style_battery_part(inner, false);
    lv_obj_set_style_border_width(inner, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(inner, 2, LV_PART_MAIN);

    lv_obj_t *tip = lv_obj_create(parent);
    if (!tip) {
        ESP_LOGW(TAG, "battery tip create failed");
        return;
    }
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tip, 55, 22);
    lv_obj_set_size(tip, 3, 6);
    style_battery_part(tip, true);
    lv_obj_set_style_border_width(tip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 1, LV_PART_MAIN);

    for (int i = 0; i < 5; ++i) {
        segments[i] = lv_obj_create(frame);
        if (!segments[i]) {
            ESP_LOGW(TAG, "battery segment %d create failed", i);
            continue;
        }
        lv_obj_clear_flag(segments[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(segments[i], 3 + i * 6, 4);
        lv_obj_set_size(segments[i], 4, 8);
        style_battery_part(segments[i], false);
        lv_obj_set_style_border_width(segments[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(segments[i], 1, LV_PART_MAIN);
    }
}

void show_boot_screen()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label_with_font(screen, 28, 30, 344, 30, "RLCD Weather Clock", &lv_font_montserrat_16);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    g_boot_status_label = make_label_with_font(screen, 28, 64, 344, 24, "Starting...", &lv_font_montserrat_16);
    lv_obj_set_style_text_align(g_boot_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    g_boot_detail_label = make_label_with_font(screen, 28, 256, 344, 22, "Preparing system", &lv_font_montserrat_14);
    lv_obj_set_style_text_align(g_boot_detail_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *version = make_label_with_font(screen, 28, 226, 344, 24, APP_VERSION, &lv_font_montserrat_16);
    lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (!g_boot_anim_canvas_buf) {
        g_boot_anim_canvas_buf = alloc_canvas_buffer(BOOT_ANIM_WIDTH, BOOT_ANIM_HEIGHT);
    }
    g_boot_anim_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_boot_anim_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_boot_anim_canvas, 144, 100);
    lv_obj_set_size(g_boot_anim_canvas, BOOT_ANIM_WIDTH, BOOT_ANIM_HEIGHT);
    lv_obj_set_style_border_width(g_boot_anim_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_boot_anim_canvas, 0, LV_PART_MAIN);
    if (g_boot_anim_canvas_buf) {
        lv_canvas_set_buffer(g_boot_anim_canvas,
                             g_boot_anim_canvas_buf,
                             BOOT_ANIM_WIDTH,
                             BOOT_ANIM_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(g_boot_anim_canvas, lv_color_white(), LV_OPA_COVER);
        draw_boot_anim_frame_index(0);
    }

    lv_refr_now(nullptr);
}

void update_boot_screen(int percent, const char *status, const char *detail)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    if (Lvgl_lock(2000)) {
        if (g_boot_status_label) {
            set_label_text_if_changed(g_boot_status_label, status);
        }
        if (g_boot_detail_label) {
            set_label_text_if_changed(g_boot_detail_label, detail);
        }
        lv_refr_now(nullptr);
        Lvgl_unlock();
    }
}

void finish_boot_screen()
{
    if (Lvgl_lock(2000)) {
        lv_obj_clean(lv_scr_act());
        clear_clock_object_refs();
        clear_info_object_refs();
        g_boot_status_label = nullptr;
        g_boot_detail_label = nullptr;
        g_boot_anim_canvas = nullptr;
        g_active_work_page = first_enabled_work_page();
        show_active_work_page();
        lv_refr_now(nullptr);
        Lvgl_unlock();
    }
}

void build_boot_info_page()
{
    if (g_info_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    g_info_root = screen;
    lv_obj_add_flag(g_info_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label_with_font(screen, 24, 18, 352, 26, "SYSTEM INFO", &lv_font_montserrat_16);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *top_line = make_bar(screen, 24, 50, 352, 3);
    set_obj_black(top_line, true);

    static const int y_positions[] = {70, 104, 138, 172, 206};
    for (int i = 0; i < 5; ++i) {
        g_info_labels[i] = make_label_with_font(screen, 30, y_positions[i], 340, 24, "--", &lv_font_montserrat_14);
    }

    lv_obj_t *bottom_line = make_bar(screen, 24, 238, 352, 3);
    set_obj_black(bottom_line, true);
    g_info_ota_label = make_label_with_font(screen, 24, 252, 352, 22, "Hold KEY to return", &lv_font_montserrat_14);
    lv_obj_set_style_text_align(g_info_ota_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_info_ota_hint_label = nullptr;
    g_info_ota_bar_frame = nullptr;
    g_info_ota_bar_fill = nullptr;
}

void update_boot_info_page()
{
    char ntp[32];
    char weather[32];
    char line[96];

    format_time_or_dash(g_last_ntp_sync_time, ntp, sizeof(ntp));
    snprintf(line, sizeof(line), "Last NTP: %s", ntp);
    set_label_text_if_changed(g_info_labels[0], line);

    snprintf(line, sizeof(line), "WiFi: %s", g_wifi_ssid[0] ? g_wifi_ssid : "--");
    set_label_text_if_changed(g_info_labels[1], line);

    format_time_or_dash(g_last_weather_sync_time, weather, sizeof(weather));
    snprintf(line, sizeof(line), "Last Weather: %s", weather);
    set_label_text_if_changed(g_info_labels[2], line);

    if (g_battery_percent >= 0 && g_battery_voltage >= 0.0f) {
        snprintf(line, sizeof(line), "Battery: %d%%  %.2fV", g_battery_percent, g_battery_voltage);
    } else if (g_battery_percent >= 0) {
        snprintf(line, sizeof(line), "Battery: %d%%  --", g_battery_percent);
    } else {
        snprintf(line, sizeof(line), "Battery: --  --");
    }
    set_label_text_if_changed(g_info_labels[3], line);

    snprintf(line, sizeof(line), "Version: %s / %s", APP_VERSION, APP_BUILD_DATE);
    set_label_text_if_changed(g_info_labels[4], line);

    ota_reset_status_if_idle();
}

void build_network_diag_page()
{
    if (g_network_diag_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    g_network_diag_root = screen;
    lv_obj_add_flag(g_network_diag_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(screen, 24, 18, 352, 28, "网络检测");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *top_line = make_bar(screen, 24, 52, 352, 3);
    set_obj_black(top_line, true);

    g_network_diag_summary_label = make_label(screen, 24, 62, 352, 22, "准备检测...");
    lv_obj_set_style_text_align(g_network_diag_summary_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    for (int i = 0; i < kNetworkDiagLineCount; ++i) {
        int x = 30;
        int y = 88;
        int w = 340;
        if (i == 0) {
            y = 88;
        } else if (i == 1) {
            y = 112;
        } else {
            int grid = i - 2;
            int row = grid / 2;
            int col = grid & 1;
            x = 30 + col * 174;
            y = 142 + row * 28;
            w = 160;
            if (i == 8) {
                x = 30;
                w = 340;
            }
        }
        g_network_diag_labels[i] = make_label(screen, x, y, w, 22, "--");
        lv_label_set_long_mode(g_network_diag_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(g_network_diag_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    }

    lv_obj_t *bottom_line = make_bar(screen, 24, 266, 352, 2);
    set_obj_black(bottom_line, true);
    g_network_diag_hint_label = make_label(screen, 24, 272, 352, 20, "Hold KEY to return");
    lv_obj_set_style_text_align(g_network_diag_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

bool update_network_diag_page()
{
    bool changed = false;
    char summary[64];
    if (g_network_diag_state == kNetworkDiagRunning) {
        snprintf(summary, sizeof(summary), "检测中...");
    } else if (g_network_diag_state == kNetworkDiagDone) {
        snprintf(summary, sizeof(summary), "检测完成");
    } else {
        snprintf(summary, sizeof(summary), "等待开始");
    }
    changed |= set_label_text_if_changed(g_network_diag_summary_label, summary);
    for (int i = 0; i < kNetworkDiagLineCount; ++i) {
        changed |= set_label_text_if_changed(g_network_diag_labels[i],
                                             g_network_diag_lines[i][0] ? g_network_diag_lines[i] : "--");
    }
    changed |= set_label_text_if_changed(g_network_diag_hint_label,
                                         g_network_diag_state == kNetworkDiagRunning ? "Checking... Hold KEY to return" :
                                                                                       "Hold KEY to return");
    return changed;
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
    lv_color_t fg = selected ? lv_color_white() : lv_color_black();
    lv_color_t bg = selected ? lv_color_black() : lv_color_white();
    lv_obj_set_style_bg_color(dot, on ? fg : bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dot, fg, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
}

int settings_secondary_count(int primary)
{
    switch (primary) {
    case kSettingsPrimaryNetwork:
        return 3;
    case kSettingsPrimarySound:
        return 4;
    case kSettingsPrimaryDisplay:
        return 7;
    case kSettingsPrimarySystem:
        return 5;
    default:
        return 0;
    }
}

void reset_settings_confirmation()
{
    g_factory_reset_confirm_pending = false;
    g_offline_disable_confirm_pending = false;
}

static int clamp_settings_primary(int primary)
{
    if (primary < 0 || primary >= kSettingsPrimaryCount) {
        return kSettingsPrimaryNetwork;
    }
    return primary;
}

static int clamp_settings_secondary(int primary, int selected)
{
    int count = settings_secondary_count(primary);
    if (count <= 0) {
        return 0;
    }
    if (selected < 0 || selected >= count) {
        return 0;
    }
    return selected;
}

void handle_settings_key_short()
{
    int primary = clamp_settings_primary(g_settings_primary_selection);
    if (g_settings_page_order_mode) {
        g_settings_page_order_selection = (g_settings_page_order_selection + 1) % kWorkPageCount;
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
    g_settings_feedback[0] = '\0';
    notify_ui_task();
}

void handle_settings_key_long()
{
    g_settings_last_activity_tick = xTaskGetTickCount();
    if (g_settings_page_order_mode) {
        g_settings_page_order_mode = false;
        g_settings_focus_secondary = true;
        g_settings_primary_selection = kSettingsPrimaryDisplay;
        g_settings_selection = 6;
        if (save_work_page_order()) {
            g_active_work_page = first_enabled_work_page();
            set_settings_feedback("页面顺序已保存", 2500);
        } else {
            set_settings_feedback("保存失败", 2500);
        }
        reset_settings_confirmation();
        notify_ui_task();
        return;
    } else if (g_settings_focus_secondary) {
        g_settings_focus_secondary = false;
        g_settings_selection = 0;
        s_settings_primary_exit_block_until = xTaskGetTickCount() + pdMS_TO_TICKS(800);
    } else {
        TickType_t now = xTaskGetTickCount();
        if (s_settings_primary_exit_block_until != 0 && now < s_settings_primary_exit_block_until) {
            g_settings_last_activity_tick = now;
            notify_ui_task();
            return;
        }
        s_settings_primary_exit_block_until = 0;
        g_settings_requested = false;
        g_settings_page_order_mode = false;
        g_settings_focus_secondary = false;
        reset_settings_confirmation();
        g_settings_feedback[0] = '\0';
        notify_ui_task();
        return;
    }
    reset_settings_confirmation();
    g_settings_feedback[0] = '\0';
    notify_ui_task();
}

void build_settings_page()
{
    if (g_settings_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    g_settings_root = screen;
    lv_obj_add_flag(g_settings_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(screen, 24, 18, 352, 28, "设置");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_t *top_line = make_bar(screen, 24, 52, 352, 3);
    set_obj_black(top_line, true);

    lv_obj_t *separator = make_bar(screen, 136, 62, 2, 174);
    set_obj_black(separator, true);

    static const int y_positions[] = {66, 105, 144, 183, 222, 222, 222};
    for (int i = 0; i < kSettingsPrimaryCount; ++i) {
        g_settings_labels[i] = make_label(screen, 12, y_positions[i], 112, 30, "--");
        lv_label_set_long_mode(g_settings_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(g_settings_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    for (int i = 0; i < kSettingsSecondaryMaxCount; ++i) {
        int slot = kSettingsPrimaryCount + i;
        g_settings_labels[slot] = make_label(screen, 150, y_positions[i], 228, 30, "--");
        lv_label_set_long_mode(g_settings_labels[slot], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(g_settings_labels[slot], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        g_settings_switch_dots[i] = lv_obj_create(screen);
        lv_obj_clear_flag(g_settings_switch_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(g_settings_switch_dots[i], 362, y_positions[i] + 8);
        lv_obj_set_size(g_settings_switch_dots[i], 12, 12);
        style_settings_switch_dot(g_settings_switch_dots[i], false, false);
        lv_obj_add_flag(g_settings_switch_dots[i], LV_OBJ_FLAG_HIDDEN);
        g_settings_switch_texts[i] = make_label(screen, 352, y_positions[i] + 6, 28, 18, "");
        lv_obj_set_style_text_align(g_settings_switch_texts[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(g_settings_switch_texts[i], 0, LV_PART_MAIN);
        lv_label_set_long_mode(g_settings_switch_texts[i], LV_LABEL_LONG_CLIP);
        lv_obj_add_flag(g_settings_switch_texts[i], LV_OBJ_FLAG_HIDDEN);
    }
    g_settings_ota_status_label = make_label(screen, 150, 176, 228, 22, "");
    lv_obj_set_style_text_align(g_settings_ota_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_settings_ota_bar_frame = lv_obj_create(screen);
    lv_obj_clear_flag(g_settings_ota_bar_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_settings_ota_bar_frame, 164, 203);
    lv_obj_set_size(g_settings_ota_bar_frame, 200, 9);
    lv_obj_set_style_bg_color(g_settings_ota_bar_frame, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_settings_ota_bar_frame, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_settings_ota_bar_frame, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_settings_ota_bar_frame, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(g_settings_ota_bar_frame, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_settings_ota_bar_frame, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_settings_ota_bar_frame, LV_OBJ_FLAG_HIDDEN);
    g_settings_ota_bar_fill = make_bar(screen, 166, 205, 1, 5);
    set_obj_black(g_settings_ota_bar_fill, true);
    lv_obj_set_style_radius(g_settings_ota_bar_fill, 2, LV_PART_MAIN);
    lv_obj_add_flag(g_settings_ota_bar_fill, LV_OBJ_FLAG_HIDDEN);
    g_settings_ota_hint_label = make_label(screen, 150, 218, 228, 20, "");
    lv_obj_set_style_text_align(g_settings_ota_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    g_settings_feedback_label = make_label(screen, 24, 246, 352, 20, "");
    lv_obj_set_style_text_align(g_settings_feedback_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *hint = make_label(screen, 24, 270, 352, 22, "KEY选择  长按返回  BOOT确认");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

bool update_settings_page()
{
    ota_reset_status_if_idle();
    bool changed = false;
    static int last_primary = -1;
    static int last_selected = -1;
    static bool last_focus_secondary = false;
    static int last_ota_state = -1;
    static int last_ota_progress = -2;
    static int last_ota_speed = -2;

    const char *primary_items[kSettingsPrimaryCount] = {"网络", "声音", "显示", "系统"};
    char secondary_items[kSettingsSecondaryMaxCount][40] = {};
    static const int y_positions[] = {66, 105, 144, 183, 222, 222, 222};
    int primary = clamp_settings_primary(g_settings_primary_selection);
    int selected = clamp_settings_secondary(primary, g_settings_selection);
    g_settings_primary_selection = primary;
    g_settings_selection = selected;
    if (g_settings_page_order_mode) {
        normalize_work_page_order();
    }

    if (primary == kSettingsPrimaryNetwork) {
        strlcpy(secondary_items[0], "同步时间", sizeof(secondary_items[0]));
        strlcpy(secondary_items[1], "同步天气", sizeof(secondary_items[1]));
        strlcpy(secondary_items[2], "更新一言", sizeof(secondary_items[2]));
    } else if (primary == kSettingsPrimarySound) {
        snprintf(secondary_items[0], sizeof(secondary_items[0]), "音量 %d%%", g_chime_volume_percent);
        snprintf(secondary_items[1], sizeof(secondary_items[1]), "声音选择 %d", g_chime_sound_index + 1);
        strlcpy(secondary_items[2], "整点提醒 7:00 - 22:00", sizeof(secondary_items[2]));
        strlcpy(secondary_items[3], "全天提醒 0:00 - 24:00", sizeof(secondary_items[3]));
    } else if (primary == kSettingsPrimaryDisplay) {
        strlcpy(secondary_items[0], "天气时钟", sizeof(secondary_items[0]));
        strlcpy(secondary_items[1], "图片时钟", sizeof(secondary_items[1]));
        strlcpy(secondary_items[2], "温度历史", sizeof(secondary_items[2]));
        strlcpy(secondary_items[3], "日历", sizeof(secondary_items[3]));
        strlcpy(secondary_items[4], "天气看板", sizeof(secondary_items[4]));
        strlcpy(secondary_items[5], "翻页时钟", sizeof(secondary_items[5]));
        strlcpy(secondary_items[6], "页面顺序", sizeof(secondary_items[6]));
    } else {
        snprintf(secondary_items[0], sizeof(secondary_items[0]), "离线模式 %s", g_offline_mode_ui_enabled ? "开" : "关");
        strlcpy(secondary_items[1], "网络检测", sizeof(secondary_items[1]));
        strlcpy(secondary_items[2], g_factory_reset_confirm_pending ? "确认恢复" : "恢复出厂设置", sizeof(secondary_items[2]));
        strlcpy(secondary_items[3], "关于本机", sizeof(secondary_items[3]));
        strlcpy(secondary_items[4], "检查更新", sizeof(secondary_items[4]));
    }
    static bool last_page_order_mode = false;
    static int last_page_order_selection = -1;
    bool selection_changed = selected != last_selected ||
                             primary != last_primary ||
                             g_settings_focus_secondary != last_focus_secondary ||
                             g_settings_page_order_mode != last_page_order_mode ||
                             g_settings_page_order_selection != last_page_order_selection ||
                             g_ota_state != last_ota_state ||
                             g_ota_progress != last_ota_progress ||
                             g_ota_speed_kbps != last_ota_speed;
    if (selection_changed) {
        changed = true;
        last_selected = selected;
        last_primary = primary;
        last_focus_secondary = g_settings_focus_secondary;
        last_page_order_mode = g_settings_page_order_mode;
        last_page_order_selection = g_settings_page_order_selection;
        last_ota_state = g_ota_state;
        last_ota_progress = g_ota_progress;
        last_ota_speed = g_ota_speed_kbps;
    }
    for (int i = 0; i < kSettingsPrimaryCount; ++i) {
        if (g_settings_labels[i]) {
            changed |= set_label_text_if_changed(g_settings_labels[i], primary_items[i]);
            if (selection_changed) {
                style_settings_item(g_settings_labels[i], i == primary);
            }
        }
    }
    int secondary_count = settings_secondary_count(primary);
    for (int i = 0; i < kSettingsSecondaryMaxCount; ++i) {
        int slot = kSettingsPrimaryCount + i;
        if (!g_settings_labels[slot]) {
            continue;
        }
        if (g_settings_page_order_mode) {
            static const char *page_names[kWorkPageCount] = {"天气时钟", "温度历史", "图片时钟", "日历", "天气看板", "翻页时钟"};
            static const int kOrderGridRowY[3] = {66, 105, 144};
            static constexpr int kOrderGridLeftX = 150;
            static constexpr int kOrderGridRightX = 267;
            static constexpr int kOrderGridColW = 111;
            int col = i & 1;
            int row = i >> 1;
            lv_obj_set_pos(g_settings_labels[slot], col == 0 ? kOrderGridLeftX : kOrderGridRightX, kOrderGridRowY[row]);
            lv_obj_set_size(g_settings_labels[slot], kOrderGridColW, 30);
            if (i < kWorkPageCount) {
                snprintf(secondary_items[i], sizeof(secondary_items[i]), "%d %s", i + 1, page_names[g_work_page_order[i]]);
            }
            if (g_settings_switch_dots[i]) {
                set_obj_visible(g_settings_switch_dots[i], false);
            }
            if (g_settings_switch_texts[i]) {
                set_obj_visible(g_settings_switch_texts[i], false);
            }
        } else if (primary == kSettingsPrimaryDisplay || primary == kSettingsPrimarySystem) {
            int col = i & 1;
            int row = i >> 1;
            bool grid_item = primary == kSettingsPrimaryDisplay ? i < 6 : i < 4;
            if (grid_item) {
                static const int kGridRowY[3] = {66, 105, 144};
                static constexpr int kGridLeftX = 150;
                static constexpr int kGridRightX = 267;
                static constexpr int kGridColW = 111;
                static constexpr int kGridSwitchDotXOffset = 92;
                int grid_x = col == 0 ? kGridLeftX : kGridRightX;
                lv_obj_set_pos(g_settings_labels[slot], grid_x, kGridRowY[row]);
                lv_obj_set_size(g_settings_labels[slot], kGridColW, 30);
                if (g_settings_switch_dots[i]) {
                    lv_obj_set_pos(g_settings_switch_dots[i], grid_x + kGridSwitchDotXOffset, kGridRowY[row] + 9);
                }
                if (g_settings_switch_texts[i]) {
                    int status_x = grid_x + (primary == kSettingsPrimarySystem ? 80 : 90);
                    int status_w = primary == kSettingsPrimarySystem ? 26 : 30;
                    lv_obj_set_pos(g_settings_switch_texts[i], status_x, kGridRowY[row] + 7);
                    lv_obj_set_size(g_settings_switch_texts[i], status_w, 18);
                }
            } else {
                lv_obj_set_pos(g_settings_labels[slot], 150, primary == kSettingsPrimarySystem ? 144 : 183);
                lv_obj_set_size(g_settings_labels[slot], 228, 30);
                if (g_settings_switch_dots[i]) {
                    set_obj_visible(g_settings_switch_dots[i], false);
                }
                if (g_settings_switch_texts[i]) {
                    set_obj_visible(g_settings_switch_texts[i], false);
                }
            }
        } else {
            lv_obj_set_pos(g_settings_labels[slot], 150, y_positions[i]);
            lv_obj_set_size(g_settings_labels[slot], 228, 30);
            if (g_settings_switch_dots[i]) {
                lv_obj_set_pos(g_settings_switch_dots[i], 362, y_positions[i] + 8);
            }
            if (g_settings_switch_texts[i]) {
                lv_obj_set_pos(g_settings_switch_texts[i], 352, y_positions[i] + 6);
                lv_obj_set_size(g_settings_switch_texts[i], 28, 18);
            }
        }
        bool visible = i < secondary_count;
        if (g_settings_page_order_mode) {
            visible = i < kWorkPageCount;
        }
        set_obj_visible(g_settings_labels[slot], visible);
        if (visible) {
            changed |= set_label_text_if_changed(g_settings_labels[slot], secondary_items[i]);
            if (selection_changed) {
                bool selected_item = g_settings_page_order_mode ? i == g_settings_page_order_selection :
                                     (g_settings_focus_secondary && i == selected);
                style_settings_item(g_settings_labels[slot], selected_item);
                if (primary == kSettingsPrimarySystem && i < 4) {
                    lv_obj_set_style_pad_left(g_settings_labels[slot], 4, LV_PART_MAIN);
                    lv_obj_set_style_pad_right(g_settings_labels[slot], 4, LV_PART_MAIN);
                }
            }
        }
        bool dot_visible = false;
        bool dot_on = false;
        bool switch_text_visible = false;
        const char *switch_text = "";
        if (visible && primary == kSettingsPrimarySound) {
            if (i >= 2) {
                dot_visible = true;
                dot_on = i == 2 ? g_hourly_chime_enabled : g_hourly_chime_all_day;
            }
        } else if (visible && primary == kSettingsPrimaryDisplay && !g_settings_page_order_mode && i < 6) {
            dot_visible = true;
            if (i == 0) {
                dot_on = true;
            } else if (i == 1) {
                dot_on = is_work_page_enabled(2);
            } else if (i == 2) {
                dot_on = is_work_page_enabled(1);
            } else if (i == 3) {
                dot_on = is_work_page_enabled(3);
            } else if (i == 4) {
                dot_on = is_work_page_enabled(4);
            } else if (i == 5) {
                dot_on = is_work_page_enabled(5);
            }
        }
        if (g_settings_switch_dots[i]) {
            set_obj_visible(g_settings_switch_dots[i], dot_visible);
            if (dot_visible) {
                style_settings_switch_dot(g_settings_switch_dots[i], dot_on, g_settings_focus_secondary && i == selected);
            }
        }
        if (g_settings_switch_texts[i]) {
            set_obj_visible(g_settings_switch_texts[i], switch_text_visible);
            if (switch_text_visible) {
                set_label_text_if_changed(g_settings_switch_texts[i], switch_text);
                bool selected_item = g_settings_focus_secondary && i == selected;
                lv_obj_set_style_text_color(g_settings_switch_texts[i],
                                            selected_item ? lv_color_white() : lv_color_black(),
                                            LV_PART_MAIN);
            }
        }
    }
    bool ota_panel_visible = primary == kSettingsPrimarySystem && selected == 4;
    if (g_settings_ota_status_label) {
        char ota_line[96] = "";
        char ota_hint[48] = "";
        bool progress_visible = false;
        int progress = g_ota_progress;
        if (ota_panel_visible) {
            if (g_ota_state == kOtaUpdating && progress >= 0) {
                progress_visible = true;
                if (g_ota_speed_kbps > 0) {
                    snprintf(ota_line, sizeof(ota_line), "OTA %d%%  %d KB/s", progress, g_ota_speed_kbps);
                } else {
                    snprintf(ota_line, sizeof(ota_line), "OTA %d%%", progress);
                }
                strlcpy(ota_hint, "下载中，请等待", sizeof(ota_hint));
            } else if (g_ota_state == kOtaAvailable) {
                snprintf(ota_line, sizeof(ota_line), "%s", g_ota_status);
                strlcpy(ota_hint, "BOOT安装更新", sizeof(ota_hint));
            } else if (g_ota_state == kOtaChecking) {
                snprintf(ota_line, sizeof(ota_line), "%s", g_ota_status);
                strlcpy(ota_hint, "正在检查，请等待", sizeof(ota_hint));
            } else if (g_ota_state == kOtaSucceeded) {
                progress_visible = true;
                progress = 100;
                snprintf(ota_line, sizeof(ota_line), "%s", g_ota_status);
                strlcpy(ota_hint, "即将重启", sizeof(ota_hint));
            } else if (g_ota_state == kOtaFailed || g_ota_state == kOtaNoUpdate) {
                snprintf(ota_line, sizeof(ota_line), "%s", g_ota_status);
                strlcpy(ota_hint, "BOOT重新检查", sizeof(ota_hint));
            } else {
                snprintf(ota_line, sizeof(ota_line), "当前版本 %s", APP_VERSION);
                strlcpy(ota_hint, "BOOT开始检查", sizeof(ota_hint));
            }
        }
        changed |= set_label_text_if_changed(g_settings_ota_status_label, ota_line);
        if (g_settings_ota_hint_label) {
            changed |= set_label_text_if_changed(g_settings_ota_hint_label, ota_hint);
        }
        if (g_settings_ota_bar_frame) {
            set_obj_visible(g_settings_ota_bar_frame, ota_panel_visible && progress_visible);
        }
        if (g_settings_ota_bar_fill) {
            int clamped = progress;
            if (clamped < 0) {
                clamped = 0;
            } else if (clamped > 100) {
                clamped = 100;
            }
            int fill_w = (196 * clamped) / 100;
            if (fill_w < 1) {
                fill_w = 1;
            }
            lv_obj_set_width(g_settings_ota_bar_fill, fill_w);
            set_obj_visible(g_settings_ota_bar_fill, ota_panel_visible && progress_visible);
        }
    }
    if (g_settings_feedback_label) {
        TickType_t now = xTaskGetTickCount();
        if (g_settings_feedback[0] && now < g_settings_feedback_until_tick) {
            changed |= set_label_text_if_changed(g_settings_feedback_label, g_settings_feedback);
        } else {
            g_settings_feedback[0] = '\0';
            changed |= set_label_text_if_changed(g_settings_feedback_label, "");
        }
    }
    return changed;
}

void set_settings_feedback(const char *text, uint32_t duration_ms)
{
    strlcpy(g_settings_feedback, text, sizeof(g_settings_feedback));
    TickType_t now = xTaskGetTickCount();
    g_settings_feedback_until_tick = now + pdMS_TO_TICKS(duration_ms);
    if (g_settings_requested) {
        g_settings_last_activity_tick = now;
    }
    notify_ui_task();
}

bool is_settings_sync_busy()
{
    return g_settings_sync_op != kSettingsSyncNone || g_network_diag_state == kNetworkDiagRunning;
}

void begin_settings_sync(SettingsSyncOp op, const char *text)
{
    TickType_t now = xTaskGetTickCount();
    g_settings_sync_op = op;
    g_settings_sync_deadline_tick = now + pdMS_TO_TICKS(kSettingsManualSyncTimeoutMs);
    g_settings_last_activity_tick = now;
    set_settings_feedback(text, kSettingsManualSyncTimeoutMs);
}

void finish_settings_sync(SettingsSyncOp op, const char *text)
{
    if (g_settings_sync_op != op) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    g_settings_sync_op = kSettingsSyncNone;
    g_settings_sync_deadline_tick = 0;
    g_settings_last_activity_tick = now;
    set_settings_feedback(text, 3500);
}

bool update_setup_status_panel()
{
    bool changed = false;
    char line[96];
    if (!g_setup_status_labels[0]) {
        return false;
    }
    changed |= set_label_text_if_changed(g_setup_status_labels[0], "Setup Mode");
    snprintf(line, sizeof(line), "AP SSID: %s", g_ap_ssid[0] ? g_ap_ssid : "--");
    changed |= set_label_text_if_changed(g_setup_status_labels[1], line);
    snprintf(line, sizeof(line), "AP Password: %s", kSetupApPassword);
    changed |= set_label_text_if_changed(g_setup_status_labels[2], line);
    snprintf(line, sizeof(line), "Portal IP: %s", kSetupPortalIp);
    changed |= set_label_text_if_changed(g_setup_status_labels[3], line);
    snprintf(line, sizeof(line), "STA SSID: %s", g_wifi_ssid[0] ? g_wifi_ssid : "--");
    changed |= set_label_text_if_changed(g_setup_status_labels[4], line);
    if (g_sta_ip[0]) {
        snprintf(line, sizeof(line), "STA IP: %s", g_sta_ip);
    } else if (g_last_wifi_disconnect_reason) {
        snprintf(line, sizeof(line), "STA IP: --  reason %d", g_last_wifi_disconnect_reason);
    } else {
        snprintf(line, sizeof(line), "STA IP: --");
    }
    changed |= set_label_text_if_changed(g_setup_status_labels[5], line);
    return changed;
}

void update_battery_segments(lv_obj_t **segments, int percent, bool charging, bool blink_on)
{
    int filled = 0;
    int blink_index = -1;
    if (percent >= 0) {
        if (percent > 100) {
            percent = 100;
        }
        filled = (percent + 19) / 20;
        if (charging && percent < 100) {
            blink_index = percent / 20;
            if (blink_index > 4) {
                blink_index = 4;
            }
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (segments[i]) {
            bool active = i < filled;
            if (i == blink_index) {
                active = blink_on;
            }
            style_battery_part(segments[i], active);
            lv_obj_set_style_border_width(segments[i], 0, LV_PART_MAIN);
            lv_obj_set_style_radius(segments[i], 1, LV_PART_MAIN);
        }
    }
}

void update_battery_icon(int percent, bool charging, bool blink_on)
{
    update_battery_segments(g_battery_segments, percent, charging, blink_on);
    update_battery_segments(g_history_battery_segments, percent, charging, blink_on);
    update_battery_segments(g_gallery_battery_segments, percent, charging, blink_on);
    update_battery_segments(g_calendar_battery_segments, percent, charging, blink_on);
}
