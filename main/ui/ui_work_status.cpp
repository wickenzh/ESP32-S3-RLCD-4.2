// 统一构建和刷新非天气时钟工作页顶部状态栏。
#include "ui_views.h"

namespace {

static constexpr int kStatusDateX = 198;
static constexpr int kStatusDateY = 15;
static constexpr int kStatusDateW = 182;
static constexpr int kStatusDateH = 26;
static constexpr int kStatusSummaryX = 210;
static constexpr int kStatusSummaryY = 36;
static constexpr int kStatusSummaryW = 98;
static constexpr int kStatusSummaryH = 18;
static constexpr int kStatusTimeX = 318;
static constexpr int kStatusTimeY = 36;
static constexpr int kStatusTimeW = 60;
static constexpr int kStatusTimeH = 18;
static constexpr int kStatusChimeX = 64;
static constexpr int kStatusWifiX = 90;
static constexpr int kStatusIconY = 15;
static constexpr int kStatusFirstWorkPage = kWorkPageWeatherClock;
static constexpr const char *kStatusDatePlaceholder = "----/--/-- / 星期-";
static constexpr const char *kStatusSummaryPlaceholder = "--C --%";
static constexpr const char *kStatusTimePlaceholder = "--:--";
static constexpr size_t kStatusTimeTextSize = 8;
static constexpr const char *kStatusTimeFormat = "%02d:%02d";

bool is_status_icon_page(int page)
{
    return page >= kStatusFirstWorkPage && page < kWorkPageCount && page != kWorkPageWeatherClock;
}

void build_status_icon(lv_obj_t *screen,
                       lv_obj_t **canvas,
                       lv_color_t **buffer,
                       int x,
                       int y,
                       int width,
                       int height,
                       int bytes_per_row,
                       const uint8_t *bits)
{
    if (!screen || !canvas || !buffer) {
        ESP_LOGW(TAG, "status icon invalid arg");
        return;
    }
    if (width <= 0 || height <= 0 || bytes_per_row <= 0 || !bits) {
        ESP_LOGW(TAG, "status icon invalid size %dx%d row=%d", width, height, bytes_per_row);
        return;
    }
    if (!*buffer) {
        *buffer = alloc_canvas_buffer(width, height);
    }
    *canvas = lv_canvas_create(screen);
    if (!*canvas) {
        ESP_LOGW(TAG, "status icon canvas create failed");
        return;
    }
    lv_obj_clear_flag(*canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(*canvas, x, y);
    lv_obj_set_size(*canvas, width, height);
    lv_obj_set_style_border_width(*canvas, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(*canvas, 0, LV_PART_MAIN);
    if (*buffer) {
        lv_canvas_set_buffer(*canvas, *buffer, width, height, LV_IMG_CF_TRUE_COLOR);
        draw_1bit_icon(*canvas, width, height, bytes_per_row, bits, lv_color_black(), lv_color_white());
    }
    lv_obj_add_flag(*canvas, LV_OBJ_FLAG_HIDDEN);
}

bool set_status_icon_visible_if_changed(lv_obj_t *icon, bool visible)
{
    if (!icon) {
        return false;
    }
    bool already_visible = !lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN);
    if (already_visible == visible) {
        return false;
    }
    set_obj_visible(icon, visible);
    return true;
}

} // namespace

void build_work_page_status_bar(lv_obj_t *screen,
                                int page,
                                lv_obj_t **date_label,
                                lv_obj_t **summary_label,
                                lv_obj_t **time_label,
                                bool show_time)
{
    if (!screen) {
        return;
    }
    if (date_label) {
        *date_label = make_label(screen, kStatusDateX, kStatusDateY, kStatusDateW, kStatusDateH, kStatusDatePlaceholder);
        if (*date_label) {
            lv_obj_set_style_text_align(*date_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        } else {
            ESP_LOGW(TAG, "work status date label create failed page=%d", page);
        }
    }
    if (summary_label) {
        *summary_label = make_label_with_font(screen,
                                              kStatusSummaryX,
                                              kStatusSummaryY,
                                              kStatusSummaryW,
                                              kStatusSummaryH,
                                              kStatusSummaryPlaceholder,
                                              &lv_font_montserrat_16);
        if (*summary_label) {
            style_work_page_sensor_summary(*summary_label);
        } else {
            ESP_LOGW(TAG, "work status summary label create failed page=%d", page);
        }
    }
    if (time_label) {
        *time_label = nullptr;
    }
    if (show_time && time_label) {
        *time_label = make_label_with_font(screen,
                                           kStatusTimeX,
                                           kStatusTimeY,
                                           kStatusTimeW,
                                           kStatusTimeH,
                                           kStatusTimePlaceholder,
                                           &lv_font_montserrat_16);
        if (*time_label) {
            lv_obj_set_style_text_align(*time_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
            lv_obj_set_style_pad_all(*time_label, 0, LV_PART_MAIN);
        } else {
            ESP_LOGW(TAG, "work status time label create failed page=%d", page);
        }
    }
    if (is_status_icon_page(page)) {
        build_status_icon(screen,
                          &g_work_status_chime_icon_canvas[page],
                          &g_work_status_chime_icon_canvas_buf[page],
                          kStatusChimeX,
                          kStatusIconY,
                          CHIME_STATUS_ICON_WIDTH,
                          CHIME_STATUS_ICON_HEIGHT,
                          CHIME_STATUS_ICON_BYTES_PER_ROW,
                          chime_status_icon_bits);
        build_status_icon(screen,
                          &g_work_status_wifi_icon_canvas[page],
                          &g_work_status_wifi_icon_canvas_buf[page],
                          kStatusWifiX,
                          kStatusIconY,
                          WIFI_STATUS_ICON_WIDTH,
                          WIFI_STATUS_ICON_HEIGHT,
                          WIFI_STATUS_ICON_BYTES_PER_ROW,
                          wifi_status_icon_bits);
    }
}

bool update_work_page_status_time(lv_obj_t *label, const struct tm &local)
{
    if (!label) {
        return false;
    }
    char text[kStatusTimeTextSize];
    snprintf(text, sizeof(text), kStatusTimeFormat, local.tm_hour, local.tm_min);
    return set_label_text_if_changed(label, text);
}

bool update_work_page_status_icons(int page)
{
    if (!is_status_icon_page(page)) {
        return false;
    }
    bool allow = !g_low_battery_mode && !g_setup_portal_active;
    bool chime_visible = allow && (g_hourly_chime_enabled || g_hourly_chime_all_day);
    bool wifi_visible = allow && g_wifi_radio_on;
    lv_obj_t *chime = g_work_status_chime_icon_canvas[page];
    lv_obj_t *wifi = g_work_status_wifi_icon_canvas[page];
    bool changed = false;
    changed |= set_status_icon_visible_if_changed(chime, chime_visible);
    changed |= set_status_icon_visible_if_changed(wifi, wifi_visible);
    return changed;
}
