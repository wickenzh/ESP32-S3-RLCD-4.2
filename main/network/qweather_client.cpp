// 对接 IP 定位、QWeather 城市查询、实时天气和天气预警接口。
#include "network_services.h"

#include "audio_services.h"
#include "sensor_services.h"
#include "ui_views.h"

#include <stdarg.h>

const char *qweather_api_host()
{
    return "devapi.qweather.com";
}

namespace {
constexpr size_t kIpGeoResponseBufferSize = 2048;
constexpr size_t kQweatherCityResponseBufferSize = 8192;
constexpr size_t kQweatherNowResponseBufferSize = 8192;
constexpr size_t kQweatherAlertResponseBufferSize = 16384;
constexpr size_t kQweatherDailyResponseBufferSize = 24576;
constexpr size_t kQweatherAirResponseBufferSize = 8192;

bool format_qweather_url(char *out, size_t out_len, const char *stage, const char *fmt, ...)
{
    if (!out || out_len == 0 || !fmt) {
        ESP_LOGW(TAG, "qweather url invalid arg stage=%s", stage ? stage : "unknown");
        return false;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out, out_len, fmt, args);
    va_end(args);
    if (written < 0 || written >= (int)out_len) {
        out[0] = '\0';
        ESP_LOGW(TAG, "qweather %s url too long", stage ? stage : "request");
        return false;
    }
    return true;
}

char *alloc_qweather_response(const char *stage, size_t buffer_size)
{
    if (buffer_size == 0) {
        ESP_LOGW(TAG, "qweather %s response size invalid", stage ? stage : "request");
        return nullptr;
    }
    char *response = (char *)malloc(buffer_size);
    if (!response) {
        ESP_LOGW(TAG, "qweather %s response alloc failed", stage ? stage : "request");
        return nullptr;
    }
    response[0] = '\0';
    return response;
}
} // namespace

bool ip_geolocation_lookup(char *location, size_t location_len, char *city, size_t city_len)
{
    if (!location || location_len == 0 || !city || city_len == 0) {
        ESP_LOGW(TAG, "ip location invalid arg");
        return false;
    }
    char *response = alloc_qweather_response("ip location", kIpGeoResponseBufferSize);
    if (!response) {
        return false;
    }
    if (http_get_text("https://uapis.cn/api/v1/network/myip", response, kIpGeoResponseBufferSize) != ESP_OK) {
        free(response);
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        free(response);
        return false;
    }
    bool ok = false;
    cJSON *lat = cJSON_GetObjectItem(root, "latitude");
    cJSON *lon = cJSON_GetObjectItem(root, "longitude");
    cJSON *region = cJSON_GetObjectItem(root, "region");
    if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        snprintf(location, location_len, "%.4f,%.4f", lon->valuedouble, lat->valuedouble);
        if (cJSON_IsString(region) && region->valuestring) {
            char region_copy[96] = {};
            strlcpy(region_copy, region->valuestring, sizeof(region_copy));
            char *parts[5] = {};
            int count = 0;
            char *token = strtok(region_copy, " ");
            while (token && count < (int)(sizeof(parts) / sizeof(parts[0]))) {
                if (token[0] != '\0') {
                    parts[count++] = token;
                }
                token = strtok(nullptr, " ");
            }
            const char *city_part = count >= 3 ? parts[2] : (count > 0 ? parts[count - 1] : "");
            strlcpy(city, city_part, city_len);
            size_t len = strlen(city);
            if (len >= 3 && strcmp(city + len - 3, "市") == 0) {
                city[len - 3] = '\0';
            }
        }
        if (city[0] == '\0') {
            strlcpy(city, location, city_len);
        }
        ESP_LOGI(TAG, "ip location resolved: %s city=%s", location, city);
        ok = true;
    }
    cJSON_Delete(root);
    free(response);
    return ok;
}

bool qweather_lookup_city(const char *location,
                                 char *city_id,
                                 size_t city_id_len,
                                 char *city_name,
                                 size_t city_name_len,
                                 char *lat_out,
                                 size_t lat_len,
                                 char *lon_out,
                                 size_t lon_len)
{
    if (!location || !city_id || city_id_len == 0 || !city_name || city_name_len == 0) {
        ESP_LOGW(TAG, "qweather city invalid arg");
        return false;
    }
    char encoded_location[128] = {};
    if (!url_encode_component(location, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "qweather city location too long");
        return false;
    }

    char url[512];
    if (!format_qweather_url(url,
                             sizeof(url),
                             "city",
                             "https://geoapi.qweather.com/v2/city/lookup?location=%s&number=1&range=cn&lang=zh",
                             encoded_location)) {
        return false;
    }
    ESP_LOGI(TAG, "qweather city lookup: %s via geoapi.qweather.com", location);
    char *response = alloc_qweather_response("city", kQweatherCityResponseBufferSize);
    if (!response) {
        return false;
    }
    if (http_get_text(url, response, kQweatherCityResponseBufferSize, g_weather_api_key) != ESP_OK) {
        ESP_LOGW(TAG, "qweather city lookup http failed");
        free(response);
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather city", response);
        free(response);
        return false;
    }
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *locations = cJSON_GetObjectItem(root, "location");
    cJSON *first = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : nullptr;
    if (cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 && first) {
        ok = json_copy_string(first, "id", city_id, city_id_len) &&
             json_copy_string(first, "name", city_name, city_name_len);
        if (ok) {
            if (lat_out && lat_len > 0) {
                json_copy_string(first, "lat", lat_out, lat_len);
            }
            if (lon_out && lon_len > 0) {
                json_copy_string(first, "lon", lon_out, lon_len);
            }
            ESP_LOGI(TAG, "qweather city resolved: %s id=%s", city_name, city_id);
        }
    } else {
        ESP_LOGW(TAG, "qweather city lookup failed code=%s",
                 cJSON_IsString(code) ? code->valuestring : "missing");
    }
    cJSON_Delete(root);
    free(response);
    return ok;
}

const char *warning_color_name(const char *code)
{
    if (!code) {
        return "";
    }
    if (strcmp(code, "blue") == 0) return "蓝色";
    if (strcmp(code, "yellow") == 0) return "黄色";
    if (strcmp(code, "orange") == 0) return "橙色";
    if (strcmp(code, "red") == 0) return "红色";
    if (strcmp(code, "white") == 0) return "白色";
    if (strcmp(code, "black") == 0) return "黑色";
    return "";
}

int warning_color_rank(const char *code)
{
    if (!code) {
        return 0;
    }
    if (strcmp(code, "red") == 0) return 5;
    if (strcmp(code, "orange") == 0) return 4;
    if (strcmp(code, "yellow") == 0) return 3;
    if (strcmp(code, "blue") == 0) return 2;
    if (strcmp(code, "white") == 0) return 1;
    if (strcmp(code, "black") == 0) return 1;
    return 0;
}

static size_t alert_utf8_char_len(unsigned char ch)
{
    if ((ch & 0x80) == 0) return 1;
    if ((ch & 0xE0) == 0xC0) return 2;
    if ((ch & 0xF0) == 0xE0) return 3;
    if ((ch & 0xF8) == 0xF0) return 4;
    return 1;
}

static size_t alert_utf8_char_count(const char *text)
{
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)text; p && *p;) {
        size_t len = alert_utf8_char_len(*p);
        if (len == 0) {
            break;
        }
        p += len;
        ++count;
    }
    return count;
}

static void alert_utf8_copy_chars(char *out, size_t out_len, const char *in, size_t max_chars)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }
    size_t used = 0;
    size_t chars = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && chars < max_chars) {
        size_t len = alert_utf8_char_len(*p);
        if (used + len >= out_len) {
            break;
        }
        memcpy(out + used, p, len);
        used += len;
        p += len;
        ++chars;
    }
    out[used] = '\0';
}

static void replace_all(char *text, size_t text_len, const char *from, const char *to)
{
    if (!text || text_len == 0 || !from || !to) {
        return;
    }
    char buffer[kWeatherAlertTitleLen] = {};
    const char *read = text;
    size_t used = 0;
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    if (from_len == 0) {
        return;
    }
    while (*read && used + 1 < sizeof(buffer)) {
        if (strncmp(read, from, from_len) == 0) {
            if (used + to_len >= sizeof(buffer)) {
                break;
            }
            memcpy(buffer + used, to, to_len);
            used += to_len;
            read += from_len;
        } else {
            size_t len = alert_utf8_char_len((unsigned char)*read);
            if (used + len >= sizeof(buffer)) {
                break;
            }
            memcpy(buffer + used, read, len);
            used += len;
            read += len;
        }
    }
    buffer[used] = '\0';
    strlcpy(text, buffer, text_len);
}

static void compact_weather_alert_title(char *title, size_t title_len)
{
    if (!title || title[0] == '\0') {
        return;
    }
    if (alert_utf8_char_count(title) <= 6) {
        return;
    }
    replace_all(title, title_len, "预警", "");
    replace_all(title, title_len, "蓝色", "蓝");
    replace_all(title, title_len, "黄色", "黄");
    replace_all(title, title_len, "橙色", "橙");
    replace_all(title, title_len, "红色", "红");
    replace_all(title, title_len, "白色", "白");
    replace_all(title, title_len, "黑色", "黑");
    if (alert_utf8_char_count(title) > 6) {
        char clipped[kWeatherAlertTitleLen] = {};
        alert_utf8_copy_chars(clipped, sizeof(clipped), title, 6);
        strlcpy(title, clipped, title_len);
    }
}

void add_weather_alert_title(WeatherAlertData *alert, const char *title, int rank)
{
    if (!alert || !title || title[0] == '\0') {
        return;
    }

    int insert_at = alert->count;
    for (int i = 0; i < alert->count; ++i) {
        if (rank > alert->ranks[i]) {
            insert_at = i;
            break;
        }
    }

    if (alert->count < kMaxWeatherAlerts) {
        ++alert->count;
    } else if (insert_at >= kMaxWeatherAlerts) {
        return;
    }

    for (int i = alert->count - 1; i > insert_at; --i) {
        strlcpy(alert->titles[i], alert->titles[i - 1], sizeof(alert->titles[i]));
        alert->ranks[i] = alert->ranks[i - 1];
    }
    char compact_title[kWeatherAlertTitleLen] = {};
    strlcpy(compact_title, title, sizeof(compact_title));
    compact_weather_alert_title(compact_title, sizeof(compact_title));
    strlcpy(alert->titles[insert_at], compact_title, sizeof(alert->titles[insert_at]));
    alert->ranks[insert_at] = rank;
    alert->active = alert->count > 0;
}

bool qweather_fetch_alert(const char *lat, const char *lon, WeatherAlertData *alert)
{
    if (!alert) {
        ESP_LOGW(TAG, "qweather alert invalid arg");
        return false;
    }
    if (!lat || !lon || lat[0] == '\0' || lon[0] == '\0') {
        alert->active = false;
        return true;
    }

    char url[256];
    if (!format_qweather_url(url,
                             sizeof(url),
                             "alert",
                             "https://%s/weatheralert/v1/current/%s/%s?lang=zh&localTime=true",
                             qweather_api_host(),
                             lat,
                             lon)) {
        return false;
    }
    ESP_LOGI(TAG, "qweather alert lookup: %s,%s via %s", lat, lon, qweather_api_host());
    char *response = alloc_qweather_response("alert", kQweatherAlertResponseBufferSize);
    if (!response) {
        return false;
    }
    if (http_get_text(url, response, kQweatherAlertResponseBufferSize, g_weather_api_key) != ESP_OK) {
        ESP_LOGW(TAG, "qweather alert http failed");
        free(response);
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather alert", response);
        free(response);
        return false;
    }

    WeatherAlertData next = {};
    bool ok = true;
    cJSON *alerts = cJSON_GetObjectItem(root, "alerts");
    int alert_count = cJSON_IsArray(alerts) ? cJSON_GetArraySize(alerts) : 0;
    for (int i = 0; i < alert_count; ++i) {
        cJSON *item = cJSON_GetArrayItem(alerts, i);
        if (!item) {
            continue;
        }
        char event_name[24] = {};
        char color_code[16] = {};
        char headline[64] = {};
        cJSON *event = cJSON_GetObjectItem(item, "eventType");
        cJSON *color = cJSON_GetObjectItem(item, "color");
        if (event) {
            json_copy_string(event, "name", event_name, sizeof(event_name));
        }
        if (color) {
            json_copy_string(color, "code", color_code, sizeof(color_code));
        }
        json_copy_string(item, "headline", headline, sizeof(headline));

        int rank = warning_color_rank(color_code);

        char title[kWeatherAlertTitleLen] = {};
        const char *color_name = warning_color_name(color_code);
        if (event_name[0] != '\0' && color_name[0] != '\0') {
            snprintf(title, sizeof(title), "%s%s预警", event_name, color_name);
        } else if (headline[0] != '\0') {
            strlcpy(title, headline, sizeof(title));
        } else if (event_name[0] != '\0') {
            snprintf(title, sizeof(title), "%s预警", event_name);
        }
        add_weather_alert_title(&next, title, rank);
    }
    next.active = next.count > 0;
    time(&next.updated_at);
    *alert = next;

    cJSON_Delete(root);
    free(response);
    return ok;
}

bool qweather_fetch_now(const char *city_id, WeatherData *weather)
{
    if (!city_id || !weather) {
        ESP_LOGW(TAG, "qweather now invalid arg");
        return false;
    }
    char encoded_location[128] = {};
    if (!url_encode_component(city_id, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "qweather now location too long");
        return false;
    }

    char url[512];
    if (!format_qweather_url(url,
                             sizeof(url),
                             "now",
                             "https://%s/v7/weather/now?location=%s&lang=zh&unit=m",
                             qweather_api_host(),
                             encoded_location)) {
        return false;
    }
    ESP_LOGI(TAG, "qweather now lookup: %s via %s", city_id, qweather_api_host());
    char *response = alloc_qweather_response("now", kQweatherNowResponseBufferSize);
    if (!response) {
        return false;
    }
    if (http_get_text(url, response, kQweatherNowResponseBufferSize, g_weather_api_key) != ESP_OK) {
        ESP_LOGW(TAG, "qweather now http failed");
        free(response);
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather now", response);
        free(response);
        return false;
    }
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *now = cJSON_GetObjectItem(root, "now");
    if (cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 && now) {
        ok = json_copy_string(now, "text", weather->text, sizeof(weather->text)) &&
             json_copy_string(now, "icon", weather->icon, sizeof(weather->icon)) &&
             json_copy_string(now, "temp", weather->temp, sizeof(weather->temp)) &&
             json_copy_string(now, "humidity", weather->humidity, sizeof(weather->humidity));
    } else {
        ESP_LOGW(TAG, "qweather now failed code=%s",
                 cJSON_IsString(code) ? code->valuestring : "missing");
    }
    cJSON_Delete(root);
    free(response);
    return ok;
}

static int weather_text_to_int(const char *text, int fallback = 0)
{
    return text && text[0] ? atoi(text) : fallback;
}

static void build_weather_advice(WeatherForecastData *forecast)
{
    if (!forecast || forecast->count <= 0 || !forecast->days[0].valid) {
        return;
    }
    const WeatherForecastDay &today = forecast->days[0];
    int temp_max = weather_text_to_int(today.temp_max);
    int temp_min = weather_text_to_int(today.temp_min, temp_max);
    const char *text = today.text;
    if (text && (strstr(text, "雨") || strstr(text, "雪"))) {
        strlcpy(forecast->advice, "有雨雪，出门记得带伞。", sizeof(forecast->advice));
    } else if (temp_max >= 30) {
        strlcpy(forecast->advice, "天气较热，注意防晒补水。", sizeof(forecast->advice));
    } else if (temp_min <= 8) {
        strlcpy(forecast->advice, "气温偏低，注意保暖。", sizeof(forecast->advice));
    } else if (temp_max - temp_min >= 10) {
        strlcpy(forecast->advice, "早晚温差大，建议备外套。", sizeof(forecast->advice));
    } else {
        strlcpy(forecast->advice, "天气平稳，适合轻装出行。", sizeof(forecast->advice));
    }
}

static bool qweather_fetch_daily_days(const char *city_id, int days, WeatherForecastData *forecast)
{
    if (!city_id || !forecast || (days != 3 && days != 7)) {
        ESP_LOGW(TAG, "qweather daily invalid arg");
        return false;
    }
    char encoded_location[128] = {};
    if (!url_encode_component(city_id, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "qweather daily location too long");
        return false;
    }

    char url[512];
    if (!format_qweather_url(url,
                             sizeof(url),
                             "daily",
                             "https://%s/v7/weather/%dd?location=%s&lang=zh&unit=m",
                             qweather_api_host(),
                             days,
                             encoded_location)) {
        return false;
    }
    ESP_LOGI(TAG, "qweather daily lookup: %s %dd via %s", city_id, days, qweather_api_host());
    char *response = alloc_qweather_response("daily", kQweatherDailyResponseBufferSize);
    if (!response) {
        return false;
    }
    esp_err_t http_err = http_get_text(url, response, kQweatherDailyResponseBufferSize, g_weather_api_key);
    if (http_err != ESP_OK) {
        ESP_LOGW(TAG, "qweather daily http failed err=%s", esp_err_to_name(http_err));
        free(response);
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather daily", response);
        free(response);
        return false;
    }

    WeatherForecastData next = {};
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 && cJSON_IsArray(daily)) {
        int count = cJSON_GetArraySize(daily);
        if (count > kWeatherForecastDays) {
            count = kWeatherForecastDays;
        }
        for (int i = 0; i < count; ++i) {
            cJSON *item = cJSON_GetArrayItem(daily, i);
            if (!item) {
                continue;
            }
            WeatherForecastDay &day = next.days[next.count];
            json_copy_string(item, "fxDate", day.date, sizeof(day.date));
            json_copy_string(item, "textDay", day.text, sizeof(day.text));
            json_copy_string(item, "iconDay", day.icon, sizeof(day.icon));
            json_copy_string(item, "tempMax", day.temp_max, sizeof(day.temp_max));
            json_copy_string(item, "tempMin", day.temp_min, sizeof(day.temp_min));
            json_copy_string(item, "humidity", day.humidity, sizeof(day.humidity));
            json_copy_string(item, "windDirDay", day.wind_dir, sizeof(day.wind_dir));
            json_copy_string(item, "windScaleDay", day.wind_scale, sizeof(day.wind_scale));
            json_copy_string(item, "sunrise", day.sunrise, sizeof(day.sunrise));
            json_copy_string(item, "sunset", day.sunset, sizeof(day.sunset));
            day.valid = day.date[0] != '\0' &&
                        (day.text[0] != '\0' || day.temp_max[0] != '\0' || day.temp_min[0] != '\0');
            if (day.valid) {
                ++next.count;
            }
        }
        next.ready = next.count > 0;
        if (next.ready) {
            time(&next.updated_at);
            build_weather_advice(&next);
            *forecast = next;
            ok = true;
        }
    } else {
        ESP_LOGW(TAG, "qweather daily failed code=%s",
                 cJSON_IsString(code) ? code->valuestring : "missing");
    }
    cJSON_Delete(root);
    free(response);
    return ok;
}

bool qweather_fetch_daily(const char *city_id, WeatherForecastData *forecast)
{
    if (qweather_fetch_daily_days(city_id, 7, forecast)) {
        return true;
    }
    return qweather_fetch_daily_days(city_id, 3, forecast);
}

bool qweather_fetch_air(const char *city_id, WeatherAirData *air)
{
    if (!city_id || !air) {
        ESP_LOGW(TAG, "qweather air invalid arg");
        return false;
    }
    char encoded_location[128] = {};
    if (!url_encode_component(city_id, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "qweather air location too long");
        return false;
    }

    char url[512];
    if (!format_qweather_url(url,
                             sizeof(url),
                             "air",
                             "https://%s/v7/air/now?location=%s&lang=zh",
                             qweather_api_host(),
                             encoded_location)) {
        return false;
    }
    ESP_LOGI(TAG, "qweather air lookup: %s via %s", city_id, qweather_api_host());
    char *response = alloc_qweather_response("air", kQweatherAirResponseBufferSize);
    if (!response) {
        return false;
    }
    esp_err_t http_err = http_get_text(url, response, kQweatherAirResponseBufferSize, g_weather_api_key);
    if (http_err != ESP_OK) {
        ESP_LOGW(TAG, "qweather air http failed err=%s", esp_err_to_name(http_err));
        free(response);
        return false;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        log_response_preview("qweather air", response);
        free(response);
        return false;
    }
    WeatherAirData next = {};
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *now = cJSON_GetObjectItem(root, "now");
    if (cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 && now) {
        ok = json_copy_string(now, "aqi", next.aqi, sizeof(next.aqi)) &&
             json_copy_string(now, "category", next.category, sizeof(next.category));
        json_copy_string(now, "primary", next.primary, sizeof(next.primary));
        json_copy_string(now, "pm2p5", next.pm2p5, sizeof(next.pm2p5));
        next.ready = ok;
        if (ok) {
            time(&next.updated_at);
            *air = next;
        }
    } else {
        ESP_LOGW(TAG, "qweather air failed code=%s",
                 cJSON_IsString(code) ? code->valuestring : "missing");
    }
    cJSON_Delete(root);
    free(response);
    return ok;
}

void get_weather_snapshot(WeatherData *weather, WeatherAlertData *alert)
{
    portENTER_CRITICAL(&g_weather_state_mux);
    if (weather) {
        *weather = g_weather;
    }
    if (alert) {
        *alert = g_weather_alert;
    }
    portEXIT_CRITICAL(&g_weather_state_mux);
}

void get_weather_forecast_snapshot(WeatherForecastData *forecast)
{
    if (!forecast) {
        return;
    }
    portENTER_CRITICAL(&g_weather_state_mux);
    *forecast = g_weather_forecast;
    portEXIT_CRITICAL(&g_weather_state_mux);
}

void get_weather_air_snapshot(WeatherAirData *air)
{
    if (!air) {
        return;
    }
    portENTER_CRITICAL(&g_weather_state_mux);
    *air = g_weather_air;
    portEXIT_CRITICAL(&g_weather_state_mux);
}

bool perform_weather_update()
{
    if (!g_have_weather_key || g_low_battery_mode) {
        xEventGroupClearBits(g_app_events, kWeatherReadyBit);
        return false;
    }

    char location[32] = {};
    char city_id[24] = {};
    char ip_city[32] = {};
    char lookup_city[32] = {};
    WeatherData next = {};
    if (ip_geolocation_lookup(location, sizeof(location), ip_city, sizeof(ip_city))) {
        trim_ascii(location);
        bool have_city_id = qweather_lookup_city(location,
                                                 city_id,
                                                 sizeof(city_id),
                                                 lookup_city,
                                                 sizeof(lookup_city),
                                                 next.lat,
                                                 sizeof(next.lat),
                                                 next.lon,
                                                 sizeof(next.lon));
        if (!have_city_id && ip_city[0] != '\0') {
            ESP_LOGW(TAG, "retry qweather city lookup by ip city: %s", ip_city);
            have_city_id = qweather_lookup_city(ip_city,
                                                city_id,
                                                sizeof(city_id),
                                                lookup_city,
                                                sizeof(lookup_city),
                                                next.lat,
                                                sizeof(next.lat),
                                                next.lon,
                                                sizeof(next.lon));
        }
        strlcpy(next.city, ip_city[0] ? ip_city : (lookup_city[0] ? lookup_city : location), sizeof(next.city));
        if (!have_city_id) {
            strlcpy(city_id, location, sizeof(city_id));
            char *comma = strchr(location, ',');
            if (comma) {
                size_t lon_len = comma - location;
                if (lon_len >= sizeof(next.lon)) {
                    lon_len = sizeof(next.lon) - 1;
                }
                memcpy(next.lon, location, lon_len);
                next.lon[lon_len] = '\0';
                strlcpy(next.lat, comma + 1, sizeof(next.lat));
            }
            ESP_LOGW(TAG, "using ip coordinates for weather now: %s", city_id);
        }
        if (qweather_fetch_now(city_id, &next)) {
            WeatherAlertData next_alert = {};
            WeatherForecastData next_forecast = {};
            WeatherAirData next_air = {};
            (void)qweather_fetch_alert(next.lat, next.lon, &next_alert);
            bool forecast_ok = qweather_fetch_daily(city_id, &next_forecast);
            bool air_ok = qweather_fetch_air(city_id, &next_air);
            time_t now = 0;
            time(&now);
            portENTER_CRITICAL(&g_weather_state_mux);
            g_weather = next;
            g_weather_alert = next_alert;
            if (forecast_ok) {
                g_weather_forecast = next_forecast;
            }
            if (air_ok) {
                g_weather_air = next_air;
            }
            g_last_weather_sync_time = now;
            portEXIT_CRITICAL(&g_weather_state_mux);
            xEventGroupSetBits(g_app_events, kWeatherReadyBit);
            ESP_LOGI(TAG, "weather updated: %s %s %sC %s%% icon=%s forecast=%s air=%s",
                     next.city,
                     next.text,
                     next.temp,
                     next.humidity,
                     next.icon,
                     forecast_ok ? "ok" : "cached",
                     air_ok ? "ok" : "cached");
            return true;
        } else {
            ESP_LOGW(TAG, "weather update failed after ip lookup");
        }
    } else {
        ESP_LOGW(TAG, "ip geolocation lookup failed");
    }
    return false;
}

uint32_t weather_icon_codepoint(const char *code)
{
    if (!code || code[0] == '\0') {
        return 0xF146;
    }
    int icon = atoi(code);
    if (icon >= 100 && icon <= 104) {
        return 0xF101 + (uint32_t)(icon - 100);
    }
    if (icon >= 150 && icon <= 153) {
        return 0xF106 + (uint32_t)(icon - 150);
    }
    if (icon >= 300 && icon <= 318) {
        return 0xF10A + (uint32_t)(icon - 300);
    }
    if (icon >= 350 && icon <= 351) {
        return 0xF11D + (uint32_t)(icon - 350);
    }
    if (icon == 399) {
        return 0xF11F;
    }
    if (icon >= 400 && icon <= 410) {
        return 0xF120 + (uint32_t)(icon - 400);
    }
    if (icon >= 456 && icon <= 457) {
        return 0xF12B + (uint32_t)(icon - 456);
    }
    if (icon == 499) {
        return 0xF12D;
    }
    if (icon >= 500 && icon <= 504) {
        return 0xF12E + (uint32_t)(icon - 500);
    }
    if (icon >= 507 && icon <= 515) {
        return 0xF133 + (uint32_t)(icon - 507);
    }
    if (icon >= 800 && icon <= 807) {
        return 0xF13C + (uint32_t)(icon - 800);
    }
    if (icon == 900) {
        return 0xF144;
    }
    if (icon == 901) {
        return 0xF145;
    }
    if (icon == 9999) {
        return 0xF1CB;
    }
    return 0xF146;
}

const char *weather_icon_text(const char *code)
{
    static char text[5];
    uint32_t cp = weather_icon_codepoint(code);
    if (cp <= 0x7F) {
        text[0] = (char)cp;
        text[1] = '\0';
    } else if (cp <= 0x7FF) {
        text[0] = (char)(0xC0 | (cp >> 6));
        text[1] = (char)(0x80 | (cp & 0x3F));
        text[2] = '\0';
    } else if (cp <= 0xFFFF) {
        text[0] = (char)(0xE0 | (cp >> 12));
        text[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        text[2] = (char)(0x80 | (cp & 0x3F));
        text[3] = '\0';
    } else {
        text[0] = (char)(0xF0 | (cp >> 18));
        text[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        text[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        text[3] = (char)(0x80 | (cp & 0x3F));
        text[4] = '\0';
    }
    return text;
}
