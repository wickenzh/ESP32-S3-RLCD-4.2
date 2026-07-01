// 构建启动页、System Info、设置页和配网状态页面。
#include "ui_views.h"

#include "audio_services.h"
#include "network_services.h"
#include "ota_services.h"
#include "sensor_services.h"

namespace {
TickType_t s_settings_primary_exit_block_until = 0;

template <typename T, size_t N>
constexpr size_t array_count(const T (&)[N])
{
    return N;
}

void copy_text(char *out, size_t out_len, const char *text)
{
    if (!out || out_len == 0) {
        return;
    }
    strlcpy(out, text ? text : "", out_len);
}

constexpr uint32_t kBootAnimLvglLockTimeoutMs = 100;
constexpr uint32_t kBootAnimFinishLvglLockTimeoutMs = 200;
constexpr uint32_t kBootAnimFinishHoldMs = 100;
constexpr uint32_t kBootScreenLvglLockTimeoutMs = 2000;
constexpr uint32_t kSettingsPrimaryExitBlockMs = 800;
constexpr int kNetworkDiagLocalIpLine = 0;
constexpr int kNetworkDiagPublicIpLine = 1;
constexpr int kNetworkDiagGridFirstLine = 2;
constexpr int kNetworkDiagWideLine = kNetworkDiagLineCount - 1;
constexpr int kNetworkDiagGridColumns = 2;
constexpr int kNetworkDiagWideX = 30;
constexpr int kNetworkDiagWideW = 340;
constexpr int kNetworkDiagLocalIpY = 88;
constexpr int kNetworkDiagPublicIpY = 112;
constexpr int kNetworkDiagGridStartY = 142;
constexpr int kNetworkDiagGridRowGap = 28;
constexpr int kNetworkDiagGridColGap = 174;
constexpr int kNetworkDiagGridW = 160;
constexpr size_t kNetworkDiagSummaryTextSize = 64;
constexpr const char *kNetworkDiagTitle = "网络检测";
constexpr const char *kNetworkDiagSummaryReady = "准备检测...";
constexpr const char *kNetworkDiagSummaryRunning = "检测中...";
constexpr const char *kNetworkDiagSummaryDone = "检测完成";
constexpr const char *kNetworkDiagSummaryIdle = "等待开始";
constexpr const char *kNetworkDiagLinePlaceholder = "--";
constexpr const char *kNetworkDiagHintIdle = "Hold KEY to return";
constexpr const char *kNetworkDiagHintRunning = "Checking... Hold KEY to return";
constexpr int kSettingsOtaBarFrameX = 164;
constexpr int kSettingsOtaBarFrameY = 203;
constexpr int kSettingsOtaBarFrameW = 200;
constexpr int kSettingsOtaBarFrameH = 9;
constexpr int kSettingsOtaBarInset = 2;
constexpr int kSettingsOtaBarFillW = kSettingsOtaBarFrameW - kSettingsOtaBarInset * 2;
constexpr int kSettingsOtaBarFillH = kSettingsOtaBarFrameH - kSettingsOtaBarInset * 2;
constexpr int kSettingsOtaProgressMax = 100;
constexpr size_t kSettingsOtaLineTextSize = 96;
constexpr size_t kSettingsOtaHintTextSize = 48;
constexpr const char *kSettingsOtaUpdatingWithSpeedFormat = "OTA %d%%  %d KB/s";
constexpr const char *kSettingsOtaUpdatingFormat = "OTA %d%%";
constexpr const char *kSettingsOtaCurrentVersionFormat = "当前版本 %s";
constexpr const char *kSettingsOtaHintDownloading = "下载中，请等待";
constexpr const char *kSettingsOtaHintInstall = "BOOT安装更新";
constexpr const char *kSettingsOtaHintChecking = "正在检查，请等待";
constexpr const char *kSettingsOtaHintRebooting = "即将重启";
constexpr const char *kSettingsOtaHintRetry = "BOOT重新检查";
constexpr const char *kSettingsOtaHintCheck = "BOOT开始检查";
constexpr int kSettingsPrimaryX = 12;
constexpr int kSettingsPrimaryW = 112;
constexpr int kSettingsSecondaryX = 150;
constexpr int kSettingsSecondaryW = 228;
constexpr int kSettingsSecondaryH = 30;
constexpr int kSettingsSwitchDotX = 362;
constexpr int kSettingsSwitchDotYOffset = 8;
constexpr int kSettingsSwitchDotSize = 12;
constexpr int kSettingsSwitchTextX = 352;
constexpr int kSettingsSwitchTextYOffset = 6;
constexpr int kSettingsSwitchTextW = 28;
constexpr int kSettingsSwitchTextH = 18;
constexpr size_t kSettingsSecondaryTextSize = 56;
constexpr int kSettingsListRowY[] = {66, 105, 144, 183, 222, 222, 222};
constexpr int kSettingsGridRowY[] = {66, 105, 144};
constexpr size_t kSettingsListRowCount = array_count(kSettingsListRowY);
constexpr size_t kSettingsGridRowCount = array_count(kSettingsGridRowY);
constexpr int kSettingsGridColumns = 2;
constexpr int kSettingsGridLeftX = 150;
constexpr int kSettingsGridRightX = 267;
constexpr int kSettingsGridColW = 111;
constexpr int kSettingsGridSwitchDotXOffset = 92;
constexpr int kSettingsGridSwitchDotYOffset = 9;
constexpr int kSettingsGridSwitchTextSystemXOffset = 80;
constexpr int kSettingsGridSwitchTextDisplayXOffset = 90;
constexpr int kSettingsGridSwitchTextSystemW = 26;
constexpr int kSettingsGridSwitchTextDisplayW = 30;
constexpr int kSettingsGridSwitchTextYOffset = 7;
constexpr const char *kSettingsPrimaryItems[kSettingsPrimaryCount] = {"网络", "声音", "显示", "系统"};
constexpr const char *kSettingsNetworkSyncTimeText = "同步时间";
constexpr const char *kSettingsNetworkSyncWeatherText = "同步天气";
constexpr const char *kSettingsNetworkSayingText = "更新一言";
constexpr const char *kSettingsWeatherCityManualFormat = "天气城市 %s";
constexpr const char *kSettingsWeatherCityAutoText = "天气城市 自动";
constexpr const char *kSettingsSoundVolumeFormat = "音量 %d%%";
constexpr const char *kSettingsSoundChoiceFormat = "声音选择 %d";
constexpr const char *kSettingsHourlyText = "整点提醒 7:00 - 22:00";
constexpr const char *kSettingsAllDayText = "全天提醒 0:00 - 24:00";
constexpr const char *kSettingsPageOrderText = "页面顺序";
constexpr const char *kSettingsOfflineFormat = "离线模式 %s";
constexpr const char *kSettingsOfflineOnText = "开";
constexpr const char *kSettingsOfflineOffText = "关";
constexpr const char *kSettingsNetworkDiagText = "网络检测";
constexpr const char *kSettingsFactoryResetConfirmText = "确认恢复";
constexpr const char *kSettingsFactoryResetText = "恢复出厂设置";
constexpr const char *kSettingsSystemInfoText = "关于本机";
constexpr const char *kSettingsCheckUpdateText = "检查更新";
constexpr int kBatteryFrameX = 20;
constexpr int kBatteryFrameY = 17;
constexpr int kBatteryFrameW = 34;
constexpr int kBatteryFrameH = 16;
constexpr int kBatteryInnerX = 2;
constexpr int kBatteryInnerY = 2;
constexpr int kBatteryInnerW = 30;
constexpr int kBatteryInnerH = 12;
constexpr int kBatteryTipX = 55;
constexpr int kBatteryTipY = 22;
constexpr int kBatteryTipW = 3;
constexpr int kBatteryTipH = 6;
constexpr int kBatterySegmentCount = 5;
constexpr int kBatteryPercentPerSegment = 20;
constexpr int kBatterySegmentX = 3;
constexpr int kBatterySegmentY = 4;
constexpr int kBatterySegmentW = 4;
constexpr int kBatterySegmentH = 8;
constexpr int kBatterySegmentGap = 6;
constexpr size_t kSetupStatusLineSize = 96;
constexpr const char *kSetupStatusTitle = "Setup Mode";
constexpr const char *kSetupStatusPlaceholder = "--";
constexpr const char *kSetupApSsidFormat = "AP SSID: %s";
constexpr const char *kSetupApPasswordFormat = "AP Password: %s";
constexpr const char *kSetupPortalIpFormat = "Portal IP: %s";
constexpr const char *kSetupStaSsidFormat = "STA SSID: %s";
constexpr const char *kSetupStaIpFormat = "STA IP: %s";
constexpr const char *kSetupStaIpReasonFormat = "STA IP: --  reason %d";
constexpr const char *kSetupStaIpPlaceholder = "STA IP: --";
constexpr size_t kInfoTimeTextSize = 32;
constexpr size_t kInfoLineTextSize = 96;
constexpr const char *kInfoLastNtpFormat = "Last NTP: %s";
constexpr const char *kInfoWifiFormat = "WiFi: %s";
constexpr const char *kInfoLastWeatherFormat = "Last Weather: %s";
constexpr const char *kInfoBatteryFullFormat = "Battery: %d%%  %.2fV";
constexpr const char *kInfoBatteryPercentOnlyFormat = "Battery: %d%%  --";
constexpr const char *kInfoBatteryPlaceholder = "Battery: --  --";
constexpr const char *kInfoVersionFormat = "Version: %s / %s";
constexpr int kInfoLabelY[] = {70, 104, 138, 172, 206};
constexpr size_t kInfoLabelCount = array_count(kInfoLabelY);
static_assert(kSettingsListRowCount == kSettingsSecondaryMaxCount);
static_assert(kSettingsGridRowCount * kSettingsGridColumns >= kWorkPageCount);
static_assert(array_count(kSettingsPrimaryItems) == kSettingsPrimaryCount);

void set_secondary_text(char items[][kSettingsSecondaryTextSize], int index, const char *text)
{
    copy_text(items[index], kSettingsSecondaryTextSize, text);
}

void hide_settings_switch_slot(int index)
{
    if (g_settings_switch_dots[index]) {
        set_obj_visible(g_settings_switch_dots[index], false);
    }
    if (g_settings_switch_texts[index]) {
        set_obj_visible(g_settings_switch_texts[index], false);
    }
}
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
            bool black = packed_1bit_bit_is_set(pixels, bit);
            lv_canvas_set_px_color(g_boot_anim_canvas, x, y, black ? lv_color_black() : lv_color_white());
        }
    }
    lv_obj_invalidate(g_boot_anim_canvas);
}

void boot_anim_task(void *)
{
    int frame = 0;
    while (g_boot_anim_running) {
        if (Lvgl_lock(kBootAnimLvglLockTimeoutMs)) {
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
    if (Lvgl_lock(kBootAnimFinishLvglLockTimeoutMs)) {
        draw_boot_anim_frame_index(BOOT_ANIM_FRAME_COUNT - 1);
        g_boot_anim_current_frame = BOOT_ANIM_FRAME_COUNT - 1;
        lv_refr_now(nullptr);
        Lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(kBootAnimFinishHoldMs));
}

void style_battery_part(lv_obj_t *obj, bool filled)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_bg_color(obj, filled ? lv_color_black() : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

void style_battery_frame(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
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
    lv_obj_set_pos(frame, kBatteryFrameX, kBatteryFrameY);
    lv_obj_set_size(frame, kBatteryFrameW, kBatteryFrameH);
    style_battery_frame(frame);

    lv_obj_t *inner = lv_obj_create(frame);
    if (!inner) {
        ESP_LOGW(TAG, "battery inner create failed");
        return;
    }
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(inner, kBatteryInnerX, kBatteryInnerY);
    lv_obj_set_size(inner, kBatteryInnerW, kBatteryInnerH);
    style_battery_part(inner, false);
    lv_obj_set_style_border_width(inner, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(inner, 2, LV_PART_MAIN);

    lv_obj_t *tip = lv_obj_create(parent);
    if (!tip) {
        ESP_LOGW(TAG, "battery tip create failed");
        return;
    }
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tip, kBatteryTipX, kBatteryTipY);
    lv_obj_set_size(tip, kBatteryTipW, kBatteryTipH);
    style_battery_part(tip, true);
    lv_obj_set_style_border_width(tip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 1, LV_PART_MAIN);

    for (int i = 0; i < kBatterySegmentCount; ++i) {
        segments[i] = lv_obj_create(frame);
        if (!segments[i]) {
            ESP_LOGW(TAG, "battery segment %d create failed", i);
            continue;
        }
        lv_obj_clear_flag(segments[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(segments[i], kBatterySegmentX + i * kBatterySegmentGap, kBatterySegmentY);
        lv_obj_set_size(segments[i], kBatterySegmentW, kBatterySegmentH);
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
    if (title) {
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "boot title create failed");
    }

    g_boot_status_label = make_label_with_font(screen, 28, 64, 344, 24, "Starting...", &lv_font_montserrat_16);
    if (g_boot_status_label) {
        lv_obj_set_style_text_align(g_boot_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "boot status label create failed");
    }

    g_boot_detail_label = make_label_with_font(screen, 28, 256, 344, 22, "Preparing system", &lv_font_montserrat_14);
    if (g_boot_detail_label) {
        lv_obj_set_style_text_align(g_boot_detail_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "boot detail label create failed");
    }

    lv_obj_t *version = make_label_with_font(screen, 28, 226, 344, 24, APP_VERSION, &lv_font_montserrat_16);
    if (version) {
        lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "boot version label create failed");
    }

    if (!g_boot_anim_canvas_buf) {
        g_boot_anim_canvas_buf = alloc_canvas_buffer(BOOT_ANIM_WIDTH, BOOT_ANIM_HEIGHT);
    }
    g_boot_anim_canvas = lv_canvas_create(screen);
    if (g_boot_anim_canvas) {
        lv_obj_clear_flag(g_boot_anim_canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(g_boot_anim_canvas, 144, 100);
        lv_obj_set_size(g_boot_anim_canvas, BOOT_ANIM_WIDTH, BOOT_ANIM_HEIGHT);
        lv_obj_set_style_border_width(g_boot_anim_canvas, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(g_boot_anim_canvas, 0, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "boot anim canvas create failed");
    }
    if (g_boot_anim_canvas && g_boot_anim_canvas_buf) {
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
    if (Lvgl_lock(kBootScreenLvglLockTimeoutMs)) {
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
    if (Lvgl_lock(kBootScreenLvglLockTimeoutMs)) {
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
    if (!screen) {
        return;
    }
    g_info_root = screen;
    lv_obj_add_flag(g_info_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label_with_font(screen, 24, 18, 352, 26, "SYSTEM INFO", &lv_font_montserrat_16);
    if (title) {
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "system info title create failed");
    }

    lv_obj_t *top_line = make_bar(screen, 24, 50, 352, 3);
    set_obj_black(top_line, true);

    static_assert(kInfoLabelCount == array_count(g_info_labels),
                  "System Info labels and row coordinates must stay in sync");
    for (size_t i = 0; i < kInfoLabelCount; ++i) {
        g_info_labels[i] = make_label_with_font(screen, 30, kInfoLabelY[i], 340, 24, "--", &lv_font_montserrat_14);
    }

    lv_obj_t *bottom_line = make_bar(screen, 24, 238, 352, 3);
    set_obj_black(bottom_line, true);
    g_info_ota_label = make_label_with_font(screen, 24, 252, 352, 22, "Hold KEY to return", &lv_font_montserrat_14);
    if (g_info_ota_label) {
        lv_obj_set_style_text_align(g_info_ota_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "system info return label create failed");
    }
    g_info_ota_hint_label = nullptr;
    g_info_ota_bar_frame = nullptr;
    g_info_ota_bar_fill = nullptr;
}

void update_boot_info_page()
{
    char ntp[kInfoTimeTextSize];
    char weather[kInfoTimeTextSize];
    char line[kInfoLineTextSize];

    format_time_or_dash(g_last_ntp_sync_time, ntp, sizeof(ntp));
    snprintf(line, sizeof(line), kInfoLastNtpFormat, ntp);
    set_label_text_if_changed(g_info_labels[0], line);

    snprintf(line, sizeof(line), kInfoWifiFormat, g_wifi_ssid[0] ? g_wifi_ssid : "--");
    set_label_text_if_changed(g_info_labels[1], line);

    format_time_or_dash(g_last_weather_sync_time, weather, sizeof(weather));
    snprintf(line, sizeof(line), kInfoLastWeatherFormat, weather);
    set_label_text_if_changed(g_info_labels[2], line);

    if (g_battery_percent >= 0 && g_battery_voltage >= 0.0f) {
        snprintf(line, sizeof(line), kInfoBatteryFullFormat, g_battery_percent, g_battery_voltage);
    } else if (g_battery_percent >= 0) {
        snprintf(line, sizeof(line), kInfoBatteryPercentOnlyFormat, g_battery_percent);
    } else {
        copy_text(line, sizeof(line), kInfoBatteryPlaceholder);
    }
    set_label_text_if_changed(g_info_labels[3], line);

    snprintf(line, sizeof(line), kInfoVersionFormat, APP_VERSION, APP_BUILD_DATE);
    set_label_text_if_changed(g_info_labels[4], line);

    ota_reset_status_if_idle();
}

void build_network_diag_page()
{
    if (g_network_diag_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    g_network_diag_root = screen;
    lv_obj_add_flag(g_network_diag_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(screen, 24, 18, 352, 28, kNetworkDiagTitle);
    if (title) {
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "network diag title create failed");
    }

    lv_obj_t *top_line = make_bar(screen, 24, 52, 352, 3);
    set_obj_black(top_line, true);

    g_network_diag_summary_label = make_label(screen, 24, 62, 352, 22, kNetworkDiagSummaryReady);
    if (g_network_diag_summary_label) {
        lv_obj_set_style_text_align(g_network_diag_summary_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "network diag summary label create failed");
    }

    for (int i = 0; i < kNetworkDiagLineCount; ++i) {
        int x = kNetworkDiagWideX;
        int y = kNetworkDiagLocalIpY;
        int w = kNetworkDiagWideW;
        if (i == kNetworkDiagLocalIpLine) {
            y = kNetworkDiagLocalIpY;
        } else if (i == kNetworkDiagPublicIpLine) {
            y = kNetworkDiagPublicIpY;
        } else {
            int grid = i - kNetworkDiagGridFirstLine;
            int row = grid / kNetworkDiagGridColumns;
            int col = grid % kNetworkDiagGridColumns;
            x = kNetworkDiagWideX + col * kNetworkDiagGridColGap;
            y = kNetworkDiagGridStartY + row * kNetworkDiagGridRowGap;
            w = kNetworkDiagGridW;
            if (i == kNetworkDiagWideLine) {
                x = kNetworkDiagWideX;
                w = kNetworkDiagWideW;
            }
        }
        g_network_diag_labels[i] = make_label(screen, x, y, w, 22, kNetworkDiagLinePlaceholder);
        if (g_network_diag_labels[i]) {
            lv_label_set_long_mode(g_network_diag_labels[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(g_network_diag_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        } else {
            ESP_LOGW(TAG, "network diag line %d label create failed", i);
        }
    }

    lv_obj_t *bottom_line = make_bar(screen, 24, 266, 352, 2);
    set_obj_black(bottom_line, true);
    g_network_diag_hint_label = make_label(screen, 24, 272, 352, 20, kNetworkDiagHintIdle);
    if (g_network_diag_hint_label) {
        lv_obj_set_style_text_align(g_network_diag_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "network diag hint label create failed");
    }
}

bool update_network_diag_page()
{
    bool changed = false;
    char summary[kNetworkDiagSummaryTextSize];
    if (g_network_diag_state == kNetworkDiagRunning) {
        copy_text(summary, sizeof(summary), kNetworkDiagSummaryRunning);
    } else if (g_network_diag_state == kNetworkDiagDone) {
        copy_text(summary, sizeof(summary), kNetworkDiagSummaryDone);
    } else {
        copy_text(summary, sizeof(summary), kNetworkDiagSummaryIdle);
    }
    changed |= set_label_text_if_changed(g_network_diag_summary_label, summary);
    for (int i = 0; i < kNetworkDiagLineCount; ++i) {
        changed |= set_label_text_if_changed(g_network_diag_labels[i],
                                             g_network_diag_lines[i][0] ? g_network_diag_lines[i] :
                                                                          kNetworkDiagLinePlaceholder);
    }
    changed |= set_label_text_if_changed(g_network_diag_hint_label,
                                         g_network_diag_state == kNetworkDiagRunning ? kNetworkDiagHintRunning :
                                                                                       kNetworkDiagHintIdle);
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
    g_factory_reset_confirm_pending = false;
    g_offline_disable_confirm_pending = false;
    g_weather_city_clear_confirm_pending = false;
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
        s_settings_primary_exit_block_until = xTaskGetTickCount() + pdMS_TO_TICKS(kSettingsPrimaryExitBlockMs);
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
    if (!screen) {
        return;
    }
    g_settings_root = screen;
    lv_obj_add_flag(g_settings_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(screen, 24, 18, 352, 28, "设置");
    if (title) {
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "settings title create failed");
    }
    lv_obj_t *top_line = make_bar(screen, 24, 52, 352, 3);
    set_obj_black(top_line, true);

    lv_obj_t *separator = make_bar(screen, 136, 62, 2, 174);
    set_obj_black(separator, true);

    for (int i = 0; i < kSettingsPrimaryCount; ++i) {
        g_settings_labels[i] =
            make_label(screen, kSettingsPrimaryX, kSettingsListRowY[i], kSettingsPrimaryW, kSettingsSecondaryH, "--");
        if (g_settings_labels[i]) {
            lv_label_set_long_mode(g_settings_labels[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(g_settings_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        } else {
            ESP_LOGW(TAG, "settings primary label create failed index=%d", i);
        }
    }
    for (int i = 0; i < kSettingsSecondaryMaxCount; ++i) {
        int slot = kSettingsPrimaryCount + i;
        g_settings_labels[slot] =
            make_label(screen, kSettingsSecondaryX, kSettingsListRowY[i], kSettingsSecondaryW, kSettingsSecondaryH, "--");
        if (g_settings_labels[slot]) {
            lv_label_set_long_mode(g_settings_labels[slot], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(g_settings_labels[slot], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        } else {
            ESP_LOGW(TAG, "settings secondary label create failed index=%d", i);
        }
        g_settings_switch_dots[i] = lv_obj_create(screen);
        if (g_settings_switch_dots[i]) {
            lv_obj_clear_flag(g_settings_switch_dots[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(g_settings_switch_dots[i],
                           kSettingsSwitchDotX,
                           kSettingsListRowY[i] + kSettingsSwitchDotYOffset);
            lv_obj_set_size(g_settings_switch_dots[i], kSettingsSwitchDotSize, kSettingsSwitchDotSize);
            style_settings_switch_dot(g_settings_switch_dots[i], false, false);
            lv_obj_add_flag(g_settings_switch_dots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGW(TAG, "settings switch dot create failed index=%d", i);
        }
        g_settings_switch_texts[i] =
            make_label(screen,
                       kSettingsSwitchTextX,
                       kSettingsListRowY[i] + kSettingsSwitchTextYOffset,
                       kSettingsSwitchTextW,
                       kSettingsSwitchTextH,
                       "");
        if (g_settings_switch_texts[i]) {
            lv_obj_set_style_text_align(g_settings_switch_texts[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_pad_all(g_settings_switch_texts[i], 0, LV_PART_MAIN);
            lv_label_set_long_mode(g_settings_switch_texts[i], LV_LABEL_LONG_CLIP);
            lv_obj_add_flag(g_settings_switch_texts[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGW(TAG, "settings switch text create failed index=%d", i);
        }
    }
    g_settings_ota_status_label = make_label(screen, kSettingsSecondaryX, 176, kSettingsSecondaryW, 22, "");
    if (g_settings_ota_status_label) {
        lv_obj_set_style_text_align(g_settings_ota_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "settings ota status label create failed");
    }
    g_settings_ota_bar_frame = lv_obj_create(screen);
    if (g_settings_ota_bar_frame) {
        lv_obj_clear_flag(g_settings_ota_bar_frame, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(g_settings_ota_bar_frame, kSettingsOtaBarFrameX, kSettingsOtaBarFrameY);
        lv_obj_set_size(g_settings_ota_bar_frame, kSettingsOtaBarFrameW, kSettingsOtaBarFrameH);
        lv_obj_set_style_bg_color(g_settings_ota_bar_frame, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_settings_ota_bar_frame, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(g_settings_ota_bar_frame, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(g_settings_ota_bar_frame, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(g_settings_ota_bar_frame, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_all(g_settings_ota_bar_frame, 0, LV_PART_MAIN);
        lv_obj_add_flag(g_settings_ota_bar_frame, LV_OBJ_FLAG_HIDDEN);
    } else {
        ESP_LOGW(TAG, "settings ota bar frame create failed");
    }
    g_settings_ota_bar_fill = make_bar(screen,
                                       kSettingsOtaBarFrameX + kSettingsOtaBarInset,
                                       kSettingsOtaBarFrameY + kSettingsOtaBarInset,
                                       1,
                                       kSettingsOtaBarFillH);
    set_obj_black(g_settings_ota_bar_fill, true);
    if (g_settings_ota_bar_fill) {
        lv_obj_set_style_radius(g_settings_ota_bar_fill, 2, LV_PART_MAIN);
        lv_obj_add_flag(g_settings_ota_bar_fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        ESP_LOGW(TAG, "settings ota bar fill create failed");
    }
    g_settings_ota_hint_label = make_label(screen, kSettingsSecondaryX, 218, kSettingsSecondaryW, 20, "");
    if (g_settings_ota_hint_label) {
        lv_obj_set_style_text_align(g_settings_ota_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "settings ota hint label create failed");
    }

    g_settings_feedback_label = make_label(screen, 24, 246, 352, 20, "");
    if (g_settings_feedback_label) {
        lv_obj_set_style_text_align(g_settings_feedback_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "settings feedback label create failed");
    }

    lv_obj_t *hint = make_label(screen, 24, 270, 352, 22, "KEY选择  长按返回  BOOT确认");
    if (hint) {
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        ESP_LOGW(TAG, "settings hint label create failed");
    }
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

    char secondary_items[kSettingsSecondaryMaxCount][kSettingsSecondaryTextSize] = {};
    int primary = clamp_settings_primary(g_settings_primary_selection);
    int selected = clamp_settings_secondary(primary, g_settings_selection);
    g_settings_primary_selection = primary;
    g_settings_selection = selected;
    if (g_settings_page_order_mode) {
        normalize_work_page_order();
    }

    if (primary == kSettingsPrimaryNetwork) {
        set_secondary_text(secondary_items, kNetworkSettingsNtpItem, kSettingsNetworkSyncTimeText);
        set_secondary_text(secondary_items, kNetworkSettingsWeatherItem, kSettingsNetworkSyncWeatherText);
        set_secondary_text(secondary_items, kNetworkSettingsSayingItem, kSettingsNetworkSayingText);
        if (g_has_manual_weather_city) {
            snprintf(secondary_items[kNetworkSettingsWeatherCityItem],
                     sizeof(secondary_items[kNetworkSettingsWeatherCityItem]),
                     kSettingsWeatherCityManualFormat,
                     g_manual_weather_city);
        } else {
            set_secondary_text(secondary_items, kNetworkSettingsWeatherCityItem, kSettingsWeatherCityAutoText);
        }
    } else if (primary == kSettingsPrimarySound) {
        snprintf(secondary_items[kSoundSettingsVolumeItem],
                 sizeof(secondary_items[kSoundSettingsVolumeItem]),
                 kSettingsSoundVolumeFormat,
                 g_chime_volume_percent);
        snprintf(secondary_items[kSoundSettingsSoundItem],
                 sizeof(secondary_items[kSoundSettingsSoundItem]),
                 kSettingsSoundChoiceFormat,
                 g_chime_sound_index + 1);
        set_secondary_text(secondary_items, kSoundSettingsHourlyItem, kSettingsHourlyText);
        set_secondary_text(secondary_items, kSoundSettingsAllDayItem, kSettingsAllDayText);
    } else if (primary == kSettingsPrimaryDisplay) {
        for (int i = 0; i < kDisplaySettingsPageItemCount; ++i) {
            set_secondary_text(secondary_items, i, work_page_name(display_settings_item_work_page(i)));
        }
        set_secondary_text(secondary_items, kDisplaySettingsOrderItem, kSettingsPageOrderText);
    } else {
        snprintf(secondary_items[kSystemSettingsOfflineItem],
                 sizeof(secondary_items[kSystemSettingsOfflineItem]),
                 kSettingsOfflineFormat,
                 g_offline_mode_ui_enabled ? kSettingsOfflineOnText : kSettingsOfflineOffText);
        set_secondary_text(secondary_items, kSystemSettingsNetworkDiagItem, kSettingsNetworkDiagText);
        set_secondary_text(secondary_items,
                           kSystemSettingsFactoryResetItem,
                           g_factory_reset_confirm_pending ? kSettingsFactoryResetConfirmText
                                                           : kSettingsFactoryResetText);
        set_secondary_text(secondary_items, kSystemSettingsInfoItem, kSettingsSystemInfoText);
        set_secondary_text(secondary_items, kSystemSettingsOtaItem, kSettingsCheckUpdateText);
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
            changed |= set_label_text_if_changed(g_settings_labels[i], kSettingsPrimaryItems[i]);
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
            if (i >= kWorkPageCount) {
                set_obj_visible(g_settings_labels[slot], false);
                hide_settings_switch_slot(i);
                continue;
            }
            int col = i & 1;
            int row = i >> 1;
            lv_obj_set_pos(g_settings_labels[slot],
                           col == 0 ? kSettingsGridLeftX : kSettingsGridRightX,
                           kSettingsGridRowY[row]);
            lv_obj_set_size(g_settings_labels[slot], kSettingsGridColW, kSettingsSecondaryH);
            if (i < kWorkPageCount) {
                snprintf(secondary_items[i], sizeof(secondary_items[i]), "%d %s", i + 1,
                         work_page_name(g_work_page_order[i]));
            }
            hide_settings_switch_slot(i);
        } else if (primary == kSettingsPrimaryDisplay || primary == kSettingsPrimarySystem) {
            int col = i & 1;
            int row = i >> 1;
            bool grid_item = primary == kSettingsPrimaryDisplay
                                 ? i < kDisplaySettingsPageItemCount
                                 : i < kSystemSettingsGridItemCount;
            if (grid_item) {
                int grid_x = col == 0 ? kSettingsGridLeftX : kSettingsGridRightX;
                lv_obj_set_pos(g_settings_labels[slot], grid_x, kSettingsGridRowY[row]);
                lv_obj_set_size(g_settings_labels[slot], kSettingsGridColW, kSettingsSecondaryH);
                if (g_settings_switch_dots[i]) {
                    lv_obj_set_pos(g_settings_switch_dots[i],
                                   grid_x + kSettingsGridSwitchDotXOffset,
                                   kSettingsGridRowY[row] + kSettingsGridSwitchDotYOffset);
                }
                if (g_settings_switch_texts[i]) {
                    int status_x = grid_x +
                                   (primary == kSettingsPrimarySystem ? kSettingsGridSwitchTextSystemXOffset
                                                                      : kSettingsGridSwitchTextDisplayXOffset);
                    int status_w = primary == kSettingsPrimarySystem ? kSettingsGridSwitchTextSystemW
                                                                     : kSettingsGridSwitchTextDisplayW;
                    lv_obj_set_pos(g_settings_switch_texts[i],
                                   status_x,
                                   kSettingsGridRowY[row] + kSettingsGridSwitchTextYOffset);
                    lv_obj_set_size(g_settings_switch_texts[i], status_w, kSettingsSwitchTextH);
                }
            } else {
                lv_obj_set_pos(g_settings_labels[slot],
                               kSettingsSecondaryX,
                               primary == kSettingsPrimarySystem ? 144 : 183);
                lv_obj_set_size(g_settings_labels[slot], kSettingsSecondaryW, kSettingsSecondaryH);
                hide_settings_switch_slot(i);
            }
        } else {
            lv_obj_set_pos(g_settings_labels[slot], kSettingsSecondaryX, kSettingsListRowY[i]);
            lv_obj_set_size(g_settings_labels[slot], kSettingsSecondaryW, kSettingsSecondaryH);
            if (g_settings_switch_dots[i]) {
                lv_obj_set_pos(g_settings_switch_dots[i],
                               kSettingsSwitchDotX,
                               kSettingsListRowY[i] + kSettingsSwitchDotYOffset);
            }
            if (g_settings_switch_texts[i]) {
                lv_obj_set_pos(g_settings_switch_texts[i],
                               kSettingsSwitchTextX,
                               kSettingsListRowY[i] + kSettingsSwitchTextYOffset);
                lv_obj_set_size(g_settings_switch_texts[i], kSettingsSwitchTextW, kSettingsSwitchTextH);
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
                if (primary == kSettingsPrimarySystem && i < kSystemSettingsGridItemCount) {
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
            if (i >= kSoundSettingsHourlyItem) {
                dot_visible = true;
                dot_on = i == kSoundSettingsHourlyItem ? g_hourly_chime_enabled : g_hourly_chime_all_day;
            }
        } else if (visible &&
                   primary == kSettingsPrimaryDisplay &&
                   !g_settings_page_order_mode &&
                   i < kDisplaySettingsPageItemCount) {
            dot_visible = true;
            int page = display_settings_item_work_page(i);
            if (page == kWorkPageWeatherClock) {
                dot_on = true;
            } else {
                dot_on = is_work_page_enabled(page);
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
    bool ota_panel_visible = primary == kSettingsPrimarySystem && selected == kSystemSettingsOtaItem;
    if (g_settings_ota_status_label) {
        char ota_line[kSettingsOtaLineTextSize] = "";
        char ota_hint[kSettingsOtaHintTextSize] = "";
        bool progress_visible = false;
        int progress = g_ota_progress;
        if (ota_panel_visible) {
            if (g_ota_state == kOtaUpdating && progress >= 0) {
                progress_visible = true;
                if (g_ota_speed_kbps > 0) {
                    snprintf(ota_line, sizeof(ota_line), kSettingsOtaUpdatingWithSpeedFormat, progress, g_ota_speed_kbps);
                } else {
                    snprintf(ota_line, sizeof(ota_line), kSettingsOtaUpdatingFormat, progress);
                }
                strlcpy(ota_hint, kSettingsOtaHintDownloading, sizeof(ota_hint));
            } else if (g_ota_state == kOtaAvailable) {
                copy_text(ota_line, sizeof(ota_line), g_ota_status);
                strlcpy(ota_hint, kSettingsOtaHintInstall, sizeof(ota_hint));
            } else if (g_ota_state == kOtaChecking) {
                copy_text(ota_line, sizeof(ota_line), g_ota_status);
                strlcpy(ota_hint, kSettingsOtaHintChecking, sizeof(ota_hint));
            } else if (g_ota_state == kOtaSucceeded) {
                progress_visible = true;
                progress = kSettingsOtaProgressMax;
                copy_text(ota_line, sizeof(ota_line), g_ota_status);
                strlcpy(ota_hint, kSettingsOtaHintRebooting, sizeof(ota_hint));
            } else if (g_ota_state == kOtaFailed || g_ota_state == kOtaNoUpdate) {
                copy_text(ota_line, sizeof(ota_line), g_ota_status);
                strlcpy(ota_hint, kSettingsOtaHintRetry, sizeof(ota_hint));
            } else {
                snprintf(ota_line, sizeof(ota_line), kSettingsOtaCurrentVersionFormat, APP_VERSION);
                strlcpy(ota_hint, kSettingsOtaHintCheck, sizeof(ota_hint));
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
            } else if (clamped > kSettingsOtaProgressMax) {
                clamped = kSettingsOtaProgressMax;
            }
            int fill_w = (kSettingsOtaBarFillW * clamped) / kSettingsOtaProgressMax;
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
    char line[kSetupStatusLineSize];
    if (!g_setup_status_labels[0]) {
        return false;
    }
    changed |= set_label_text_if_changed(g_setup_status_labels[0], kSetupStatusTitle);
    snprintf(line, sizeof(line), kSetupApSsidFormat, g_ap_ssid[0] ? g_ap_ssid : kSetupStatusPlaceholder);
    changed |= set_label_text_if_changed(g_setup_status_labels[1], line);
    snprintf(line, sizeof(line), kSetupApPasswordFormat, kSetupApPassword);
    changed |= set_label_text_if_changed(g_setup_status_labels[2], line);
    snprintf(line, sizeof(line), kSetupPortalIpFormat, kSetupPortalIp);
    changed |= set_label_text_if_changed(g_setup_status_labels[3], line);
    snprintf(line, sizeof(line), kSetupStaSsidFormat, g_wifi_ssid[0] ? g_wifi_ssid : kSetupStatusPlaceholder);
    changed |= set_label_text_if_changed(g_setup_status_labels[4], line);
    if (g_sta_ip[0]) {
        snprintf(line, sizeof(line), kSetupStaIpFormat, g_sta_ip);
    } else if (g_last_wifi_disconnect_reason) {
        snprintf(line, sizeof(line), kSetupStaIpReasonFormat, g_last_wifi_disconnect_reason);
    } else {
        copy_text(line, sizeof(line), kSetupStaIpPlaceholder);
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
        filled = (percent + kBatteryPercentPerSegment - 1) / kBatteryPercentPerSegment;
        if (charging && percent < 100) {
            blink_index = percent / kBatteryPercentPerSegment;
            if (blink_index >= kBatterySegmentCount) {
                blink_index = kBatterySegmentCount - 1;
            }
        }
    }
    for (int i = 0; i < kBatterySegmentCount; ++i) {
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
