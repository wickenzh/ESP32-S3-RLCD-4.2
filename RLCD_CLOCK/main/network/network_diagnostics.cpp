// 执行设置页里的网络诊断流程，逐项显示联网链路状态。
#include "network_diagnostics_internal.h"

#include "daily_saying_service.h"
#include "ip_geolocation_client.h"

#include "app_constexpr.h"
#include "app_event_group.h"
#include "app_metadata.h"
#include "app_network_config.h"
#include "app_text_format.h"
#include "battery_runtime_state.h"
#include "network_credentials_state.h"
#include "network_diagnostics_catalog.h"
#include "network_diagnostics_probe.h"
#include "network_diagnostics_state_internal.h"
#include "network_sync_request_generation.h"
#include "network_sync_requests.h"
#include "network_sync_runtime.h"
#include "ntp_services.h"
#include "ui_settings_activity_state.h"
#include "ui_task_notify.h"
#include "weather_update.h"
#include "weather_provider.h"
#include "wifi_portal_state.h"

#include "esp_log.h"
#include "freertos/task.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
constexpr size_t kNetworkDiagPublicIpResponseBufferSize = 2048;
constexpr size_t kNetworkDiagWideProbeBufferSize = 1024;
constexpr size_t kNetworkDiagLocationTextSize = 32;
constexpr size_t kNetworkDiagCityTextSize = 32;
constexpr size_t kNetworkDiagPublicIpTextSize = 48;
constexpr int kNetworkDiagNtpMaxRetries = 5;
constexpr const char *kNetworkDiagPublicIpUrl = "https://uapis.cn/api/v1/network/myip";
constexpr const char *kNetworkDiagGithubDnsHost = "raw.githubusercontent.com";
constexpr const char *kNetworkDiagQweatherHostUnavailableLog =
    "network diag QWeather API host unavailable";
constexpr const char *kNetworkDiagStatusWaiting = "等待";
constexpr const char *kNetworkDiagStatusChecking = "检测中";
constexpr const char *kNetworkDiagStatusFailed = "超时/失败";
constexpr const char *kNetworkDiagStatusOk = "OK";
constexpr const char *kNetworkDiagStatusLowBatterySkipped = "电量低，已跳过";
constexpr const char *kNetworkDiagPlaceholder = "--";
constexpr const char *kNetworkDiagLocalIpPlaceholder = "本地IP: --";
constexpr const char *kNetworkDiagPublicIpPlaceholder = "公网IP: --";
constexpr const char *kNetworkDiagIpLocationFallback = "IP定位: 未检测";
constexpr const char *kNetworkDiagDnsUnchecked = "DNS: 未检测";
constexpr const char *kNetworkDiagWeatherUnchecked = "天气: 未检测";
constexpr const char *kNetworkDiagNtpUnchecked = "NTP: 未检测";
constexpr const char *kNetworkDiagSayingUnchecked = "一言: 未检测";
constexpr const char *kNetworkDiagInternetUnchecked = "公网: 未检测";
constexpr const char *kNetworkDiagOtaSourceUnchecked = "OTA源: 未检测";
constexpr const char *kNetworkDiagLocalIpFormat = "本地IP: %s";
constexpr const char *kNetworkDiagPublicIpFormat = "公网IP: %s";
constexpr const char *kNetworkDiagIpLocationFormat = "IP定位: %s";
constexpr const char *kNetworkDiagIpLocationCityFormat = "IP定位: %s %s";
constexpr const char *kNetworkDiagDnsFormat = "DNS: %s";
constexpr const char *kNetworkDiagWeatherFormat = "天气: %s";
constexpr const char *kNetworkDiagNtpFormat = "NTP: %s";
constexpr const char *kNetworkDiagSayingFormat = "一言: %s";
constexpr const char *kNetworkDiagInternetFormat = "公网: %s";
constexpr const char *kNetworkDiagOtaFormat = "OTA源: %s";
constexpr size_t kNetworkDiagIpv4TextMinSize = sizeof("255.255.255.255");
#define NETWORK_DIAG_LINE_INDEX_INVALID_FORMAT "network diag line index invalid: %d"
#define NETWORK_DIAG_LINE_FORMAT_FAILED_FORMAT "network diag line format failed index=%d"
#define NETWORK_DIAG_LINE_TRUNCATED_FORMAT "network diag line truncated index=%d len=%d"
static constexpr const char *kNetworkDiagBeginStateFailedLog =
    "network diag initial state publication failed";
static constexpr const char *kNetworkDiagTerminalStateFailedLog =
    "network diag terminal state publication failed";
void network_diag_set_line(int index, const char *fmt, ...);
constexpr int kNetworkDiagLineIndices[] = {
    kNetworkDiagLocalIpLine,
    kNetworkDiagPublicIpLine,
    kNetworkDiagIpLocationLine,
    kNetworkDiagDnsLine,
    kNetworkDiagWeatherLine,
    kNetworkDiagNtpLine,
    kNetworkDiagSayingLine,
    kNetworkDiagInternetLine,
    kNetworkDiagOtaLine,
};

void network_diag_clear_line(int index)
{
    network_diag_line_store(index, "");
}

void finish_network_diag_snapshot(const char *const *lines)
{
    if (network_diag_page_requested()) {
        settings_activity_record(xTaskGetTickCount());
    }
    if (!network_diag_state_publish(lines,
                                    kNetworkDiagLineCount,
                                    kNetworkDiagDone)) {
        ESP_LOGW(TAG, "%s", kNetworkDiagTerminalStateFailedLog);
        network_diag_state_store(kNetworkDiagDone);
    }
    notify_ui_task();
}

constexpr bool network_diag_lines_in_range()
{
    for (int index : kNetworkDiagLineIndices) {
        if (!network_diag_line_index_valid(index)) {
            return false;
        }
    }
    return true;
}

static_assert(kNetworkDiagWideProbeBufferSize > 0,
              "network diag wide probe buffer must be nonzero");
static_assert(kNetworkDiagPublicIpResponseBufferSize >= kNetworkDiagWideProbeBufferSize,
              "public IP response buffer must cover wide probe responses");
static_assert(kNetworkDiagLocationTextSize > 1, "network diag location text buffer must fit text and NUL");
static_assert(kNetworkDiagCityTextSize > 1, "network diag city text buffer must fit text and NUL");
static_assert(kNetworkDiagPublicIpTextSize > 1, "network diag public IP text buffer must fit text and NUL");
static_assert(kNetworkDiagPublicIpTextSize >= kNetworkDiagIpv4TextMinSize,
              "network diag public IP text buffer must fit IPv4 text");
static_assert(kNetworkDiagNtpMaxRetries > 0, "network diag NTP retry count must be positive");
static_assert(array_count(kNetworkDiagLineIndices) == kNetworkDiagLineCount,
              "network diag line index table must cover every UI row");
static_assert(network_diag_lines_in_range(), "network diag line indices must fit line table");
static_assert(kNetworkDiagOtaLine == kNetworkDiagLineCount - 1,
              "network diag OTA line must remain the final diagnostic row");
static_assert(kNetworkDiagLocalIpLine < kNetworkDiagPublicIpLine &&
                  kNetworkDiagPublicIpLine < kNetworkDiagIpLocationLine &&
                  kNetworkDiagIpLocationLine < kNetworkDiagDnsLine &&
                  kNetworkDiagDnsLine < kNetworkDiagWeatherLine &&
                  kNetworkDiagWeatherLine < kNetworkDiagNtpLine &&
                  kNetworkDiagNtpLine < kNetworkDiagSayingLine &&
                  kNetworkDiagSayingLine < kNetworkDiagInternetLine &&
                  kNetworkDiagInternetLine < kNetworkDiagOtaLine,
              "network diag line order must match UI initialization and execution order");

struct NetworkDiagLineFormat {
    int index;
    const char *format;
};

constexpr NetworkDiagLineFormat kNetworkDiagInitialLines[] = {
    {kNetworkDiagLocalIpLine, kNetworkDiagLocalIpFormat},
    {kNetworkDiagPublicIpLine, kNetworkDiagPublicIpFormat},
    {kNetworkDiagIpLocationLine, kNetworkDiagIpLocationFormat},
    {kNetworkDiagDnsLine, kNetworkDiagDnsFormat},
    {kNetworkDiagWeatherLine, kNetworkDiagWeatherFormat},
    {kNetworkDiagNtpLine, kNetworkDiagNtpFormat},
    {kNetworkDiagSayingLine, kNetworkDiagSayingFormat},
    {kNetworkDiagInternetLine, kNetworkDiagInternetFormat},
    {kNetworkDiagOtaLine, kNetworkDiagOtaFormat},
};

static_assert(array_count(kNetworkDiagInitialLines) == kNetworkDiagLineCount,
              "network diag initial line table must cover every UI row");
static_assert(kNetworkDiagInitialLines[0].index == kNetworkDiagLocalIpLine,
              "network diag initial table must follow visible row order");
static_assert(kNetworkDiagInitialLines[array_count(kNetworkDiagInitialLines) - 1].index == kNetworkDiagOtaLine,
              "network diag initial table must end with OTA row");

bool stop_remaining_network_diagnostics_if_low_battery(
    NetworkDiagLineIndex first_pending_line)
{
    if (!battery_low_mode_load()) {
        return false;
    }
    for (const auto &line : kNetworkDiagInitialLines) {
        if (line.index >= first_pending_line) {
            network_diag_set_line(line.index,
                                  line.format,
                                  kNetworkDiagStatusLowBatterySkipped);
        }
    }
    return true;
}

bool network_diagnostics_should_continue(
    NetworkDiagLineIndex first_pending_line,
    bool &completed,
    uint32_t request_generation)
{
    if (!network_sync_request_is_current(kNetworkDiagBit,
                                         request_generation)) {
        completed = false;
        return false;
    }
    if (stop_remaining_network_diagnostics_if_low_battery(
            first_pending_line)) {
        completed = true;
        return false;
    }
    if (!network_sync_continuation_allowed()) {
        completed = false;
        return false;
    }
    return true;
}

const char *diag_result_text(bool ok)
{
    return ok ? kNetworkDiagStatusOk : kNetworkDiagStatusFailed;
}

bool configured_weather_dns_ok()
{
    if (weather_provider_load() == WeatherProvider::kOpenMeteo) {
        return network_diagnostic_dns_lookup_ok("api.open-meteo.com");
    }
    char api_host[kQweatherApiHostLen] = {};
    if (!network_weather_api_host_snapshot(api_host, sizeof(api_host))) {
        ESP_LOGW(TAG, "%s", kNetworkDiagQweatherHostUnavailableLog);
        return false;
    }
    return network_diagnostic_dns_lookup_ok(api_host);
}
} // namespace

void network_diag_reset()
{
    network_diag_state_clear(kNetworkDiagIdle);
}

void network_diag_begin()
{
    const char *line_formats[kNetworkDiagLineCount] = {};
    for (const auto &line : kNetworkDiagInitialLines) {
        line_formats[line.index] = line.format;
    }
    if (!network_diag_state_begin(line_formats,
                                  kNetworkDiagLineCount,
                                  kNetworkDiagStatusWaiting)) {
        ESP_LOGW(TAG, "%s", kNetworkDiagBeginStateFailedLog);
    }
    notify_ui_task();
}

void network_diag_finish()
{
    if (network_diag_page_requested()) {
        settings_activity_record(xTaskGetTickCount());
    }
    network_diag_state_store(kNetworkDiagDone);
    notify_ui_task();
}

void network_diag_finish_with_status(const char *status_text)
{
    if (!status_text) {
        network_diag_finish();
        return;
    }
    const char *lines[kNetworkDiagLineCount] = {};
    for (auto &line : lines) {
        line = status_text;
    }
    finish_network_diag_snapshot(lines);
}

void network_diag_finish_unavailable(const char *ip_location_text)
{
    const char *lines[kNetworkDiagLineCount] = {
        kNetworkDiagLocalIpPlaceholder,
        kNetworkDiagPublicIpPlaceholder,
        ip_location_text ? ip_location_text : kNetworkDiagIpLocationFallback,
        kNetworkDiagDnsUnchecked,
        kNetworkDiagWeatherUnchecked,
        kNetworkDiagNtpUnchecked,
        kNetworkDiagSayingUnchecked,
        kNetworkDiagInternetUnchecked,
        kNetworkDiagOtaSourceUnchecked,
    };
    finish_network_diag_snapshot(lines);
}

namespace {
void network_diag_set_line(int index, const char *fmt, ...)
{
    if (!network_diag_line_index_valid(index)) {
        ESP_LOGW(TAG, NETWORK_DIAG_LINE_INDEX_INVALID_FORMAT, index);
        return;
    }
    if (!fmt) {
        network_diag_clear_line(index);
        notify_ui_task();
        return;
    }
    char line[kNetworkDiagLineLen] = {};
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (written < 0) {
        line[0] = '\0';
        ESP_LOGW(TAG, NETWORK_DIAG_LINE_FORMAT_FAILED_FORMAT, index);
    } else if (written >= kNetworkDiagLineLen) {
        line[kNetworkDiagLineLen - 1] = '\0';
        ESP_LOGW(TAG, NETWORK_DIAG_LINE_TRUNCATED_FORMAT, index, written);
    }
    network_diag_line_store(index, line);
    notify_ui_task();
}

void network_diag_set_result_line(int index, const char *fmt, bool ok)
{
    network_diag_set_line(index, fmt, diag_result_text(ok));
}

void network_diag_set_checking_line(int index, const char *fmt)
{
    network_diag_set_line(index, fmt, kNetworkDiagStatusChecking);
}

void network_diag_record_result_line(int index, const char *fmt, bool ok)
{
    network_diag_set_result_line(index, fmt, ok);
}

void network_diag_record_text_line(int index, const char *fmt, bool ok, const char *success_text, const char *failed_text)
{
    network_diag_set_line(index, fmt, ok ? success_text : failed_text);
}
} // namespace

bool run_network_diagnostic_checks(uint32_t request_generation)
{
    bool completed = false;
    if (!network_diagnostics_should_continue(kNetworkDiagLocalIpLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    char location[kNetworkDiagLocationTextSize] = {};
    char city[kNetworkDiagCityTextSize] = {};
    char public_ip[kNetworkDiagPublicIpTextSize] = {};
    char local_ip[kWifiStationIpTextLen] = {};
    bool local_ip_ok = wifi_station_ip_snapshot(local_ip, sizeof(local_ip));
    network_diag_record_text_line(kNetworkDiagLocalIpLine,
                                  kNetworkDiagLocalIpFormat,
                                  local_ip_ok,
                                  local_ip,
                                  kNetworkDiagPlaceholder);
    if (!network_diagnostics_should_continue(kNetworkDiagPublicIpLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }

    network_diag_set_checking_line(kNetworkDiagPublicIpLine, kNetworkDiagPublicIpFormat);
    const NetworkDiagnosticPublicIpLookupResult public_ip_result =
        network_diagnostic_lookup_public_ip(
            kNetworkDiagPublicIpUrl,
            public_ip,
            sizeof(public_ip),
            kNetworkDiagPublicIpResponseBufferSize);
    bool public_ip_ok = public_ip_result.address_ok;
    if (!network_diagnostics_should_continue(kNetworkDiagPublicIpLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_record_text_line(kNetworkDiagPublicIpLine,
                                  kNetworkDiagPublicIpFormat,
                                  public_ip_ok,
                                  public_ip,
                                  kNetworkDiagStatusFailed);
    if (!network_diagnostics_should_continue(kNetworkDiagIpLocationLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }

    network_diag_set_checking_line(kNetworkDiagIpLocationLine, kNetworkDiagIpLocationFormat);
    bool ip_ok = ip_geolocation_lookup(location, sizeof(location), city, sizeof(city));
    if (!network_diagnostics_should_continue(kNetworkDiagIpLocationLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_set_line(kNetworkDiagIpLocationLine, kNetworkDiagIpLocationCityFormat,
                          diag_result_text(ip_ok),
                          city[0] ? city : kNetworkDiagPlaceholder);
    if (!network_diagnostics_should_continue(kNetworkDiagDnsLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }

    network_diag_set_checking_line(kNetworkDiagDnsLine, kNetworkDiagDnsFormat);
    bool dns_ok = configured_weather_dns_ok() &&
                  network_diagnostic_dns_lookup_ok(kNetworkDiagGithubDnsHost);
    if (!network_diagnostics_should_continue(kNetworkDiagDnsLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_record_result_line(kNetworkDiagDnsLine, kNetworkDiagDnsFormat, dns_ok);
    if (!network_diagnostics_should_continue(kNetworkDiagWeatherLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }

    bool weather_ok = false;
    if (network_weather_configuration_configured() &&
        !battery_low_mode_load()) {
        network_diag_set_checking_line(kNetworkDiagWeatherLine, kNetworkDiagWeatherFormat);
        weather_ok =
            perform_weather_update(WeatherUpdateScope::kFull) ==
            WeatherUpdateResult::kSuccess;
    }
    if (!network_diagnostics_should_continue(kNetworkDiagWeatherLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_record_result_line(kNetworkDiagWeatherLine,
                                    kNetworkDiagWeatherFormat,
                                    weather_ok);
    if (!network_diagnostics_should_continue(kNetworkDiagNtpLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_set_checking_line(kNetworkDiagNtpLine, kNetworkDiagNtpFormat);
    bool ntp_ok = perform_ntp_sync(kNetworkDiagNtpMaxRetries);
    if (!network_diagnostics_should_continue(kNetworkDiagNtpLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_record_result_line(kNetworkDiagNtpLine, kNetworkDiagNtpFormat, ntp_ok);
    if (!network_diagnostics_should_continue(kNetworkDiagSayingLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }

    network_diag_set_checking_line(kNetworkDiagSayingLine, kNetworkDiagSayingFormat);
    bool saying_ok = !battery_low_mode_load() && perform_daily_saying_update();
    if (!network_diagnostics_should_continue(kNetworkDiagSayingLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_record_result_line(kNetworkDiagSayingLine,
                                    kNetworkDiagSayingFormat,
                                    saying_ok);
    if (!network_diagnostics_should_continue(kNetworkDiagInternetLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_set_checking_line(kNetworkDiagInternetLine, kNetworkDiagInternetFormat);
    bool internet_ok = public_ip_result.request_ok ||
                       network_diagnostic_http_probe_ok(
                           kNetworkDiagPublicIpUrl,
                           kNetworkDiagWideProbeBufferSize);
    if (!network_diagnostics_should_continue(kNetworkDiagInternetLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_record_result_line(kNetworkDiagInternetLine, kNetworkDiagInternetFormat, internet_ok);
    if (!network_diagnostics_should_continue(kNetworkDiagOtaLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }

    network_diag_set_checking_line(kNetworkDiagOtaLine, kNetworkDiagOtaFormat);
    bool ota_ok = network_diagnostic_http_probe_ok(
        kOtaManifestUrl,
        kNetworkDiagWideProbeBufferSize);
    if (!network_diagnostics_should_continue(kNetworkDiagOtaLine,
                                             completed,
                                             request_generation)) {
        return completed;
    }
    network_diag_record_result_line(kNetworkDiagOtaLine, kNetworkDiagOtaFormat, ota_ok);
    return true;
}
