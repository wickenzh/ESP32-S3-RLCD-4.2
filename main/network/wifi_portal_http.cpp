// 实现配网 HTTP 路由、表单保存、城市校验和强制门户服务生命周期。
#include "network_services.h"

#include "app_constexpr.h"
#include "manual_weather_city_state.h"
#include "network_diagnostics_state.h"
#include "wifi_portal_dns.h"
#include "wifi_portal_pages.h"
#include "wifi_portal_state.h"

#include "ui_info_page_state.h"
#include "ui_settings_activity_state.h"
#include "ui_views.h"

namespace {
httpd_handle_t s_http_server = nullptr;
constexpr uint16_t kSetupHttpServerPort = 80;
constexpr size_t kSetupHttpServerStackSize = 8192;
constexpr size_t kPortalSubmitSsidFieldSize = 33;
constexpr size_t kPortalRequestBufferSize = 640;
constexpr size_t kPortalWeatherCityIdSize = 24;
constexpr size_t kPortalWeatherCityNameSize = 32;
constexpr uint32_t kPortalSaveWifiConnectWaitMs = 12000;
constexpr const char *kPortalHttpStatusBadRequest = "400 Bad Request";
constexpr const char *kPortalHttpStatusNoContent = "204 No Content";
constexpr const char *kPortalErrorMissingQuery = "缺少请求参数。";
constexpr const char *kPortalWeatherCityInvalidMessage =
    "QWeather 无法识别填写的天气城市，已恢复为自动定位。";
constexpr const char *kPortalWeatherCityDeferredMessage =
    "天气城市已保存，但在线校验超时；下次同步天气时会自动重试。";
constexpr const char *kPortalRootUri = "/";
constexpr const char *kPortalSaveUri = "/save";
constexpr const char *kPortalFaviconUri = "/favicon.ico";
constexpr const char *kPortalAppleTouchIconUri = "/apple-touch-icon.png";
constexpr const char *kPortalAppleTouchIconPrecomposedUri = "/apple-touch-icon-precomposed.png";
constexpr const char *kPortalWildcardUri = "/*";
struct PortalHttpRoute {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
};

enum ManualWeatherCityValidationResult {
    kManualWeatherCityValidationOk,
    kManualWeatherCityValidationInvalid,
    kManualWeatherCityValidationDeferred,
};

constexpr PortalHttpRoute kPortalHttpRoutes[] = {
    {kPortalRootUri, HTTP_GET, root_get_handler},
    {kPortalSaveUri, HTTP_POST, save_post_handler},
    {kPortalSaveUri, HTTP_GET, save_get_handler},
    {kPortalFaviconUri, HTTP_GET, empty_asset_handler},
    {kPortalAppleTouchIconUri, HTTP_GET, empty_asset_handler},
    {kPortalAppleTouchIconPrecomposedUri, HTTP_GET, empty_asset_handler},
    {kPortalWildcardUri, HTTP_GET, captive_portal_handler},
};

constexpr bool portal_http_routes_valid()
{
    for (const PortalHttpRoute &route : kPortalHttpRoutes) {
        if (!cstr_nonempty(route.uri) || !route.handler) {
            return false;
        }
    }
    return true;
}

static_assert(kSetupHttpServerPort > 0, "setup HTTP server port must be positive");
static_assert(kSetupHttpServerStackSize > 0, "setup HTTP server stack must be positive");
static_assert(kPortalRequestBufferSize > kPortalSubmitSsidFieldSize,
              "portal request buffer must exceed submitted SSID field size");
static_assert(kPortalWeatherCityIdSize > 1, "portal weather city id buffer must fit text and NUL");
static_assert(kPortalWeatherCityNameSize > 1, "portal weather city name buffer must fit text and NUL");
static_assert(kPortalSaveWifiConnectWaitMs > 0, "portal save Wi-Fi wait must be positive");
static_assert(array_count(kPortalHttpRoutes) > 0, "portal HTTP route table must not be empty");
static_assert(portal_http_routes_valid(), "portal HTTP routes must have URI and handler");

#define SETUP_PORTAL_WITHOUT_CAPTIVE_DNS_LOG "setup portal running without captive dns"
#define PORTAL_HTTP_SERVER_START_FAILED_FORMAT "http server start failed: %s"
#define PORTAL_HTTP_SERVER_STOP_FAILED_FORMAT "http server stop failed: %s"
#define PORTAL_HTTP_URI_REGISTER_FAILED_FORMAT "http uri register failed: %s"
#define PORTAL_POST_BODY_TRUNCATED_FORMAT "setup POST body truncated content_len=%d buffer=%u"
#define PORTAL_POST_BODY_RECEIVE_FAILED_FORMAT "setup POST body receive failed ret=%d received=%d expected=%d"
#define MANUAL_WEATHER_CITY_VALIDATED_FORMAT "manual weather city validated: %s id=%s"
#define MANUAL_WEATHER_CITY_VALIDATION_FAILED_LOG "manual weather city validation failed, restoring auto location"
#define MANUAL_WEATHER_CITY_VALIDATION_DEFERRED_LOG "manual weather city validation deferred after network/API error"
#define PORTAL_PROVISIONING_SYNC_EVENT_UNAVAILABLE_LOG "setup save skipped initial sync request: app events unavailable"

void request_provisioning_sync_after_save()
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "%s", PORTAL_PROVISIONING_SYNC_EVENT_UNAVAILABLE_LOG);
        return;
    }
    xEventGroupSetBits(g_app_events, kProvisioningSyncBit);
}

bool stop_http_server_handle()
{
    if (!s_http_server) {
        return true;
    }
    esp_err_t err = httpd_stop(s_http_server);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, PORTAL_HTTP_SERVER_STOP_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }
    s_http_server = nullptr;
    return true;
}

ManualWeatherCityValidationResult validate_saved_manual_weather_city()
{
    char weather_city[kManualWeatherCityLen] = {};
    if (!manual_weather_city_snapshot(weather_city, sizeof(weather_city))) {
        return kManualWeatherCityValidationOk;
    }
    char city_id[kPortalWeatherCityIdSize] = {};
    char city_name[kPortalWeatherCityNameSize] = {};
    QweatherCityLookupStatus status = qweather_lookup_city_status(weather_city,
                                                                  city_id,
                                                                  sizeof(city_id),
                                                                  city_name,
                                                                  sizeof(city_name));
    if (status == kQweatherCityLookupOk) {
        ESP_LOGI(TAG, MANUAL_WEATHER_CITY_VALIDATED_FORMAT, city_name, city_id);
        return kManualWeatherCityValidationOk;
    }
    if (status == kQweatherCityLookupNotFound) {
        ESP_LOGW(TAG, MANUAL_WEATHER_CITY_VALIDATION_FAILED_LOG);
        (void)clear_manual_weather_city();
        return kManualWeatherCityValidationInvalid;
    }
    ESP_LOGW(TAG, MANUAL_WEATHER_CITY_VALIDATION_DEFERRED_LOG);
    return kManualWeatherCityValidationDeferred;
}

esp_err_t handle_setup_save(httpd_req_t *req, const char *body)
{
    char ssid[kPortalSubmitSsidFieldSize] = {};
    form_value(body, "ssid", ssid, sizeof(ssid));
    trim_ascii(ssid);
    if (ssid[0] == '\0') {
        bool offline_saved = save_offline_datetime_from_body(body);
        esp_err_t err = send_offline_result_page(req, offline_saved);
        if (offline_saved) {
            settings_page_clear();
            network_diag_page_clear();
            info_page_clear();
            stop_wifi_radio(true);
            notify_ui_task();
        }
        return err;
    }
    bool saved = save_credentials_from_body(body);
    bool connected = saved && wait_for_wifi_connected(kPortalSaveWifiConnectWaitMs);
    const char *extra_message = nullptr;
    if (connected) {
        ManualWeatherCityValidationResult city_result = validate_saved_manual_weather_city();
        if (city_result == kManualWeatherCityValidationInvalid) {
            extra_message = kPortalWeatherCityInvalidMessage;
        } else if (city_result == kManualWeatherCityValidationDeferred) {
            extra_message = kPortalWeatherCityDeferredMessage;
        }
    }
    esp_err_t err = send_save_result_page(req, saved, connected, extra_message);
    if (connected) {
        request_provisioning_sync_after_save();
    }
    return err;
}

esp_err_t receive_portal_post_body(httpd_req_t *req, char *body, size_t body_size)
{
    if (!req || !body || body_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    int total = 0;
    const int capacity = (int)body_size - 1;
    while (total < req->content_len && total < capacity) {
        int ret = httpd_req_recv(req, body + total, capacity - total);
        if (ret <= 0) {
            ESP_LOGW(TAG, PORTAL_POST_BODY_RECEIVE_FAILED_FORMAT, ret, total, req->content_len);
            return ESP_FAIL;
        }
        total += ret;
    }
    body[total] = '\0';
    if (total < req->content_len) {
        ESP_LOGW(TAG, PORTAL_POST_BODY_TRUNCATED_FORMAT, req->content_len, (unsigned)body_size);
    }
    return ESP_OK;
}

esp_err_t register_http_handler(httpd_handle_t server,
                                const char *uri,
                                httpd_method_t method,
                                esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t route = {};
    route.uri = uri;
    route.method = method;
    route.handler = handler;
    return httpd_register_uri_handler(server, &route);
}
} // namespace

void stop_http_server()
{
    (void)stop_http_server_handle();
    stop_captive_dns_server();
    setup_portal_active_store(false);
}

esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[kPortalRequestBufferSize] = {};
    esp_err_t err = receive_portal_post_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        return err;
    }
    return handle_setup_save(req, body);
}

esp_err_t save_get_handler(httpd_req_t *req)
{
    if (!req) {
        return ESP_ERR_INVALID_ARG;
    }
    char query[kPortalRequestBufferSize] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_portal_text_status(req, kPortalHttpStatusBadRequest, kPortalErrorMissingQuery);
    }
    return handle_setup_save(req, query);
}

esp_err_t empty_asset_handler(httpd_req_t *req)
{
    return send_portal_empty_status(req, kPortalHttpStatusNoContent);
}

esp_err_t captive_portal_handler(httpd_req_t *req)
{
    return redirect_to_setup_portal(req);
}

bool start_http_server()
{
    if (s_http_server && !setup_portal_active_load() && !stop_http_server_handle()) {
        return false;
    }
    if (s_http_server) {
        setup_portal_active_store(true);
        if (!start_captive_dns_server()) {
            ESP_LOGW(TAG, SETUP_PORTAL_WITHOUT_CAPTIVE_DNS_LOG);
        }
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kSetupHttpServerPort;
    config.stack_size = kSetupHttpServerStackSize;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        s_http_server = nullptr;
        setup_portal_active_store(false);
        ESP_LOGW(TAG, PORTAL_HTTP_SERVER_START_FAILED_FORMAT, esp_err_to_name(err));
        return false;
    }

    for (const PortalHttpRoute &route : kPortalHttpRoutes) {
        if (err != ESP_OK) {
            break;
        }
        err = register_http_handler(s_http_server, route.uri, route.method, route.handler);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, PORTAL_HTTP_URI_REGISTER_FAILED_FORMAT, esp_err_to_name(err));
        (void)stop_http_server_handle();
        setup_portal_active_store(false);
        return false;
    }
    if (!start_captive_dns_server()) {
        ESP_LOGW(TAG, SETUP_PORTAL_WITHOUT_CAPTIVE_DNS_LOG);
    }
    setup_portal_active_store(true);
    return true;
}
