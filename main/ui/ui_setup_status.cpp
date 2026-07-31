// 构建并刷新配网模式下显示的 AP、门户和 STA 状态行。
#include "ui_setup_status.h"

#include "app_constexpr.h"
#include "app_state.h"
#include "network_credentials_state.h"
#include "ui_text_format.h"
#include "ui_views.h"
#include "wifi_portal_state.h"

namespace {
#define SETUP_STATUS_LABEL_CREATE_FAILED_FORMAT "setup status label create failed index=%d"
#define SETUP_STATUS_LINE_INDEX_OUT_OF_RANGE_FORMAT "setup status line index out of range: %u"

constexpr int kSetupStatusLabelX = 26;
constexpr int kSetupStatusLabelWidth = 348;
constexpr int kSetupStatusLabelHeight = 18;
constexpr int kSetupStatusLabelY[] = {194, 212, 230, 248, 266, 284};
constexpr const char *kSetupStatusInitialText[] = {
    "Setup Mode",
    "AP SSID: --",
    "AP Password: --",
    "Portal IP: --",
    "STA SSID: --",
    "STA IP: --",
};
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
constexpr size_t kSetupStatusTitleIndex = 0;
constexpr size_t kSetupStatusApSsidIndex = 1;
constexpr size_t kSetupStatusApPasswordIndex = 2;
constexpr size_t kSetupStatusPortalIpIndex = 3;
constexpr size_t kSetupStatusStaSsidIndex = 4;
constexpr size_t kSetupStatusStaIpIndex = 5;
constexpr const char *kSetupStatusFixedTexts[] = {
    kSetupStatusTitle,
    kSetupStatusPlaceholder,
    kSetupApSsidFormat,
    kSetupApPasswordFormat,
    kSetupPortalIpFormat,
    kSetupStaSsidFormat,
    kSetupStaIpFormat,
    kSetupStaIpReasonFormat,
    kSetupStaIpPlaceholder,
};
constexpr const char *kSetupStatusLogTexts[] = {
    SETUP_STATUS_LABEL_CREATE_FAILED_FORMAT,
    SETUP_STATUS_LINE_INDEX_OUT_OF_RANGE_FORMAT,
};

template <typename... Args>
bool set_setup_status_line(size_t index, const char *fallback, const char *format, Args... args)
{
    if (index >= array_count(g_setup_status_labels)) {
        ESP_LOGW(TAG, SETUP_STATUS_LINE_INDEX_OUT_OF_RANGE_FORMAT, (unsigned)index);
        return false;
    }
    char line[kSetupStatusLineSize] = {};
    ui_text::format_or_fallback(line, sizeof(line), fallback, format, args...);
    return set_label_text_if_changed(g_setup_status_labels[index], line);
}

static_assert(array_count(kSetupStatusLabelY) == array_count(kSetupStatusInitialText),
              "setup status coordinates and text must stay in sync");
static_assert(array_count(kSetupStatusLabelY) == array_count(g_setup_status_labels),
              "setup status label storage must match the rendered row count");
static_assert(kSetupStatusStaIpIndex < array_count(g_setup_status_labels),
              "setup status semantic indices must fit label storage");
static_assert(cstr_array_nonempty(kSetupStatusInitialText), "setup status initial texts must be non-empty");
static_assert(cstr_array_nonempty(kSetupStatusFixedTexts), "setup status fixed texts must be non-empty");
static_assert(cstr_array_nonempty(kSetupStatusLogTexts), "setup status log texts must be non-empty");
} // namespace

void build_setup_status_panel(lv_obj_t *parent)
{
    for (size_t i = 0; i < array_count(kSetupStatusLabelY); ++i) {
        g_setup_status_labels[i] = make_label_with_font(parent,
                                                        kSetupStatusLabelX,
                                                        kSetupStatusLabelY[i],
                                                        kSetupStatusLabelWidth,
                                                        kSetupStatusLabelHeight,
                                                        kSetupStatusInitialText[i],
                                                        &lv_font_montserrat_14);
        if (g_setup_status_labels[i]) {
            lv_obj_add_flag(g_setup_status_labels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGW(TAG, SETUP_STATUS_LABEL_CREATE_FAILED_FORMAT, static_cast<int>(i));
        }
    }
}

bool update_setup_status_panel()
{
    char wifi_ssid[kNetworkWifiSsidLen] = {};
    (void)network_wifi_ssid_snapshot(wifi_ssid, sizeof(wifi_ssid));
    bool changed = false;
    if (!g_setup_status_labels[kSetupStatusTitleIndex]) {
        return false;
    }
    changed |= set_label_text_if_changed(g_setup_status_labels[kSetupStatusTitleIndex], kSetupStatusTitle);
    changed |= set_setup_status_line(kSetupStatusApSsidIndex,
                                     kSetupStatusPlaceholder,
                                     kSetupApSsidFormat,
                                     g_ap_ssid[0] ? g_ap_ssid : kSetupStatusPlaceholder);
    changed |= set_setup_status_line(kSetupStatusApPasswordIndex,
                                     kSetupStatusPlaceholder,
                                     kSetupApPasswordFormat,
                                     kSetupApPassword);
    changed |= set_setup_status_line(kSetupStatusPortalIpIndex,
                                     kSetupStatusPlaceholder,
                                     kSetupPortalIpFormat,
                                     kSetupPortalIp);
    changed |= set_setup_status_line(kSetupStatusStaSsidIndex,
                                     kSetupStatusPlaceholder,
                                     kSetupStaSsidFormat,
                                     wifi_ssid[0] ? wifi_ssid : kSetupStatusPlaceholder);
    char station_ip[kWifiStationIpTextLen] = {};
    const bool have_station_ip = wifi_station_ip_snapshot(station_ip, sizeof(station_ip));
    const int disconnect_reason = wifi_last_disconnect_reason();
    if (have_station_ip) {
        changed |= set_setup_status_line(
            kSetupStatusStaIpIndex, kSetupStaIpPlaceholder, kSetupStaIpFormat, station_ip);
    } else if (disconnect_reason) {
        changed |= set_setup_status_line(kSetupStatusStaIpIndex,
                                         kSetupStaIpPlaceholder,
                                         kSetupStaIpReasonFormat,
                                         disconnect_reason);
    } else {
        char line[kSetupStatusLineSize] = {};
        ui_text::copy(line, sizeof(line), kSetupStaIpPlaceholder);
        changed |= set_label_text_if_changed(g_setup_status_labels[kSetupStatusStaIpIndex], line);
    }
    return changed;
}
