// 构建和刷新天气时钟主页的时间、天气、温湿度和状态区域。
#include "ui_views.h"

#include "audio_services.h"
#include "network_services.h"
#include "ota_services.h"
#include "sensor_services.h"

namespace {
constexpr int kChimeVolumeLevels[] = {20, 40, 60, 80, 100};
template <typename T, size_t N>
constexpr size_t array_count(const T (&)[N])
{
    return N;
}

constexpr int kChimeVolumeLevelCount = static_cast<int>(array_count(kChimeVolumeLevels));
constexpr int kDefaultChimeVolumePercent = kChimeVolumeLevels[0];
constexpr int kSettingsFeedbackDefaultMs = 2500;
constexpr int kSettingsFeedbackBusyMs = 2000;
constexpr int kSettingsFeedbackSavedMs = 1800;
constexpr int kSettingsFeedbackInstructionMs = 3500;
constexpr const char *kSettingsSaveFailedFeedback = "保存失败";
constexpr const char *kSettingsOrderSavedFeedback = "页面顺序已保存";
constexpr const char *kSettingsSyncBusyFeedback = "请等待同步完成";
constexpr const char *kSettingsOfflineEnabledFeedback = "离线模式已开启";
constexpr const char *kSettingsOfflineDisabledFeedback = "离线模式已关闭";
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
constexpr const char *kWeatherClockRequiredFeedback = "天气时钟不可关闭";
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
constexpr size_t kClockDateTextSize = 48;
constexpr const char *kClockDateFormat = "%04d/%02d/%02d / %s";
constexpr int kTmYearOffset = 1900;
constexpr int kTmMonthOffset = 1;
constexpr int kSecondsPerMinute = 60;
constexpr int kMinutesPerHour = 60;
constexpr int kHoursPerDay = 24;
constexpr int kProgressSegmentCount = 60;
constexpr int kSecondsPerHour = kMinutesPerHour * kSecondsPerMinute;
constexpr int kSecondsPerDay = kHoursPerDay * kSecondsPerHour;
} // namespace

void build_clock_ui()
{
    if (g_clock_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    g_clock_root = screen;

    g_date_label = make_label(screen, 198, 15, 182, 26, "----/--/-- / 星期-");
    lv_obj_set_style_text_align(g_date_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    build_battery_icon(screen, g_battery_segments);

    g_alert_pill = lv_obj_create(screen);
    lv_obj_clear_flag(g_alert_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_alert_pill, 64, 11);
    lv_obj_set_size(g_alert_pill, 128, 26);
    lv_obj_set_style_bg_color(g_alert_pill, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_alert_pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_alert_pill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_alert_pill, 13, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alert_pill, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_alert_pill, LV_OBJ_FLAG_HIDDEN);

    if (!g_alert_icon_canvas_buf) {
        g_alert_icon_canvas_buf = alloc_canvas_buffer(WARNING_ICON_WIDTH, WARNING_ICON_HEIGHT);
    }
    g_alert_icon_canvas = lv_canvas_create(g_alert_pill);
    lv_obj_clear_flag(g_alert_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_alert_icon_canvas, 4, 4);
    lv_obj_set_size(g_alert_icon_canvas, WARNING_ICON_WIDTH, WARNING_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_alert_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alert_icon_canvas, 0, LV_PART_MAIN);
    if (g_alert_icon_canvas_buf) {
        lv_canvas_set_buffer(g_alert_icon_canvas,
                             g_alert_icon_canvas_buf,
                             WARNING_ICON_WIDTH,
                             WARNING_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        draw_1bit_icon(g_alert_icon_canvas,
                       WARNING_ICON_WIDTH,
                       WARNING_ICON_HEIGHT,
                       WARNING_ICON_BYTES_PER_ROW,
                       warning_icon_bits,
                       lv_color_white(),
                       lv_color_black());
    }
    g_alert_label = make_label_with_font(g_alert_pill, 24, 4, 94, 18, "", &zh_font_16);
    lv_obj_set_style_text_color(g_alert_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(g_alert_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(g_alert_label, LV_LABEL_LONG_CLIP);

    if (!g_chime_status_icon_canvas_buf) {
        g_chime_status_icon_canvas_buf = alloc_canvas_buffer(CHIME_STATUS_ICON_WIDTH, CHIME_STATUS_ICON_HEIGHT);
    }
    g_chime_status_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_chime_status_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_chime_status_icon_canvas, 64, 15);
    lv_obj_set_size(g_chime_status_icon_canvas, CHIME_STATUS_ICON_WIDTH, CHIME_STATUS_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_chime_status_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_chime_status_icon_canvas, 0, LV_PART_MAIN);
    if (g_chime_status_icon_canvas_buf) {
        lv_canvas_set_buffer(g_chime_status_icon_canvas,
                             g_chime_status_icon_canvas_buf,
                             CHIME_STATUS_ICON_WIDTH,
                             CHIME_STATUS_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        draw_1bit_icon(g_chime_status_icon_canvas,
                       CHIME_STATUS_ICON_WIDTH,
                       CHIME_STATUS_ICON_HEIGHT,
                       CHIME_STATUS_ICON_BYTES_PER_ROW,
                       chime_status_icon_bits,
                       lv_color_black(),
                       lv_color_white());
    }
    lv_obj_add_flag(g_chime_status_icon_canvas, LV_OBJ_FLAG_HIDDEN);

    if (!g_wifi_status_icon_canvas_buf) {
        g_wifi_status_icon_canvas_buf = alloc_canvas_buffer(WIFI_STATUS_ICON_WIDTH, WIFI_STATUS_ICON_HEIGHT);
    }
    g_wifi_status_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_wifi_status_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_wifi_status_icon_canvas, 90, 15);
    lv_obj_set_size(g_wifi_status_icon_canvas, WIFI_STATUS_ICON_WIDTH, WIFI_STATUS_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_wifi_status_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_wifi_status_icon_canvas, 0, LV_PART_MAIN);
    if (g_wifi_status_icon_canvas_buf) {
        lv_canvas_set_buffer(g_wifi_status_icon_canvas,
                             g_wifi_status_icon_canvas_buf,
                             WIFI_STATUS_ICON_WIDTH,
                             WIFI_STATUS_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        draw_1bit_icon(g_wifi_status_icon_canvas,
                       WIFI_STATUS_ICON_WIDTH,
                       WIFI_STATUS_ICON_HEIGHT,
                       WIFI_STATUS_ICON_BYTES_PER_ROW,
                       wifi_status_icon_bits,
                       lv_color_black(),
                       lv_color_white());
    }
    lv_obj_add_flag(g_wifi_status_icon_canvas, LV_OBJ_FLAG_HIDDEN);

    g_weather_city_label = make_label(screen, 14, 196, 76, 20, kClockWeatherCityPlaceholder);
    remember_lower_panel_object(g_weather_city_label);
    lv_obj_set_style_text_align(g_weather_city_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_weather_icon_label = make_label(screen, 91, 194, 34, 38, "");
    remember_lower_panel_object(g_weather_icon_label);
    lv_obj_set_style_text_font(g_weather_icon_label, &qweather_icons_36, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_weather_icon_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_weather_icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_weather_info_label = make_label(screen, 14, 218, 76, 20, kClockWeatherInfoWaitingText);
    remember_lower_panel_object(g_weather_info_label);
    lv_label_set_long_mode(g_weather_info_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(g_weather_info_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    g_weather_temp_label = make_label(screen, 20, 242, 68, 20, kClockWeatherTempPlaceholder);
    g_weather_humi_label = make_label(screen, 20, 264, 68, 20, kClockWeatherHumidityPlaceholder);
    remember_lower_panel_object(g_weather_temp_label);
    remember_lower_panel_object(g_weather_humi_label);
    lv_obj_set_style_text_align(g_weather_temp_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_weather_humi_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (!g_temp_icon_canvas_buf) {
        g_temp_icon_canvas_buf = alloc_canvas_buffer(TEMP_ICON_WIDTH, TEMP_ICON_HEIGHT);
    }
    if (!g_humi_icon_canvas_buf) {
        g_humi_icon_canvas_buf = alloc_canvas_buffer(HUMI_ICON_WIDTH, HUMI_ICON_HEIGHT);
    }
    g_temp_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_temp_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_temp_icon_canvas, 152, 214);
    lv_obj_set_size(g_temp_icon_canvas, TEMP_ICON_WIDTH, TEMP_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_temp_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_temp_icon_canvas, 0, LV_PART_MAIN);
    if (g_temp_icon_canvas_buf) {
        lv_canvas_set_buffer(g_temp_icon_canvas,
                             g_temp_icon_canvas_buf,
                             TEMP_ICON_WIDTH,
                             TEMP_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        draw_1bit_icon(g_temp_icon_canvas,
                       TEMP_ICON_WIDTH,
                       TEMP_ICON_HEIGHT,
                       TEMP_ICON_BYTES_PER_ROW,
                       temp_icon_bits,
                       lv_color_black(),
                       lv_color_white());
    }
    g_humi_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_humi_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_humi_icon_canvas, 154, 244);
    lv_obj_set_size(g_humi_icon_canvas, HUMI_ICON_WIDTH, HUMI_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_humi_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_humi_icon_canvas, 0, LV_PART_MAIN);
    if (g_humi_icon_canvas_buf) {
        lv_canvas_set_buffer(g_humi_icon_canvas,
                             g_humi_icon_canvas_buf,
                             HUMI_ICON_WIDTH,
                             HUMI_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        draw_1bit_icon(g_humi_icon_canvas,
                       HUMI_ICON_WIDTH,
                       HUMI_ICON_HEIGHT,
                       HUMI_ICON_BYTES_PER_ROW,
                       humi_icon_bits,
                       lv_color_black(),
                       lv_color_white());
    }
    g_temp_label = make_label(screen, 174, 214, 62, 28, "--.-℃");
    g_humi_label = make_label(screen, 174, 246, 62, 28, "--.-%");
    remember_lower_panel_object(g_temp_icon_canvas);
    remember_lower_panel_object(g_humi_icon_canvas);
    remember_lower_panel_object(g_temp_label);
    remember_lower_panel_object(g_humi_label);
    lv_obj_set_style_text_align(g_temp_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_humi_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    if (!g_temp_trend_canvas_buf) {
        g_temp_trend_canvas_buf = alloc_canvas_buffer(TREND_ICON_WIDTH, TREND_ICON_HEIGHT);
    }
    if (!g_humi_trend_canvas_buf) {
        g_humi_trend_canvas_buf = alloc_canvas_buffer(TREND_ICON_WIDTH, TREND_ICON_HEIGHT);
    }
    g_temp_trend_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_temp_trend_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_temp_trend_canvas, 239, 215);
    lv_obj_set_size(g_temp_trend_canvas, TREND_ICON_WIDTH, TREND_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_temp_trend_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_temp_trend_canvas, 0, LV_PART_MAIN);
    if (g_temp_trend_canvas_buf) {
        lv_canvas_set_buffer(g_temp_trend_canvas,
                             g_temp_trend_canvas_buf,
                             TREND_ICON_WIDTH,
                             TREND_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        update_trend_icon(g_temp_trend_canvas, g_temp_trend, nullptr);
    }
    g_humi_trend_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_humi_trend_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_humi_trend_canvas, 239, 248);
    lv_obj_set_size(g_humi_trend_canvas, TREND_ICON_WIDTH, TREND_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_humi_trend_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_humi_trend_canvas, 0, LV_PART_MAIN);
    if (g_humi_trend_canvas_buf) {
        lv_canvas_set_buffer(g_humi_trend_canvas,
                             g_humi_trend_canvas_buf,
                             TREND_ICON_WIDTH,
                             TREND_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        update_trend_icon(g_humi_trend_canvas, g_humi_trend, nullptr);
    }
    remember_lower_panel_object(g_temp_trend_canvas);
    remember_lower_panel_object(g_humi_trend_canvas);
    constexpr int canvas_w = 292;
    constexpr int canvas_h = 92;
    if (!g_time_canvas_buf) {
        g_time_canvas_buf = alloc_canvas_buffer(canvas_w, canvas_h);
    }
    g_time_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_time_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_time_canvas, 18, 76);
    lv_obj_set_size(g_time_canvas, canvas_w, canvas_h);
    lv_obj_set_style_border_width(g_time_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_time_canvas, 0, LV_PART_MAIN);
    if (g_time_canvas_buf) {
        lv_canvas_set_buffer(g_time_canvas, g_time_canvas_buf, canvas_w, canvas_h, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(g_time_canvas, lv_color_white(), LV_OPA_COVER);
    }

    constexpr int second_w = 60;
    constexpr int second_h = 40;
    if (!g_second_canvas_buf) {
        g_second_canvas_buf = alloc_canvas_buffer(second_w, second_h);
    }
    g_second_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_second_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_second_canvas, 320, 124);
    lv_obj_set_size(g_second_canvas, second_w, second_h);
    lv_obj_set_style_border_width(g_second_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_second_canvas, 0, LV_PART_MAIN);
    if (g_second_canvas_buf) {
        lv_canvas_set_buffer(g_second_canvas, g_second_canvas_buf, second_w, second_h, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(g_second_canvas, lv_color_white(), LV_OPA_COVER);
    }

    if (!g_status_gif_canvas_buf) {
        g_status_gif_canvas_buf = alloc_canvas_buffer(STATUS_GIF_WIDTH, STATUS_GIF_HEIGHT);
    }
    g_status_gif_canvas = lv_canvas_create(screen);
    remember_lower_panel_object(g_status_gif_canvas);
    lv_obj_clear_flag(g_status_gif_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_status_gif_canvas, 279, 196);
    lv_obj_set_size(g_status_gif_canvas, STATUS_GIF_WIDTH, STATUS_GIF_HEIGHT);
    lv_obj_set_style_border_width(g_status_gif_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_status_gif_canvas, 0, LV_PART_MAIN);
    if (g_status_gif_canvas_buf) {
        lv_canvas_set_buffer(g_status_gif_canvas,
                             g_status_gif_canvas_buf,
                             STATUS_GIF_WIDTH,
                             STATUS_GIF_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(g_status_gif_canvas, lv_color_white(), LV_OPA_COVER);
        draw_status_gif_frame(0);
    }

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    lv_obj_t *bottom_line = make_bar(screen, 18, 184, 364, 4);
    build_progress_canvas(screen, &g_day_progress_canvas, &g_day_progress_canvas_buf, 59);
    build_progress_canvas(screen, &g_second_progress_canvas, &g_second_progress_canvas_buf, 180);
    g_panel_sep_a = make_bar(screen, 139, 188, 2, 102);
    g_panel_sep_b = make_bar(screen, 260, 188, 2, 102);
    remember_lower_panel_object(g_panel_sep_a);
    remember_lower_panel_object(g_panel_sep_b);
    set_obj_black(top_line, true);
    set_obj_black(bottom_line, true);
    set_obj_black(g_panel_sep_a, true);
    set_obj_black(g_panel_sep_b, true);

    if (!g_low_battery_icon_canvas_buf) {
        g_low_battery_icon_canvas_buf = alloc_canvas_buffer(LOW_BATTERY_ICON_WIDTH, LOW_BATTERY_ICON_HEIGHT);
    }
    g_low_battery_icon_canvas = lv_canvas_create(screen);
    lv_obj_clear_flag(g_low_battery_icon_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_low_battery_icon_canvas, 156, 214);
    lv_obj_set_size(g_low_battery_icon_canvas, LOW_BATTERY_ICON_WIDTH, LOW_BATTERY_ICON_HEIGHT);
    lv_obj_set_style_border_width(g_low_battery_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_low_battery_icon_canvas, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_low_battery_icon_canvas, LV_OBJ_FLAG_HIDDEN);
    if (g_low_battery_icon_canvas_buf) {
        lv_canvas_set_buffer(g_low_battery_icon_canvas,
                             g_low_battery_icon_canvas_buf,
                             LOW_BATTERY_ICON_WIDTH,
                             LOW_BATTERY_ICON_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        draw_1bit_icon(g_low_battery_icon_canvas,
                       LOW_BATTERY_ICON_WIDTH,
                       LOW_BATTERY_ICON_HEIGHT,
                       LOW_BATTERY_ICON_BYTES_PER_ROW,
                       low_battery_icon_bits,
                       lv_color_black(),
                       lv_color_white());
    }

    static const int setup_y[] = {194, 212, 230, 248, 266, 284};
    static const char *setup_text[] = {
        "Setup Mode",
        "AP SSID: --",
        "AP Password: --",
        "Portal IP: --",
        "STA SSID: --",
        "STA IP: --",
    };
    constexpr size_t kSetupStatusLabelCount = array_count(setup_y);
    static_assert(kSetupStatusLabelCount == array_count(setup_text),
                  "setup status coordinates and text must stay in sync");
    static_assert(kSetupStatusLabelCount == array_count(g_setup_status_labels),
                  "setup status label storage must match the rendered row count");
    for (size_t i = 0; i < kSetupStatusLabelCount; ++i) {
        g_setup_status_labels[i] = make_label_with_font(screen,
                                                        26,
                                                        setup_y[i],
                                                        348,
                                                        18,
                                                        setup_text[i],
                                                        &lv_font_montserrat_14);
        lv_obj_add_flag(g_setup_status_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}



bool update_time_ui(const struct tm &local, bool clock_page_active, int active_work_page)
{
    bool changed = false;
    static int last_chime_hour_key = -1;
    int minute_key = local.tm_hour * 60 + local.tm_min;
    if (clock_page_active && minute_key != g_last_ui_minute) {
        draw_time_canvas(local);
        if (!g_low_battery_mode) {
            int day_seconds = local.tm_hour * kSecondsPerHour + local.tm_min * kSecondsPerMinute + local.tm_sec;
            int day_filled = (day_seconds * kProgressSegmentCount) / kSecondsPerDay;
            update_progress_canvas(g_day_progress_canvas, day_filled, &g_last_day_progress_filled);
        }
        g_last_ui_minute = minute_key;
        changed = true;
    }
    if (clock_page_active && !g_low_battery_mode && local.tm_sec != g_last_ui_second) {
        draw_second_canvas(local);
        draw_status_gif_frame(local.tm_sec % STATUS_GIF_FRAME_COUNT);
        update_progress_canvas(g_second_progress_canvas, local.tm_sec + 1, &g_last_second_progress_filled);
        g_last_ui_second = local.tm_sec;
        changed = true;
    }

    int date_key = (local.tm_year + kTmYearOffset) * 10000 + (local.tm_mon + kTmMonthOffset) * 100 + local.tm_mday;
    int date_page = (active_work_page == kWorkPageWeatherClock || g_low_battery_mode || g_setup_portal_active)
                        ? kWorkPageWeatherClock
                        : active_work_page;
    if (date_key != g_last_ui_date_key || date_page != g_last_ui_date_page) {
        static const char *week_days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        char date[kClockDateTextSize];
        snprintf(date, sizeof(date), kClockDateFormat,
                 local.tm_year + kTmYearOffset,
                 local.tm_mon + kTmMonthOffset,
                 local.tm_mday,
                 week_days[local.tm_wday]);
        if (date_page == kWorkPageWeatherClock) {
            changed |= set_label_text_if_changed(g_date_label, date);
        } else if (date_page == kWorkPageHistory) {
            changed |= set_label_text_if_changed(g_history_date_label, date);
        } else if (date_page == kWorkPageGallery) {
            changed |= set_label_text_if_changed(g_gallery_date_label, date);
        } else if (date_page == kWorkPageCalendar) {
            changed |= set_label_text_if_changed(g_calendar_date_label, date);
        } else if (date_page == kWorkPageWeatherBoard) {
            changed |= set_label_text_if_changed(g_weather_board_date_label, date);
        } else if (date_page == kWorkPageFlipClock) {
            changed |= set_label_text_if_changed(g_flip_clock_date_label, date);
        }
        g_last_ui_date_key = date_key;
        g_last_ui_date_page = date_page;
    }

    int hour_key = local.tm_yday * 24 + local.tm_hour;
    bool chime_enabled = g_hourly_chime_enabled || g_hourly_chime_all_day;
    if (chime_enabled && !g_low_battery_mode &&
        local.tm_min == 0 && local.tm_sec <= 2 && hour_key != last_chime_hour_key) {
        last_chime_hour_key = hour_key;
        play_hourly_chime(local.tm_hour);
    }
    return changed;
}

void handle_settings_action()
{
    int primary = g_settings_primary_selection;
    if (primary < 0 || primary >= kSettingsPrimaryCount) {
        primary = kSettingsPrimaryNetwork;
    }
    int selected = g_settings_selection;
    int secondary_count = settings_secondary_count(primary);
    if (selected < 0 || selected >= secondary_count) {
        selected = 0;
    }
    g_settings_primary_selection = primary;
    g_settings_selection = selected;
    g_settings_last_activity_tick = xTaskGetTickCount();
    if (g_settings_page_order_mode) {
        normalize_work_page_order();
        int current = g_settings_page_order_selection;
        if (current < 0 || current >= kWorkPageCount) {
            current = 0;
        }
        int next = (current + 1) % kWorkPageCount;
        uint8_t tmp = g_work_page_order[current];
        g_work_page_order[current] = g_work_page_order[next];
        g_work_page_order[next] = tmp;
        g_settings_page_order_selection = next;
        if (save_work_page_order()) {
            g_active_work_page = first_enabled_work_page();
            set_settings_feedback(kSettingsOrderSavedFeedback, kSettingsFeedbackSavedMs);
        } else {
            set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
        }
        return;
    }
    if (!g_settings_focus_secondary) {
        g_settings_focus_secondary = true;
        g_settings_selection = 0;
        reset_settings_confirmation();
        g_settings_feedback[0] = '\0';
        return;
    }
    if (is_settings_sync_busy()) {
        set_settings_feedback(kSettingsSyncBusyFeedback, kSettingsFeedbackBusyMs);
        return;
    }
    if (!(primary == kSettingsPrimarySystem && selected == kSystemSettingsFactoryResetItem)) {
        g_factory_reset_confirm_pending = false;
    }
    if (!(primary == kSettingsPrimarySystem && selected == kSystemSettingsOfflineItem)) {
        g_offline_disable_confirm_pending = false;
    }
    if (!(primary == kSettingsPrimaryNetwork && selected == kNetworkSettingsWeatherCityItem)) {
        g_weather_city_clear_confirm_pending = false;
    }
    if (primary == kSettingsPrimaryNetwork) {
        if (selected == kNetworkSettingsWeatherCityItem) {
            if (!g_has_manual_weather_city) {
                set_settings_feedback(kManualWeatherCityEditFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            if (!g_weather_city_clear_confirm_pending) {
                g_weather_city_clear_confirm_pending = true;
                set_settings_feedback(kManualWeatherCityClearConfirmFeedback, kSettingsTimeoutMs);
                return;
            }
            if (!clear_manual_weather_city()) {
                set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            g_weather_city_clear_confirm_pending = false;
            if (g_offline_mode_ui_enabled) {
                set_settings_feedback(kManualWeatherCityAutoFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            begin_settings_sync(kSettingsSyncWeather, kManualWeatherSyncFeedback);
            ESP_LOGI(TAG, "manual weather city cleared, requesting weather sync");
            xEventGroupSetBits(g_app_events, kManualWeatherSyncBit);
            return;
        }
        if (g_offline_mode_ui_enabled) {
            set_settings_feedback(kSettingsOfflineEnabledFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        if (selected == kNetworkSettingsNtpItem) {
            begin_settings_sync(kSettingsSyncNtp, kManualNtpSyncFeedback);
            ESP_LOGI(TAG, "manual ntp sync requested");
            xEventGroupSetBits(g_app_events, kManualNtpSyncBit);
        } else if (selected == kNetworkSettingsWeatherItem) {
            begin_settings_sync(kSettingsSyncWeather, kManualWeatherSyncFeedback);
            ESP_LOGI(TAG, "manual weather sync requested");
            xEventGroupSetBits(g_app_events, kManualWeatherSyncBit);
        } else if (selected == kNetworkSettingsSayingItem) {
            begin_settings_sync(kSettingsSyncSaying, kManualSayingSyncFeedback);
            ESP_LOGI(TAG, "manual daily saying sync requested");
            xEventGroupSetBits(g_app_events, kManualSayingSyncBit);
        }
        return;
    }
    if (primary == kSettingsPrimarySound) {
        if (selected == kSoundSettingsVolumeItem) {
            int previous = g_chime_volume_percent;
            int next = kDefaultChimeVolumePercent;
            for (int i = 0; i < kChimeVolumeLevelCount; ++i) {
                if (g_chime_volume_percent == kChimeVolumeLevels[i]) {
                    next = kChimeVolumeLevels[(i + 1) % kChimeVolumeLevelCount];
                    break;
                }
            }
            g_chime_volume_percent = next;
            if (!save_hourly_chime_setting()) {
                g_chime_volume_percent = previous;
                set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            char feedback[kSettingsFeedbackTextSize];
            snprintf(feedback, sizeof(feedback), kSoundVolumeFeedbackFormat, g_chime_volume_percent);
            set_settings_feedback(feedback, kSettingsFeedbackDefaultMs);
            request_settings_confirmation_chime();
        } else if (selected == kSoundSettingsSoundItem) {
            int previous = g_chime_sound_index;
            g_chime_sound_index = (g_chime_sound_index + 1) % kChimeSoundCount;
            if (!save_hourly_chime_setting()) {
                g_chime_sound_index = previous;
                set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            char feedback[kSettingsFeedbackTextSize];
            snprintf(feedback, sizeof(feedback), kSoundIndexFeedbackFormat, g_chime_sound_index + 1);
            set_settings_feedback(feedback, kSettingsFeedbackDefaultMs);
            request_settings_confirmation_chime();
        } else if (selected == kSoundSettingsHourlyItem) {
            bool previous = g_hourly_chime_enabled;
            g_hourly_chime_enabled = !g_hourly_chime_enabled;
            if (!save_hourly_chime_setting()) {
                g_hourly_chime_enabled = previous;
                set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            set_settings_feedback(g_hourly_chime_enabled ? kHourlyChimeEnabledFeedback : kHourlyChimeDisabledFeedback, kSettingsFeedbackDefaultMs);
            ESP_LOGI(TAG, "hourly chime %s", g_hourly_chime_enabled ? "enabled" : "disabled");
            if (g_hourly_chime_enabled) {
                request_settings_confirmation_chime();
            }
        } else if (selected == kSoundSettingsAllDayItem) {
            bool previous = g_hourly_chime_all_day;
            g_hourly_chime_all_day = !g_hourly_chime_all_day;
            if (!save_hourly_chime_setting()) {
                g_hourly_chime_all_day = previous;
                set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            set_settings_feedback(g_hourly_chime_all_day ? kAllDayChimeEnabledFeedback : kAllDayChimeDisabledFeedback, kSettingsFeedbackDefaultMs);
            ESP_LOGI(TAG, "hourly chime all-day %s", g_hourly_chime_all_day ? "enabled" : "disabled");
            if (g_hourly_chime_all_day) {
                request_settings_confirmation_chime();
            }
        }
        return;
    }
    if (primary == kSettingsPrimaryDisplay) {
        if (selected == kDisplaySettingsOrderItem) {
            g_settings_page_order_mode = true;
            g_settings_page_order_selection = 0;
            normalize_work_page_order();
            set_settings_feedback(kPageOrderInstructionFeedback, kSettingsFeedbackInstructionMs);
            return;
        }
        int page = display_settings_item_work_page(selected);
        if (page == kWorkPageWeatherClock) {
            g_work_page_enabled_mask |= (1U << kWorkPageWeatherClock);
            set_settings_feedback(kWeatherClockRequiredFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        uint8_t previous = g_work_page_enabled_mask;
        g_work_page_enabled_mask ^= (1U << page);
        g_work_page_enabled_mask |= (1U << kWorkPageWeatherClock);
        if (!save_work_page_settings()) {
            g_work_page_enabled_mask = previous;
            set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
            return;
        }
        ensure_active_work_page_enabled();
        char feedback[kSettingsFeedbackTextSize];
        snprintf(feedback, sizeof(feedback), kWorkPageFeedbackFormat,
                 work_page_name(page),
                 is_work_page_enabled(page) ? kWorkPageEnabledSuffix : kWorkPageDisabledSuffix);
        set_settings_feedback(feedback, kSettingsFeedbackDefaultMs);
        return;
    }
    if (primary == kSettingsPrimarySystem) {
        if (selected == kSystemSettingsOfflineItem) {
            if (!g_offline_mode_ui_enabled) {
                if (!set_offline_mode_enabled(true)) {
                    set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
                    return;
                }
                g_offline_disable_confirm_pending = false;
                set_settings_feedback(kSettingsOfflineEnabledFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            if (can_leave_offline_mode_without_setup()) {
                if (!set_offline_mode_enabled(false)) {
                    set_settings_feedback(kSettingsSaveFailedFeedback, kSettingsFeedbackDefaultMs);
                    return;
                }
                g_offline_disable_confirm_pending = false;
                set_settings_feedback(kSettingsOfflineDisabledFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            if (!g_offline_disable_confirm_pending) {
                g_offline_disable_confirm_pending = true;
                set_settings_feedback(kOfflineSetupConfirmFeedback, kSettingsTimeoutMs);
                return;
            }
            if (!start_wifi_radio(true)) {
                set_settings_feedback(kSetupStartFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            g_offline_disable_confirm_pending = false;
            set_settings_feedback(kOfflineSetupInstructionFeedback, kSettingsFeedbackInstructionMs);
        } else if (selected == kSystemSettingsNetworkDiagItem) {
            if (g_offline_mode_ui_enabled) {
                set_settings_feedback(kSettingsOfflineEnabledFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            begin_settings_sync(kSettingsSyncNetworkDiag, kNetworkDiagSyncFeedback);
            ESP_LOGI(TAG, "manual network diagnostics requested");
            network_diag_reset();
            g_settings_requested = false;
            g_network_diag_page_requested = true;
            g_settings_focus_secondary = true;
            g_settings_primary_selection = kSettingsPrimarySystem;
            g_settings_selection = 0;
            g_info_page_until_tick = 0;
            xEventGroupSetBits(g_app_events, kNetworkDiagBit);
        } else if (selected == kSystemSettingsFactoryResetItem) {
            if (!g_factory_reset_confirm_pending) {
                g_factory_reset_confirm_pending = true;
                set_settings_feedback(kFactoryResetConfirmFeedback, kSettingsTimeoutMs);
                ESP_LOGW(TAG, "factory reset confirmation requested");
                return;
            }
            ESP_LOGW(TAG, "factory reset requested from settings");
            if (!clear_saved_config()) {
                set_settings_feedback(kFactoryResetFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            if (!start_wifi_radio(true)) {
                set_settings_feedback(kSetupStartFailedFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            g_settings_requested = false;
            g_settings_page_order_mode = false;
            g_factory_reset_confirm_pending = false;
            g_offline_disable_confirm_pending = false;
        } else if (selected == kSystemSettingsInfoItem) {
            g_settings_requested = false;
            g_settings_page_order_mode = false;
            g_factory_reset_confirm_pending = false;
            g_boot_info_requested = true;
            g_info_page_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(kSettingsTimeoutMs);
            ESP_LOGI(TAG, "system info requested from settings");
        } else if (selected == kSystemSettingsOtaItem) {
            if (g_offline_mode_ui_enabled) {
                set_settings_feedback(kSettingsOfflineEnabledFeedback, kSettingsFeedbackDefaultMs);
                return;
            }
            ota_handle_info_key();
        }
    }
}
