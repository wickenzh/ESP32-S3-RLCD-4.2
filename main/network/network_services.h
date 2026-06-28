// 声明配网、Wi-Fi、HTTP、天气、NTP、OTA 调度等网络服务接口。
#pragma once
#include "app_state.h"

struct HttpBuffer {
    char *data;
    size_t len;
    size_t cap;
};

bool load_saved_config();
bool save_config(const char *ssid, const char *pass, const char *api_key);
bool set_offline_mode_enabled(bool enabled);
bool can_leave_offline_mode_without_setup();
void clear_network_request_bits();
bool save_hourly_chime_setting();
bool save_work_page_settings();
bool save_work_page_order();
bool clear_saved_config();
void url_decode(char *dst, size_t dst_len, const char *src);
void form_value(const char *body, const char *key, char *out, size_t out_len);
void form_value_fallback(const char *body, const char *primary_key, const char *fallback_key, char *out, size_t out_len);
bool save_credentials_from_body(const char *body);
bool save_offline_datetime_from_body(const char *body);
void html_append(char *html, size_t html_len, const char *fmt, ...);
void html_escape(const char *src, char *dst, size_t dst_len);
void append_wifi_scan_list(char *html, size_t html_len);
bool apply_station_config(bool reconnect);
void stop_http_server();
esp_err_t root_get_handler(httpd_req_t *req);
esp_err_t send_save_result_page(httpd_req_t *req, bool saved, bool connected);
esp_err_t save_post_handler(httpd_req_t *req);
esp_err_t save_get_handler(httpd_req_t *req);
esp_err_t empty_asset_handler(httpd_req_t *req);
esp_err_t captive_portal_handler(httpd_req_t *req);
bool start_captive_dns_server();
void stop_captive_dns_server();
bool start_http_server();
bool start_wifi_radio(bool enable_setup_portal);
void stop_wifi_radio(bool force_setup_portal = false);
void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data);
void init_wifi();
bool perform_ntp_sync(int max_retries = 30);
int boot_sync_remaining_ms();
esp_err_t http_event_handler(esp_http_client_event_t *evt);
bool gzip_payload_range(const uint8_t *data, size_t len, size_t *payload_offset, size_t *payload_len);
esp_err_t decode_http_body(char *out, size_t out_len, size_t *body_len);
esp_err_t http_get_text(const char *url, char *out, size_t out_len, const char *api_key = nullptr);
const char *qweather_api_host();
void trim_ascii(char *text);
bool json_copy_string(cJSON *obj, const char *name, char *out, size_t out_len);
bool url_is_unreserved(char ch);
bool url_encode_component(const char *in, char *out, size_t out_len);
void log_response_preview(const char *stage, const char *response);
bool ip_geolocation_lookup(char *location, size_t location_len, char *city, size_t city_len);
bool qweather_lookup_city(const char *location,
                          char *city_id,
                          size_t city_id_len,
                          char *city_name,
                          size_t city_name_len,
                          char *lat_out = nullptr,
                          size_t lat_len = 0,
                          char *lon_out = nullptr,
                          size_t lon_len = 0);
const char *warning_color_name(const char *code);
int warning_color_rank(const char *code);
void add_weather_alert_title(WeatherAlertData *alert, const char *title, int rank);
bool qweather_fetch_alert(const char *lat, const char *lon, WeatherAlertData *alert);
bool qweather_fetch_now(const char *city_id, WeatherData *weather);
bool qweather_fetch_daily(const char *city_id, WeatherForecastData *forecast);
bool qweather_fetch_air(const char *city_id, WeatherAirData *air);
void get_weather_snapshot(WeatherData *weather, WeatherAlertData *alert);
void get_weather_forecast_snapshot(WeatherForecastData *forecast);
void get_weather_air_snapshot(WeatherAirData *air);
bool perform_weather_update();
void load_daily_saying_cache();
bool perform_daily_saying_update();
uint32_t weather_icon_codepoint(const char *code);
const char *weather_icon_text(const char *code);
bool wait_for_wifi_connected(uint32_t timeout_ms);
bool is_time_valid(struct tm *local_out = nullptr);
void run_boot_connectivity_sync();
void boot_connectivity_task(void *);
void wait_for_network_sync_event(uint32_t timeout_ms);
uint32_t network_idle_wait_ms(time_t now, time_t next_weather_at, time_t next_ntp_retry_at);
void network_diag_reset();
void network_diag_begin();
void network_diag_finish();
void network_diag_set_line(int index, const char *fmt, ...);
void run_network_diagnostics();
void network_sync_task(void *);
