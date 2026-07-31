// 构建并刷新 System Info 与网络检测两个独立辅助页面。
#include "ui_views.h"

#include "app_constexpr.h"
#include "network_credentials_state.h"
#include "network_diagnostics_catalog.h"
#include "network_diagnostics_state.h"
#include "network_services.h"
#include "ota_services.h"
#include "ui_text_format.h"

namespace {
constexpr int kNetworkDiagGridFirstLine = kNetworkDiagIpLocationLine;
constexpr int kNetworkDiagWideLine = kNetworkDiagOtaLine;
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
constexpr int kAuxPageTitleX = 24;
constexpr int kAuxPageTitleY = 18;
constexpr int kAuxPageTitleW = 352;
constexpr int kInfoPageTitleH = 26;
constexpr int kNetworkDiagTitleH = 28;
constexpr int kAuxPageLineX = 24;
constexpr int kAuxPageLineW = 352;
constexpr int kInfoPageTopLineY = 50;
constexpr int kInfoPageTopLineH = 3;
constexpr int kInfoPageBottomLineY = 238;
constexpr int kInfoPageBottomLineH = 3;
constexpr int kInfoReturnHintX = 24;
constexpr int kInfoReturnHintY = 252;
constexpr int kInfoReturnHintW = 352;
constexpr int kInfoReturnHintH = 22;
constexpr int kNetworkDiagTopLineY = 52;
constexpr int kNetworkDiagTopLineH = 3;
constexpr int kNetworkDiagSummaryX = 24;
constexpr int kNetworkDiagSummaryY = 62;
constexpr int kNetworkDiagSummaryW = 352;
constexpr int kNetworkDiagSummaryH = 22;
constexpr int kNetworkDiagLineH = 22;
constexpr int kNetworkDiagBottomLineY = 266;
constexpr int kNetworkDiagBottomLineH = 2;
constexpr int kNetworkDiagHintX = 24;
constexpr int kNetworkDiagHintY = 272;
constexpr int kNetworkDiagHintW = 352;
constexpr int kNetworkDiagHintH = 20;
constexpr const char *kNetworkDiagTitle = "网络检测";
constexpr const char *kNetworkDiagSummaryReady = "准备检测...";
constexpr const char *kNetworkDiagSummaryRunning = "检测中...";
constexpr const char *kNetworkDiagSummaryDone = "检测完成";
constexpr const char *kNetworkDiagSummaryIdle = "等待开始";
constexpr const char *kNetworkDiagLinePlaceholder = "--";
constexpr const char *kNetworkDiagHintIdle = "Hold KEY to return";
constexpr const char *kNetworkDiagHintRunning = "Checking... Hold KEY to return";
constexpr size_t kInfoTimeTextSize = 32;
constexpr size_t kInfoLineTextSize = 96;
constexpr const char *kInfoLastNtpFormat = "Last NTP: %s";
constexpr const char *kInfoWifiFormat = "WiFi: %s";
constexpr const char *kInfoLastWeatherFormat = "Last Weather: %s";
constexpr const char *kInfoBatteryFullFormat = "Battery: %d%%  %.2fV \\ %s";
constexpr const char *kInfoBatteryPercentOnlyFormat = "Battery: %d%%  -- \\ %s";
constexpr const char *kInfoBatteryPlaceholder = "Battery: --  -- \\ --";
constexpr const char *kInfoVersionFormat = "Version: %s / %s";
constexpr const char *kInfoSourceFormat = "Source: %s";
constexpr const char *kProjectSourceUrl = "github.com/wickenzh/ESP32-S3-RLCD-4.2";
constexpr const char *kInfoLinePlaceholder = "--";
constexpr const char *kInfoReturnHintText = "Hold KEY to return";
constexpr const char *kAuxPageTexts[] = {
    kNetworkDiagTitle,
    kNetworkDiagSummaryReady,
    kNetworkDiagSummaryRunning,
    kNetworkDiagSummaryDone,
    kNetworkDiagSummaryIdle,
    kNetworkDiagLinePlaceholder,
    kNetworkDiagHintIdle,
    kNetworkDiagHintRunning,
    kInfoLastNtpFormat,
    kInfoWifiFormat,
    kInfoLastWeatherFormat,
    kInfoBatteryFullFormat,
    kInfoBatteryPercentOnlyFormat,
    kInfoBatteryPlaceholder,
    kInfoVersionFormat,
    kInfoSourceFormat,
    kProjectSourceUrl,
    kInfoLinePlaceholder,
    kInfoReturnHintText,
};
constexpr int kInfoTextX = 30;
constexpr int kInfoTextW = 340;
constexpr int kInfoSourceTextX = 0;
constexpr int kInfoSourceTextW = 400;
constexpr int kInfoLabelY[] = {70, 104, 138, 172, 206, 276};
constexpr size_t kInfoLabelCount = array_count(kInfoLabelY);
constexpr size_t kInfoNtpLabelIndex = 0;
constexpr size_t kInfoWifiLabelIndex = 1;
constexpr size_t kInfoWeatherLabelIndex = 2;
constexpr size_t kInfoBatteryLabelIndex = 3;
constexpr size_t kInfoVersionLabelIndex = 4;
constexpr size_t kInfoSourceLabelIndex = kInfoLabelCount - 1;

#define NETWORK_DIAG_LINE_LABEL_CREATE_FAILED_FORMAT "network diag line %d label create failed"

static_assert(cstr_array_nonempty(kAuxPageTexts), "auxiliary page text registry must not be empty");
static_assert(kNetworkDiagLocalIpLine < kNetworkDiagPublicIpLine,
              "network diagnostics local IP line must precede public IP line");
static_assert(kNetworkDiagPublicIpLine < kNetworkDiagGridFirstLine,
              "network diagnostics public IP line must precede grid lines");
static_assert(kNetworkDiagGridFirstLine <= kNetworkDiagWideLine,
              "network diagnostics grid first line must not follow wide line");
static_assert(kNetworkDiagWideLine < kNetworkDiagLineCount,
              "network diagnostics wide line must fit line count");
static_assert(kNetworkDiagGridColumns > 0, "network diagnostics grid must have columns");
static_assert(kNetworkDiagWideW > 0 && kNetworkDiagGridW > 0,
              "network diagnostics line widths must be positive");
static_assert(kNetworkDiagWideW >= kNetworkDiagGridW,
              "network diagnostics wide line must fit grid line width");
static_assert(kAuxPageTitleW > 0 && kAuxPageLineW > 0, "auxiliary page frame widths must be positive");
static_assert(kInfoPageTitleH > 0 && kNetworkDiagTitleH > 0,
              "auxiliary page title heights must be positive");
static_assert(kInfoReturnHintW > 0 && kInfoReturnHintH > 0,
              "System Info return hint size must be positive");
static_assert(kNetworkDiagSummaryW > 0 && kNetworkDiagSummaryH > 0,
              "network diagnostics summary size must be positive");
static_assert(kNetworkDiagLineH > 0, "network diagnostics line height must be positive");
static_assert(kNetworkDiagHintW > 0 && kNetworkDiagHintH > 0,
              "network diagnostics hint size must be positive");
static_assert(kNetworkDiagSummaryTextSize > 1,
              "network diagnostics summary buffer must fit text and NUL");
static_assert(kInfoLabelCount == array_count(g_info_labels),
              "System Info labels and row coordinates must stay in sync");
static_assert(kInfoVersionLabelIndex < kInfoSourceLabelIndex,
              "System Info version label must precede source label");
static_assert(kInfoSourceLabelIndex < kInfoLabelCount,
              "System Info source label index must fit label count");

struct NetworkDiagLineLayout {
    int x;
    int y;
    int w;
};

NetworkDiagLineLayout network_diag_line_layout(int index)
{
    NetworkDiagLineLayout layout = {kNetworkDiagWideX, kNetworkDiagLocalIpY, kNetworkDiagWideW};
    if (index == kNetworkDiagLocalIpLine) {
        layout.y = kNetworkDiagLocalIpY;
    } else if (index == kNetworkDiagPublicIpLine) {
        layout.y = kNetworkDiagPublicIpY;
    } else {
        int grid = index - kNetworkDiagGridFirstLine;
        int row = grid / kNetworkDiagGridColumns;
        int col = grid % kNetworkDiagGridColumns;
        layout.x = kNetworkDiagWideX + col * kNetworkDiagGridColGap;
        layout.y = kNetworkDiagGridStartY + row * kNetworkDiagGridRowGap;
        layout.w = kNetworkDiagGridW;
        if (index == kNetworkDiagWideLine) {
            layout.x = kNetworkDiagWideX;
            layout.w = kNetworkDiagWideW;
        }
    }
    return layout;
}

void set_info_time_label(size_t index, const char *format, time_t value)
{
    char time_text[kInfoTimeTextSize] = {};
    char line[kInfoLineTextSize] = {};
    format_time_or_dash(value, time_text, sizeof(time_text));
    ui_text::format_or_fallback(line, sizeof(line), kInfoLinePlaceholder, format, time_text);
    set_label_text_if_changed(g_info_labels[index], line);
}

void set_info_string_label(size_t index, const char *format, const char *value)
{
    char line[kInfoLineTextSize] = {};
    ui_text::format_or_fallback(line, sizeof(line), kInfoLinePlaceholder, format, value ? value : "");
    set_label_text_if_changed(g_info_labels[index], line);
}

void set_info_battery_label()
{
    BatteryRuntimeSnapshot battery;
    battery_runtime_snapshot_load(&battery);
    char line[kInfoLineTextSize] = {};
    char charge_time[kInfoTimeTextSize] = {};
    format_time_or_dash(battery.last_charge_time, charge_time, sizeof(charge_time));
    if (battery.percent >= 0 && battery.voltage >= 0.0f) {
        ui_text::format_or_fallback(line, sizeof(line), kInfoBatteryPlaceholder, kInfoBatteryFullFormat,
                                    battery.percent, battery.voltage, charge_time);
    } else if (battery.percent >= 0) {
        ui_text::format_or_fallback(line, sizeof(line), kInfoBatteryPlaceholder, kInfoBatteryPercentOnlyFormat,
                                    battery.percent, charge_time);
    } else {
        ui_text::copy(line, sizeof(line), kInfoBatteryPlaceholder);
    }
    set_label_text_if_changed(g_info_labels[kInfoBatteryLabelIndex], line);
}

void set_info_version_label()
{
    char line[kInfoLineTextSize] = {};
    ui_text::format_or_fallback(line, sizeof(line), kInfoLinePlaceholder, kInfoVersionFormat, APP_VERSION, APP_BUILD_DATE);
    set_label_text_if_changed(g_info_labels[kInfoVersionLabelIndex], line);
}
} // namespace

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

    make_centered_label_with_font(screen, kAuxPageTitleX, kAuxPageTitleY, kAuxPageTitleW, kInfoPageTitleH,
                                  "SYSTEM INFO", &lv_font_montserrat_16, "system info title create failed");
    make_black_bar(screen, kAuxPageLineX, kInfoPageTopLineY, kAuxPageLineW, kInfoPageTopLineH);
    for (size_t i = 0; i < kInfoLabelCount; ++i) {
        const bool source_line = i == kInfoSourceLabelIndex;
        g_info_labels[i] = make_label_with_font(screen,
                                                source_line ? kInfoSourceTextX : kInfoTextX,
                                                kInfoLabelY[i],
                                                source_line ? kInfoSourceTextW : kInfoTextW,
                                                source_line ? 18 : 24,
                                                kInfoLinePlaceholder,
                                                source_line ? &lv_font_montserrat_12 : &lv_font_montserrat_14);
        if (source_line) {
            (void)center_align_label(g_info_labels[i]);
        }
    }
    make_black_bar(screen, kAuxPageLineX, kInfoPageBottomLineY, kAuxPageLineW, kInfoPageBottomLineH);
    make_centered_label_with_font(screen, kInfoReturnHintX, kInfoReturnHintY,
                                  kInfoReturnHintW, kInfoReturnHintH, kInfoReturnHintText,
                                  &lv_font_montserrat_14, "system info return label create failed");
}

void update_boot_info_page()
{
    char wifi_ssid[kNetworkWifiSsidLen] = {};
    (void)network_wifi_ssid_snapshot(wifi_ssid, sizeof(wifi_ssid));
    set_info_time_label(kInfoNtpLabelIndex, kInfoLastNtpFormat, get_last_ntp_sync_time());
    set_info_string_label(kInfoWifiLabelIndex,
                          kInfoWifiFormat,
                          wifi_ssid[0] ? wifi_ssid : "--");
    set_info_time_label(kInfoWeatherLabelIndex,
                        kInfoLastWeatherFormat,
                        get_last_weather_sync_time());
    set_info_battery_label();
    set_info_version_label();
    set_info_string_label(kInfoSourceLabelIndex, kInfoSourceFormat, kProjectSourceUrl);
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
    make_centered_label(screen, kAuxPageTitleX, kAuxPageTitleY, kAuxPageTitleW, kNetworkDiagTitleH,
                        kNetworkDiagTitle, "network diag title create failed");
    make_black_bar(screen, kAuxPageLineX, kNetworkDiagTopLineY, kAuxPageLineW, kNetworkDiagTopLineH);
    g_network_diag_summary_label = make_centered_label(screen, kNetworkDiagSummaryX, kNetworkDiagSummaryY,
                                                       kNetworkDiagSummaryW, kNetworkDiagSummaryH,
                                                       kNetworkDiagSummaryReady,
                                                       "network diag summary label create failed");
    for (int i = 0; i < kNetworkDiagLineCount; ++i) {
        NetworkDiagLineLayout layout = network_diag_line_layout(i);
        g_network_diag_labels[i] = make_label(screen, layout.x, layout.y, layout.w, kNetworkDiagLineH,
                                              kNetworkDiagLinePlaceholder);
        if (g_network_diag_labels[i]) {
            lv_label_set_long_mode(g_network_diag_labels[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(g_network_diag_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        } else {
            ESP_LOGW(TAG, NETWORK_DIAG_LINE_LABEL_CREATE_FAILED_FORMAT, i);
        }
    }
    make_black_bar(screen, kAuxPageLineX, kNetworkDiagBottomLineY, kAuxPageLineW, kNetworkDiagBottomLineH);
    g_network_diag_hint_label = make_centered_label(screen, kNetworkDiagHintX, kNetworkDiagHintY,
                                                    kNetworkDiagHintW, kNetworkDiagHintH, kNetworkDiagHintIdle,
                                                    "network diag hint label create failed");
}

bool update_network_diag_page()
{
    bool changed = false;
    NetworkDiagnosticsSnapshot snapshot;
    network_diag_snapshot_load(&snapshot);
    char summary[kNetworkDiagSummaryTextSize] = {};
    if (snapshot.state == kNetworkDiagRunning) {
        ui_text::copy(summary, sizeof(summary), kNetworkDiagSummaryRunning);
    } else if (snapshot.state == kNetworkDiagDone) {
        ui_text::copy(summary, sizeof(summary), kNetworkDiagSummaryDone);
    } else {
        ui_text::copy(summary, sizeof(summary), kNetworkDiagSummaryIdle);
    }
    changed |= set_label_text_if_changed(g_network_diag_summary_label, summary);
    for (int i = 0; i < kNetworkDiagLineCount; ++i) {
        changed |= set_label_text_if_changed(g_network_diag_labels[i],
                                             snapshot.lines[i][0] ? snapshot.lines[i] :
                                                                    kNetworkDiagLinePlaceholder);
    }
    changed |= set_label_text_if_changed(g_network_diag_hint_label,
                                         snapshot.state == kNetworkDiagRunning ? kNetworkDiagHintRunning :
                                                                                kNetworkDiagHintIdle);
    return changed;
}
