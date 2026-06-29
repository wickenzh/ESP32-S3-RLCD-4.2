// 构建和刷新当月日历页面及其农历节日显示。
#include "ui_views.h"

#include "calendar_lunar.h"
#include "sensor_services.h"

static constexpr int kCalendarCanvasW = 364;
static constexpr int kCalendarCanvasH = 228;
static constexpr int kCalendarCanvasX = 18;
static constexpr int kCalendarCanvasY = 62;
static constexpr int kCalendarWeekdayCount = 7;
static constexpr int kCalendarVisibleRowCount = 5;
static constexpr int kCalendarVisibleCellLimit = kCalendarWeekdayCount * kCalendarVisibleRowCount;
static constexpr int kCalendarSundayColumn = 0;
static constexpr int kCalendarSaturdayColumn = kCalendarWeekdayCount - 1;
static constexpr int kCalendarHeaderY = 2;
static constexpr int kCalendarHeaderH = 18;
static constexpr int kCalendarCellY = 29;
static constexpr int kCalendarCellW = 52;
static constexpr int kCalendarCellH = 41;
static constexpr int kCalendarGridX = 0;

static void canvas_fill_rect_safe(lv_obj_t *canvas, int w, int h, int x, int y, int rw, int rh, lv_color_t color)
{
    for (int yy = y; yy < y + rh; ++yy) {
        for (int xx = x; xx < x + rw; ++xx) {
            canvas_set_px_safe(canvas, xx, yy, w, h, color);
        }
    }
}

static void canvas_dot_rect(lv_obj_t *canvas, int w, int h, int x, int y, int rw, int rh)
{
    for (int yy = y; yy < y + rh; yy += 3) {
        for (int xx = x; xx < x + rw; xx += 4) {
            canvas_set_px_safe(canvas, xx, yy, w, h, lv_color_black());
        }
    }
    int right = x + rw - 1;
    int bottom = y + rh - 1;
    for (int yy = y; yy <= bottom; yy += 3) {
        canvas_set_px_safe(canvas, right, yy, w, h, lv_color_black());
    }
    for (int xx = x; xx <= right; xx += 4) {
        canvas_set_px_safe(canvas, xx, bottom, w, h, lv_color_black());
    }
    canvas_set_px_safe(canvas, right, bottom, w, h, lv_color_black());
}

static void canvas_fill_round_rect_safe(lv_obj_t *canvas, int w, int h, int x, int y, int rw, int rh, int radius, lv_color_t color)
{
    int r2 = radius * radius;
    for (int yy = 0; yy < rh; ++yy) {
        for (int xx = 0; xx < rw; ++xx) {
            int dx = 0;
            if (xx < radius) {
                dx = radius - xx;
            } else if (xx >= rw - radius) {
                dx = xx - (rw - radius - 1);
            }
            int dy = 0;
            if (yy < radius) {
                dy = radius - yy;
            } else if (yy >= rh - radius) {
                dy = yy - (rh - radius - 1);
            }
            if (dx == 0 || dy == 0 || dx * dx + dy * dy <= r2) {
                canvas_set_px_safe(canvas, x + xx, y + yy, w, h, color);
            }
        }
    }
}

static void draw_calendar_text(lv_obj_t *canvas,
                               const char *text,
                               int x,
                               int y,
                               int w,
                               int h,
                               const lv_font_t *font,
                               lv_color_t color,
                               lv_text_align_t align)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = color;
    dsc.font = font;
    dsc.align = align;
    lv_area_t area = {
        (lv_coord_t)x,
        (lv_coord_t)y,
        (lv_coord_t)(x + w - 1),
        (lv_coord_t)(y + h - 1),
    };
    lv_canvas_draw_text(canvas, x, y, w, &dsc, text);
    lv_obj_invalidate_area(canvas, &area);
}

static void draw_calendar_grid(const struct tm &local)
{
    if (!g_calendar_canvas) {
        return;
    }
    lv_canvas_fill_bg(g_calendar_canvas, lv_color_white(), LV_OPA_COVER);

    static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    static_assert(sizeof(weekdays) / sizeof(weekdays[0]) == kCalendarWeekdayCount);

    canvas_fill_rect_safe(g_calendar_canvas,
                          kCalendarCanvasW,
                          kCalendarCanvasH,
                          kCalendarGridX,
                          kCalendarHeaderY,
                          kCalendarCellW,
                          kCalendarHeaderH,
                          lv_color_black());
    canvas_fill_rect_safe(g_calendar_canvas,
                          kCalendarCanvasW,
                          kCalendarCanvasH,
                          kCalendarGridX + kCalendarCellW * kCalendarSaturdayColumn,
                          kCalendarHeaderY,
                          kCalendarCellW,
                          kCalendarHeaderH,
                          lv_color_black());
    canvas_dot_rect(g_calendar_canvas,
                    kCalendarCanvasW,
                    kCalendarCanvasH,
                    kCalendarGridX + kCalendarCellW,
                    kCalendarHeaderY,
                    kCalendarCellW * (kCalendarWeekdayCount - 2),
                    kCalendarHeaderH);

    for (int col = 0; col < kCalendarWeekdayCount; ++col) {
        int x = kCalendarGridX + col * kCalendarCellW;
        if (col == kCalendarSundayColumn || col == kCalendarSaturdayColumn) {
            draw_calendar_text(g_calendar_canvas, weekdays[col], x, kCalendarHeaderY, kCalendarCellW, kCalendarHeaderH, &zh_font_16, lv_color_white(), LV_TEXT_ALIGN_CENTER);
        } else {
            draw_calendar_text(g_calendar_canvas, weekdays[col], x, kCalendarHeaderY, kCalendarCellW, kCalendarHeaderH, &zh_font_16, lv_color_black(), LV_TEXT_ALIGN_CENTER);
        }
    }

    int year = local.tm_year + 1900;
    int month = local.tm_mon + 1;
    int today = local.tm_mday;
    int first_weekday = calendar_first_weekday(year, month);
    int days = calendar_days_in_month(year, month);
    int today_index = first_weekday + today - 1;
    int today_row = today_index / kCalendarWeekdayCount;
    bool shift_past_first_row = first_weekday + days > kCalendarVisibleCellLimit && today_row >= kCalendarVisibleRowCount;
    int row_offset = shift_past_first_row ? 1 : 0;
    for (int day = 1; day <= days; ++day) {
        int index = first_weekday + day - 1;
        int row = index / kCalendarWeekdayCount;
        int col = index % kCalendarWeekdayCount;
        int display_row = row - row_offset;
        if (display_row < 0 || display_row >= kCalendarVisibleRowCount) {
            continue;
        }
        int x = kCalendarGridX + col * kCalendarCellW;
        int y = kCalendarCellY + display_row * kCalendarCellH;
        bool is_today = day == today;

        if (is_today) {
            canvas_fill_round_rect_safe(g_calendar_canvas, kCalendarCanvasW, kCalendarCanvasH, x + 4, y + 3, kCalendarCellW - 8, kCalendarCellH - 5, 5, lv_color_black());
        }

        char day_text[4];
        if (day < 10) {
            day_text[0] = (char)('0' + day);
            day_text[1] = '\0';
        } else {
            day_text[0] = (char)('0' + day / 10);
            day_text[1] = (char)('0' + day % 10);
            day_text[2] = '\0';
        }
        draw_calendar_text(g_calendar_canvas,
                           day_text,
                           x + 2,
                           y + 2,
                           kCalendarCellW - 4,
                           14,
                           &lv_font_montserrat_16,
                           is_today ? lv_color_white() : lv_color_black(),
                           LV_TEXT_ALIGN_CENTER);

        struct tm day_tm = local;
        day_tm.tm_mday = day;
        day_tm.tm_hour = 12;
        day_tm.tm_min = 0;
        day_tm.tm_sec = 0;
        mktime(&day_tm);
        CalendarDayInfo info;
        calendar_day_info(day_tm, &info);
        draw_calendar_text(g_calendar_canvas,
                           info.subtext,
                           x + 2,
                           y + 20,
                           kCalendarCellW - 4,
                           12,
                           &zh_font_16,
                           is_today ? lv_color_white() : lv_color_black(),
                           LV_TEXT_ALIGN_CENTER);
    }
    lv_obj_invalidate(g_calendar_canvas);
}

bool update_calendar_page(const struct tm &local)
{
    build_calendar_page();
    bool changed = false;
    int month_key = (local.tm_year + 1900) * 12 + local.tm_mon;
    if (month_key != g_last_calendar_drawn_month || local.tm_mday != g_last_calendar_drawn_day) {
        g_last_calendar_drawn_month = month_key;
        g_last_calendar_drawn_day = local.tm_mday;
        draw_calendar_grid(local);
        changed = true;
    }
    changed |= update_work_page_status_time(g_calendar_status_time_label, local);
    changed |= update_work_page_sensor_summary(g_calendar_summary_label);
    return changed;
}

void build_calendar_page()
{
    if (g_calendar_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    g_calendar_root = screen;

    build_battery_icon(screen, g_calendar_battery_segments);
    build_work_page_status_bar(screen, 3, &g_calendar_date_label, &g_calendar_summary_label, &g_calendar_status_time_label, true);

    lv_obj_t *top_line = make_bar(screen, 18, 54, 364, 4);
    set_obj_black(top_line, true);

    if (!g_calendar_canvas_buf) {
        g_calendar_canvas_buf = alloc_canvas_buffer(kCalendarCanvasW, kCalendarCanvasH);
    }
    g_calendar_canvas = lv_canvas_create(screen);
    if (!g_calendar_canvas) {
        ESP_LOGW(TAG, "calendar canvas create failed");
    } else {
        lv_obj_clear_flag(g_calendar_canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(g_calendar_canvas, kCalendarCanvasX, kCalendarCanvasY);
        lv_obj_set_size(g_calendar_canvas, kCalendarCanvasW, kCalendarCanvasH);
        lv_obj_set_style_border_width(g_calendar_canvas, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(g_calendar_canvas, 0, LV_PART_MAIN);
        if (g_calendar_canvas_buf) {
            lv_canvas_set_buffer(g_calendar_canvas,
                                 g_calendar_canvas_buf,
                                 kCalendarCanvasW,
                                 kCalendarCanvasH,
                                 LV_IMG_CF_TRUE_COLOR);
            lv_canvas_fill_bg(g_calendar_canvas, lv_color_white(), LV_OPA_COVER);
        }
    }

    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    update_battery_segments(g_calendar_battery_segments, g_battery_percent);
}
