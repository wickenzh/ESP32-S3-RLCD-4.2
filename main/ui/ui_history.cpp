// 构建和刷新温度历史页面及工作页顶部温湿度摘要。
#include "ui_views.h"

#include "sensor_services.h"

namespace {
constexpr int kHistoryWindowHours = 24;
constexpr int kSecondsPerHour = 3600;
constexpr int kHistoryAxisTickCount = 5;
constexpr int kHistoryAxisValueCount = 3;
constexpr int kHistoryCanvasW = 364;
constexpr int kHistoryCanvasH = 190;
constexpr int kHistoryBadgeW = 40;
constexpr int kHistoryBadgeH = 16;
constexpr int kHistoryChartCanvasX = 18;
constexpr int kHistoryChartCanvasY = 82;
constexpr int kHistoryTimeLabelW = 48;
constexpr int kHistoryTimeLabelH = 18;
constexpr int kHistoryTimeLabelY = 274;
constexpr int kHistoryTimeLabelCenterX[kHistoryAxisTickCount] = {42, 110, 178, 246, 314};
constexpr int kHistoryAxisLabelX = 332;
constexpr int kHistoryAxisLabelW = 56;
constexpr int kHistoryAxisLabelH = 18;
constexpr int kHistoryTempAxisLabelY = 84;
constexpr int kHistoryHumiAxisLabelY = 186;
constexpr int kHistoryAxisLabelRowGap = 30;
} // namespace

void style_history_value_badge(lv_obj_t *label)
{
    if (!label) {
        return;
    }
    lv_obj_set_style_bg_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(label, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_left(label, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_right(label, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(label, 0, LV_PART_MAIN);
}

void format_axis_hour(time_t value, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    struct tm local = {};
    localtime_r(&value, &local);
    snprintf(out, out_len, "%02d:00", local.tm_hour);
}

int value_to_plot_y(float value, float min_value, float max_value, int y, int h)
{
    if (h <= 0) {
        return y;
    }
    float range = max_value - min_value;
    if (range < 0.01f) {
        range = 1.0f;
    }
    float normalized = (value - min_value) / range;
    int offset = (int)(normalized * (h - 1) + 0.5f);
    return y + h - 1 - offset;
}

void set_history_badge(lv_obj_t *label,
                              const char *text,
                              int canvas_x,
                              int canvas_y,
                              int point_x,
                              int point_y,
                              int plot_x,
                              int plot_y,
                              int plot_w,
                              int plot_h)
{
    if (!label) {
        return;
    }
    if (plot_w <= 0 || plot_h <= 0) {
        set_obj_visible(label, false);
        return;
    }
    set_label_text_if_changed(label, text ? text : "");
    int x = canvas_x + point_x - kHistoryBadgeW / 2;
    int min_x = canvas_x + plot_x;
    int max_x = canvas_x + plot_x + plot_w - kHistoryBadgeW;
    int min_y = canvas_y + plot_y;
    int max_y = canvas_y + plot_y + plot_h - kHistoryBadgeH;
    int y = canvas_y + point_y - kHistoryBadgeH - 4;
    if (y < min_y) {
        y = canvas_y + point_y + 4;
    }
    x = clamp_int(x, min_x, max_x);
    y = clamp_int(y, min_y, max_y);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, kHistoryBadgeW, kHistoryBadgeH);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
}

bool collect_history_window(time_t end_hour,
                                   HourlySensorSample *out,
                                   int *out_count)
{
    if (out_count) {
        *out_count = 0;
    }
    if (!out || !out_count) {
        ESP_LOGW(TAG, "history window invalid arg");
        return false;
    }
    time_t start = end_hour - kHistoryWindowHours * kSecondsPerHour;
    int count = 0;
    for (int hour = 1; hour <= kHistoryWindowHours; ++hour) {
        time_t expected = start + hour * kSecondsPerHour;
        bool found = false;
        for (int i = 0; i < kHourlyHistoryCount; ++i) {
            const HourlySensorSample &sample = g_hourly_history.samples[i];
            if (sample.valid && sample.timestamp == expected) {
                out[count++] = sample;
                found = true;
                break;
            }
        }
        if (!found) {
            HourlySensorSample sample = {};
            sample.timestamp = expected;
            out[count++] = sample;
        }
    }
    *out_count = count;
    return count > 0;
}

void update_history_axis_labels(time_t start, time_t end)
{
    (void)end;
    static const int tick_hours[kHistoryAxisTickCount] = {0, 6, 12, 18, 24};
    for (int i = 0; i < kHistoryAxisTickCount; ++i) {
        char text[8];
        format_axis_hour(start + tick_hours[i] * kSecondsPerHour, text, sizeof(text));
        set_label_text_if_changed(g_history_time_labels[i], text);
    }
}

bool update_work_page_sensor_summary(lv_obj_t *label)
{
    if (!label) {
        return false;
    }
    char text[32];
    if (g_sensor_ok) {
        snprintf(text, sizeof(text), "%.0fC %.0f%%", g_temperature, g_humidity);
    } else {
        snprintf(text, sizeof(text), "--C --%%");
    }
    return set_label_text_if_changed(label, text);
}

void style_work_page_sensor_summary(lv_obj_t *label)
{
    if (!label) {
        return;
    }
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
}

void draw_history_chart_panel(lv_obj_t *canvas,
                                     int canvas_w,
                                     int canvas_h,
                                     const HourlySensorSample *samples,
                                     int sample_count,
                                     bool temperature,
                                     int plot_x,
                                     int plot_y,
                                     int plot_w,
                                     int plot_h,
                                     lv_obj_t *max_label,
                                     lv_obj_t *min_label,
                                     lv_obj_t **axis_labels)
{
    if (!canvas || !samples || sample_count <= 0 || !axis_labels ||
        canvas_w <= 0 || canvas_h <= 0 || plot_w <= 0 || plot_h <= 0) {
        ESP_LOGW(TAG, "history chart invalid arg");
        set_obj_visible(max_label, false);
        set_obj_visible(min_label, false);
        if (axis_labels) {
            for (int i = 0; i < kHistoryAxisValueCount; ++i) {
                set_label_text_if_changed(axis_labels[i], "--");
            }
        }
        return;
    }
    int valid_count = 0;
    float min_value = 0.0f;
    float max_value = 0.0f;
    int min_index = -1;
    int max_index = -1;
    for (int i = 0; i < sample_count; ++i) {
        if (!samples[i].valid) {
            continue;
        }
        float value = temperature ? samples[i].temperature : samples[i].humidity;
        if (valid_count == 0 || value < min_value) {
            min_value = value;
            min_index = i;
        }
        if (valid_count == 0 || value > max_value) {
            max_value = value;
            max_index = i;
        }
        ++valid_count;
    }

    for (int i = 0; i < 4; ++i) {
        int y = plot_y + (plot_h * i) / 3;
        canvas_draw_dashed_hline(canvas, canvas_w, canvas_h, plot_x, plot_x + plot_w, y, lv_color_black());
    }
    canvas_draw_line(canvas, canvas_w, canvas_h, plot_x, plot_y + plot_h, plot_x + plot_w, plot_y + plot_h, lv_color_black());

    if (valid_count < 2) {
        set_obj_visible(max_label, false);
        set_obj_visible(min_label, false);
        for (int i = 0; i < kHistoryAxisValueCount; ++i) {
            set_label_text_if_changed(axis_labels[i], "--");
        }
        return;
    }

    float pad = temperature ? 0.6f : 3.0f;
    if (max_value - min_value < (temperature ? 1.0f : 5.0f)) {
        pad += (temperature ? 0.5f : 2.0f);
    }
    float axis_min = min_value - pad;
    float axis_max = max_value + pad;
    float axis_mid = (axis_min + axis_max) * 0.5f;
    char axis_text[16];
    snprintf(axis_text, sizeof(axis_text), temperature ? "%.0f℃" : "%.0f%%", axis_max);
    set_label_text_if_changed(axis_labels[0], axis_text);
    snprintf(axis_text, sizeof(axis_text), temperature ? "%.0f℃" : "%.0f%%", axis_mid);
    set_label_text_if_changed(axis_labels[1], axis_text);
    snprintf(axis_text, sizeof(axis_text), temperature ? "%.0f℃" : "%.0f%%", axis_min);
    set_label_text_if_changed(axis_labels[2], axis_text);

    int prev_x = 0;
    int prev_y = 0;
    time_t prev_ts = 0;
    bool have_prev = false;
    time_t start = samples[0].timestamp - kSecondsPerHour;
    for (int i = 0; i < sample_count; ++i) {
        if (!samples[i].valid) {
            have_prev = false;
            continue;
        }
        int x = plot_x + (int)(((samples[i].timestamp - start) * plot_w) / (kHistoryWindowHours * kSecondsPerHour));
        float value = temperature ? samples[i].temperature : samples[i].humidity;
        int y = value_to_plot_y(value, axis_min, axis_max, plot_y, plot_h);
        x = clamp_int(x, plot_x, plot_x + plot_w);
        y = clamp_int(y, plot_y, plot_y + plot_h);
        if (have_prev && samples[i].timestamp - prev_ts <= 2 * kSecondsPerHour) {
            canvas_draw_line(canvas, canvas_w, canvas_h, prev_x, prev_y, x, y, lv_color_black());
        }
        prev_x = x;
        prev_y = y;
        prev_ts = samples[i].timestamp;
        have_prev = true;
    }

    if (max_index >= 0) {
        char text[16];
        snprintf(text, sizeof(text), temperature ? "%.1f" : "%.0f",
                 temperature ? samples[max_index].temperature : samples[max_index].humidity);
        int x = plot_x + (int)(((samples[max_index].timestamp - start) * plot_w) / (kHistoryWindowHours * kSecondsPerHour));
        int y = value_to_plot_y(temperature ? samples[max_index].temperature : samples[max_index].humidity,
                                axis_min,
                                axis_max,
                                plot_y,
                                plot_h);
        canvas_draw_filled_circle(canvas, canvas_w, canvas_h, x, y, 3, lv_color_black());
        set_history_badge(max_label, text, 18, 82, x, y, plot_x, plot_y, plot_w, plot_h);
    }
    if (min_index >= 0 && min_index != max_index) {
        char text[16];
        snprintf(text, sizeof(text), temperature ? "%.1f" : "%.0f",
                 temperature ? samples[min_index].temperature : samples[min_index].humidity);
        int x = plot_x + (int)(((samples[min_index].timestamp - start) * plot_w) / (kHistoryWindowHours * kSecondsPerHour));
        int y = value_to_plot_y(temperature ? samples[min_index].temperature : samples[min_index].humidity,
                                axis_min,
                                axis_max,
                                plot_y,
                                plot_h);
        canvas_draw_filled_circle(canvas, canvas_w, canvas_h, x, y, 3, lv_color_black());
        set_history_badge(min_label, text, 18, 82, x, y, plot_x, plot_y, plot_w, plot_h);
    } else {
        set_obj_visible(min_label, false);
    }
}

bool update_history_page(const struct tm &local)
{
    build_history_page();
    if (!g_history_chart_canvas) {
        return false;
    }
    struct tm mutable_local = local;
    time_t now = mktime(&mutable_local);
    time_t end_hour = hour_start_from_time(now);
    int hour_key = local.tm_yday * 24 + local.tm_hour;
    bool changed = update_work_page_status_time(g_history_status_time_label, local);
    changed |= update_work_page_sensor_summary(g_history_summary_label);
    if (g_last_history_drawn_version == g_hourly_history_version &&
        g_last_history_drawn_hour == hour_key) {
        return changed;
    }
    g_last_history_drawn_version = g_hourly_history_version;
    g_last_history_drawn_hour = hour_key;

    lv_canvas_fill_bg(g_history_chart_canvas, lv_color_white(), LV_OPA_COVER);

    HourlySensorSample samples[kHourlyHistoryCount] = {};
    int sample_count = 0;
    collect_history_window(end_hour, samples, &sample_count);
    time_t start = end_hour - kHistoryWindowHours * kSecondsPerHour;
    update_history_axis_labels(start, end_hour);

    draw_history_chart_panel(g_history_chart_canvas,
                             kHistoryCanvasW,
                             kHistoryCanvasH,
                             samples,
                             sample_count,
                             true,
                             34,
                             10,
                             276,
                             62,
                             g_history_temp_max_label,
                             g_history_temp_min_label,
                             g_history_temp_axis_labels);
    draw_history_chart_panel(g_history_chart_canvas,
                             kHistoryCanvasW,
                             kHistoryCanvasH,
                             samples,
                             sample_count,
                             false,
                             34,
                             112,
                             276,
                             62,
                             g_history_humi_max_label,
                             g_history_humi_min_label,
                             g_history_humi_axis_labels);
    lv_obj_invalidate(g_history_chart_canvas);
    return true;
}

void build_history_page()
{
    if (g_history_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    g_history_root = screen;

    build_battery_icon(screen, g_history_battery_segments);
    build_work_page_status_bar(screen, 1, &g_history_date_label, &g_history_summary_label, &g_history_status_time_label, true);

    lv_obj_t *history_top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(history_top_line, true);
    lv_obj_t *temp_title = make_label(screen, 24, 67, 80, 24, "温度");
    lv_obj_set_style_text_font(temp_title, &zh_font_16, LV_PART_MAIN);
    lv_obj_t *humi_title = make_label(screen, 24, 172, 80, 24, "湿度");
    lv_obj_set_style_text_font(humi_title, &zh_font_16, LV_PART_MAIN);

    if (!g_history_chart_canvas_buf) {
        g_history_chart_canvas_buf = alloc_canvas_buffer(kHistoryCanvasW, kHistoryCanvasH);
    }
    g_history_chart_canvas = lv_canvas_create(screen);
    if (!g_history_chart_canvas) {
        ESP_LOGW(TAG, "history chart canvas create failed");
    } else {
        lv_obj_clear_flag(g_history_chart_canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(g_history_chart_canvas, kHistoryChartCanvasX, kHistoryChartCanvasY);
        lv_obj_set_size(g_history_chart_canvas, kHistoryCanvasW, kHistoryCanvasH);
        lv_obj_set_style_border_width(g_history_chart_canvas, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(g_history_chart_canvas, 0, LV_PART_MAIN);
        if (g_history_chart_canvas_buf) {
            lv_canvas_set_buffer(g_history_chart_canvas, g_history_chart_canvas_buf, kHistoryCanvasW, kHistoryCanvasH, LV_IMG_CF_TRUE_COLOR);
            lv_canvas_fill_bg(g_history_chart_canvas, lv_color_white(), LV_OPA_COVER);
        }
    }
    lv_obj_move_foreground(temp_title);
    lv_obj_move_foreground(humi_title);

    for (int i = 0; i < kHistoryAxisTickCount; ++i) {
        g_history_time_labels[i] = make_label_with_font(screen,
                                                        kHistoryTimeLabelCenterX[i] - kHistoryTimeLabelW / 2,
                                                        kHistoryTimeLabelY,
                                                        kHistoryTimeLabelW,
                                                        kHistoryTimeLabelH,
                                                        "--:--",
                                                        &lv_font_montserrat_14);
        lv_obj_set_style_text_align(g_history_time_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    for (int i = 0; i < kHistoryAxisValueCount; ++i) {
        g_history_temp_axis_labels[i] = make_label(screen,
                                                   kHistoryAxisLabelX,
                                                   kHistoryTempAxisLabelY + i * kHistoryAxisLabelRowGap,
                                                   kHistoryAxisLabelW,
                                                   kHistoryAxisLabelH,
                                                   "--");
        g_history_humi_axis_labels[i] = make_label(screen,
                                                   kHistoryAxisLabelX,
                                                   kHistoryHumiAxisLabelY + i * kHistoryAxisLabelRowGap,
                                                   kHistoryAxisLabelW,
                                                   kHistoryAxisLabelH,
                                                   "--");
        lv_obj_set_style_text_align(g_history_temp_axis_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_set_style_text_align(g_history_humi_axis_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    }

    g_history_temp_max_label = make_label_with_font(screen, 0, 0, 40, 16, "--", &lv_font_montserrat_12);
    g_history_temp_min_label = make_label_with_font(screen, 0, 0, 40, 16, "--", &lv_font_montserrat_12);
    g_history_humi_max_label = make_label_with_font(screen, 0, 0, 40, 16, "--", &lv_font_montserrat_12);
    g_history_humi_min_label = make_label_with_font(screen, 0, 0, 40, 16, "--", &lv_font_montserrat_12);
    style_history_value_badge(g_history_temp_max_label);
    style_history_value_badge(g_history_temp_min_label);
    style_history_value_badge(g_history_humi_max_label);
    style_history_value_badge(g_history_humi_min_label);

    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    update_battery_segments(g_history_battery_segments, g_battery_percent);
}
