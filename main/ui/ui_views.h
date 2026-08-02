// 声明 UI 构建、刷新、绘图和设置页交互的公共接口。
#pragma once
#include "app_state.h"
#include "ui_bitmap.h"
#include "ui_dseg_clock.h"
#include "ui_history_chart.h"
#include "ui_inverted_clock_card.h"
#include "ui_object_refs.h"
#include "ui_progress.h"
#include "ui_settings_feedback.h"
#include "ui_settings_navigation.h"
#include "ui_status_gif.h"
#include "ui_task_notify.h"
#include "ui_time_format.h"
#include "ui_widgets.h"
#include "ui_work_page_catalog.h"
#include "wifi_portal_state.h"
#include "wifi_radio_state.h"

inline bool wifi_radio_on_for_status_icon()
{
    return wifi_radio_on_load();
}
constexpr const char *kClockWeatherCityPlaceholder = "--";
constexpr const char *kClockWeatherInfoWaitingText = "等待数据";
constexpr const char *kClockWeatherInfoSyncingText = "天气同步中";
constexpr const char *kClockWeatherInfoMissingApiKeyText = "设置 API Key";
constexpr const char *kClockWeatherTempPlaceholder = "--℃";
constexpr const char *kClockWeatherHumidityPlaceholder = "--%";
constexpr const char *kClockWeatherUnknownIconCode = "999";
lv_color_t *alloc_canvas_buffer(int width, int height);
bool ensure_canvas_buffer(lv_color_t **buffer, int width, int height);
void configure_canvas_base(lv_obj_t *canvas,
                           lv_color_t *buffer,
                           int x,
                           int y,
                           int width,
                           int height);
void build_work_page_status_bar(lv_obj_t *screen,
                                int page,
                                lv_obj_t **date_label,
                                lv_obj_t **summary_label,
                                lv_obj_t **time_label,
                                bool show_time);
struct WorkPageStatusLabels {
    lv_obj_t *date;
    lv_obj_t *summary;
    lv_obj_t *time;
};
WorkPageStatusLabels get_work_page_status_labels(int page);
bool update_work_page_status_time(lv_obj_t *label, const struct tm &local);
bool update_work_page_sensor_summary(lv_obj_t *label);
bool update_non_clock_work_page_sensor_status(int page);
bool update_weather_clock_sensor_status();
void style_work_page_sensor_summary(lv_obj_t *label);
bool update_work_page_status_icons(int page);
lv_obj_t *create_page_root();
void set_page_visible(lv_obj_t *page, bool visible);
void show_page(lv_obj_t *page);
lv_obj_t *active_work_page_root();
void show_active_work_page();
void remember_lower_panel_object(lv_obj_t *obj);
void set_lower_panel_visible(bool visible);
void set_setup_panel_visible(bool visible);
bool set_obj_visible(lv_obj_t *obj, bool visible);
bool update_low_battery_state();
void apply_clock_mode_visibility(bool setup_active);
void update_alert_pill(bool show, int alert_index = 0);
bool update_top_status_icons(bool alert_visible);
void prepare_boot_animation();
void request_boot_animation_stop();
void boot_anim_task(void *);
void finish_boot_anim_to_last_frame();
void show_boot_screen();
void update_boot_screen(int percent, const char *status, const char *detail);
void finish_boot_screen();
void build_boot_info_page();
void update_boot_info_page();
void build_network_diag_page();
bool update_network_diag_page();
void style_settings_item(lv_obj_t *label, bool selected);
void build_settings_page();
bool update_settings_page();
int clamp_int(int value, int min_value, int max_value);
void invalidate_canvas_rect(lv_obj_t *canvas, int x1, int y1, int x2, int y2);
void canvas_set_px_safe(lv_obj_t *canvas, int x, int y, int w, int h, lv_color_t color);
void canvas_draw_line(lv_obj_t *canvas, int w, int h, int x0, int y0, int x1, int y1, lv_color_t color);
void canvas_draw_dashed_hline(lv_obj_t *canvas, int w, int h, int x1, int x2, int y, lv_color_t color);
void canvas_draw_filled_circle(lv_obj_t *canvas, int w, int h, int cx, int cy, int radius, lv_color_t color);
void format_axis_hour(time_t value, char *out, size_t out_len);
int value_to_plot_y(float value, float min_value, float max_value, int y, int h);
bool collect_history_window(time_t end_hour, HourlySensorSample *out, int *out_count);
bool update_history_page(const struct tm &local);
void build_history_page();
bool update_gallery_page(const struct tm &local);
void build_gallery_page();
void gallery_set_music_mode(bool music_mode);
bool update_calendar_page(const struct tm &local);
void build_calendar_page();
bool update_weather_board_page(const struct tm &local);
void build_weather_board_page();
void build_radio_page();
bool update_radio_page(const struct tm &local);
bool update_xiaozhi_page(const struct tm &local);
uint32_t xiaozhi_subtitle_animation_delay_ms();
void build_xiaozhi_page();
void build_clock_ui();
bool update_time_ui(const struct tm &local, bool clock_page_active, int active_work_page);
void handle_settings_action();
void ui_task(void *);
