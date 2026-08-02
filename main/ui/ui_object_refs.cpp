// 集中清理工作页、辅助页对象引用和对应绘制缓存。
#include "ui_object_refs.h"

#include "app_state.h"
#include "ui_draw_cache.h"
#include "ui_progress.h"

namespace {
template <typename T, size_t N>
void clear_pointer_array(T *(&items)[N])
{
    for (T *&item : items) {
        item = nullptr;
    }
}

void clear_work_page_root_refs()
{
    g_clock_root = nullptr;
    g_history_root = nullptr;
    g_gallery_root = nullptr;
    g_calendar_root = nullptr;
    g_weather_board_root = nullptr;
    g_radio_root = nullptr;
    g_xiaozhi_root = nullptr;
}

void clear_work_status_refs()
{
    g_date_label = nullptr;
    g_history_date_label = nullptr;
    g_gallery_date_label = nullptr;
    g_calendar_date_label = nullptr;
    g_weather_board_date_label = nullptr;
    g_radio_date_label = nullptr;
    g_xiaozhi_date_label = nullptr;
    g_history_summary_label = nullptr;
    g_gallery_summary_label = nullptr;
    g_calendar_summary_label = nullptr;
    g_weather_board_summary_label = nullptr;
    g_xiaozhi_summary_label = nullptr;
    g_history_status_time_label = nullptr;
    g_calendar_status_time_label = nullptr;
    g_weather_board_status_time_label = nullptr;
    g_xiaozhi_status_time_label = nullptr;
    clear_pointer_array(g_work_status_chime_icon_canvas);
    clear_pointer_array(g_work_status_wifi_icon_canvas);
    clear_pointer_array(g_work_status_alarm_icon_canvas);
}

void clear_gallery_calendar_refs()
{
    g_gallery_image_canvas = nullptr;
    g_gallery_time_canvas = nullptr;
    g_gallery_saying_label = nullptr;
    g_calendar_canvas = nullptr;
}

void clear_weather_clock_refs()
{
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
    g_alarm_status_icon_canvas = nullptr;
    g_low_battery_icon_canvas = nullptr;
    g_panel_sep_a = nullptr;
    g_panel_sep_b = nullptr;
    g_time_canvas = nullptr;
    g_second_canvas = nullptr;
    g_status_gif_canvas = nullptr;
    g_second_progress_canvas = nullptr;
}

void clear_radio_refs()
{
    g_radio_station_label = nullptr;
    g_radio_station_bold_label = nullptr;
    g_radio_status_label = nullptr;
    g_radio_uptime_label = nullptr;
    g_radio_index_label = nullptr;
    g_radio_hint_label = nullptr;
}

void clear_xiaozhi_refs()
{
    g_xiaozhi_state_label = nullptr;
    g_xiaozhi_detail_label = nullptr;
    g_xiaozhi_wave_canvas = nullptr;
}

void clear_battery_segment_refs()
{
    clear_pointer_array(g_battery_segments);
    clear_pointer_array(g_history_battery_segments);
    clear_pointer_array(g_gallery_battery_segments);
    clear_pointer_array(g_calendar_battery_segments);
    clear_pointer_array(g_weather_board_battery_segments);
    clear_pointer_array(g_radio_battery_segments);
    clear_pointer_array(g_xiaozhi_battery_segments);
}

void clear_history_page_refs()
{
    g_history_chart_canvas = nullptr;
    g_history_temp_max_label = nullptr;
    g_history_temp_min_label = nullptr;
    g_history_humi_max_label = nullptr;
    g_history_humi_min_label = nullptr;
    clear_pointer_array(g_history_time_labels);
    clear_pointer_array(g_history_temp_axis_labels);
    clear_pointer_array(g_history_humi_axis_labels);
    clear_pointer_array(g_lower_panel_objects);
    clear_pointer_array(g_setup_status_labels);
}

void clear_ui_draw_cache_state()
{
    invalidate_clock_time_draw_cache();
    invalidate_clock_second_progress_draw_cache();
    invalidate_status_gif_draw_cache();

    invalidate_work_status_draw_cache();
    invalidate_history_draw_cache();
}

void clear_aux_page_root_refs()
{
    g_info_root = nullptr;
    g_network_diag_root = nullptr;
    g_settings_root = nullptr;
}

void clear_system_info_refs()
{
    clear_pointer_array(g_info_labels);
}

void clear_network_diag_refs()
{
    clear_pointer_array(g_network_diag_labels);
    g_network_diag_summary_label = nullptr;
    g_network_diag_hint_label = nullptr;
}

void clear_settings_page_refs()
{
    clear_pointer_array(g_settings_labels);
    clear_pointer_array(g_settings_switch_dots);
    g_settings_feedback_label = nullptr;
    g_settings_ota_status_label = nullptr;
    g_settings_ota_hint_label = nullptr;
    g_settings_ota_bar_frame = nullptr;
    g_settings_ota_bar_fill = nullptr;
}
}

void clear_clock_object_refs()
{
    clear_work_page_root_refs();
    clear_work_status_refs();
    clear_gallery_calendar_refs();
    clear_weather_clock_refs();
    clear_radio_refs();
    clear_xiaozhi_refs();
    clear_work_page_day_progress_refs();
    clear_battery_segment_refs();
    clear_history_page_refs();
    clear_ui_draw_cache_state();
}

void clear_info_object_refs()
{
    clear_aux_page_root_refs();
    clear_system_info_refs();
    clear_network_diag_refs();
    clear_settings_page_refs();
}
