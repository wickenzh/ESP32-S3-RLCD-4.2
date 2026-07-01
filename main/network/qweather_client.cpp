// 对接 IP 定位、QWeather 城市查询、实时天气和天气预警接口。
#include "network_services.h"

#include <stdarg.h>

namespace {
constexpr const char *kQweatherApiHost = "devapi.qweather.com";
constexpr const char *kQweatherGeoApiHost = "geoapi.qweather.com";
}

const char *qweather_api_host()
{
    return kQweatherApiHost;
}

namespace {
constexpr size_t kIpGeoResponseBufferSize = 2048;
constexpr size_t kQweatherCityResponseBufferSize = 8192;
constexpr size_t kQweatherNowResponseBufferSize = 8192;
constexpr size_t kQweatherAlertResponseBufferSize = 16384;
constexpr size_t kQweatherDailyResponseBufferSize = 24576;
constexpr size_t kQweatherAirResponseBufferSize = 8192;
constexpr size_t kQweatherEncodedLocationSize = 128;
constexpr size_t kQweatherApiUrlSize = 512;
constexpr size_t kQweatherAlertUrlSize = 256;
constexpr size_t kWeatherAlertEventNameSize = 24;
constexpr size_t kWeatherAlertColorCodeSize = 16;
constexpr size_t kWeatherAlertHeadlineSize = 64;
constexpr size_t kIpRegionCopySize = 96;
constexpr size_t kWeatherLocationTextSize = 32;
constexpr size_t kQweatherCityIdSize = 24;
constexpr size_t kWeatherCityNameSize = 32;
constexpr size_t kWeatherIconUtf8TextSize = 5;
constexpr uint32_t kWeatherIconDefaultCodepoint = 0xF146;
constexpr size_t kIpRegionMaxParts = 5;
constexpr size_t kIpRegionCityPartIndex = 2;
constexpr size_t kIpRegionCityPartMinCount = kIpRegionCityPartIndex + 1;
constexpr size_t kWeatherAlertCompactTitleChars = 6;
constexpr unsigned char kUtf8AsciiMask = 0x80;
constexpr unsigned char kUtf8TwoByteMask = 0xE0;
constexpr unsigned char kUtf8TwoBytePrefix = 0xC0;
constexpr unsigned char kUtf8ThreeByteMask = 0xF0;
constexpr unsigned char kUtf8ThreeBytePrefix = 0xE0;
constexpr unsigned char kUtf8FourByteMask = 0xF8;
constexpr unsigned char kUtf8FourBytePrefix = 0xF0;
constexpr uint32_t kUtf8OneByteMaxCodepoint = 0x7F;
constexpr uint32_t kUtf8TwoByteMaxCodepoint = 0x7FF;
constexpr uint32_t kUtf8ThreeByteMaxCodepoint = 0xFFFF;
constexpr unsigned char kUtf8ContinuationPrefix = 0x80;
constexpr uint32_t kUtf8ContinuationPayloadMask = 0x3F;
constexpr int kUtf8Shift6 = 6;
constexpr int kUtf8Shift12 = 12;
constexpr int kUtf8Shift18 = 18;
constexpr size_t kUtf8OneByteLen = 1;
constexpr size_t kUtf8TwoByteLen = 2;
constexpr size_t kUtf8ThreeByteLen = 3;
constexpr size_t kUtf8FourByteLen = 4;
constexpr int kQweatherDaily3DayEndpointDays = 3;
constexpr int kQweatherDaily7DayEndpointDays = 7;
constexpr int kWeatherAdviceHotTempC = 30;
constexpr int kWeatherAdviceColdTempC = 8;
constexpr int kWeatherAdviceLargeTempGapC = 10;
constexpr const char *kWeatherAdviceRainOrSnow = "有雨雪，出门记得带伞。";
constexpr const char *kWeatherAdviceHot = "天气较热，注意防晒补水。";
constexpr const char *kWeatherAdviceCold = "气温偏低，注意保暖。";
constexpr const char *kWeatherAdviceLargeTempGap = "早晚温差大，建议备外套。";
constexpr const char *kWeatherAdviceCalm = "天气平稳，适合轻装出行。";
constexpr const char *kIpGeolocationUrl = "https://uapis.cn/api/v1/network/myip";
constexpr const char *kQweatherCityLookupUrlFormat =
    "https://geoapi.qweather.com/v2/city/lookup?location=%s&number=1&range=cn&lang=zh";
constexpr const char *kQweatherAlertUrlFormat =
    "https://%s/weatheralert/v1/current/%s/%s?lang=zh&localTime=true";
constexpr const char *kQweatherNowUrlFormat =
    "https://%s/v7/weather/now?location=%s&lang=zh&unit=m";
constexpr const char *kQweatherDailyUrlFormat =
    "https://%s/v7/weather/%dd?location=%s&lang=zh&unit=m";
constexpr const char *kQweatherAirUrlFormat =
    "https://%s/v7/air/now?location=%s&lang=zh";
constexpr const char *kIpGeoCoordinateFormat = "%.4f,%.4f";
constexpr const char *kIpRegionDelimiter = " ";
constexpr const char *kIpGeoCitySuffix = "市";
constexpr const char *kWeatherAlertSuffix = "预警";
constexpr const char *kWeatherAlertEventColorFormat = "%s%s%s";
constexpr const char *kWeatherAlertEventOnlyFormat = "%s%s";
constexpr const char *kQweatherDefaultStage = "request";
constexpr const char *kQweatherStageIpLocation = "ip location";
constexpr const char *kQweatherStageCity = "city";
constexpr const char *kQweatherStageAlert = "alert";
constexpr const char *kQweatherStageNow = "now";
constexpr const char *kQweatherStageDaily = "daily";
constexpr const char *kQweatherStageAir = "air";
constexpr const char *kQweatherPreviewCityLabel = "qweather city";
constexpr const char *kQweatherPreviewAlertLabel = "qweather alert";
constexpr const char *kQweatherPreviewNowLabel = "qweather now";
constexpr const char *kQweatherPreviewDailyLabel = "qweather daily";
constexpr const char *kQweatherPreviewAirLabel = "qweather air";
constexpr const char *kQweatherUnknownStage = "unknown";
constexpr const char *kIpGeoJsonLatitudeField = "latitude";
constexpr const char *kIpGeoJsonLongitudeField = "longitude";
constexpr const char *kIpGeoJsonRegionField = "region";
constexpr const char *kQweatherJsonCodeField = "code";
constexpr const char *kQweatherSuccessCode = "200";
constexpr const char *kQweatherMissingCodeText = "missing";
constexpr const char *kQweatherJsonLocationField = "location";
constexpr const char *kQweatherJsonIdField = "id";
constexpr const char *kQweatherJsonNameField = "name";
constexpr const char *kQweatherJsonLatField = "lat";
constexpr const char *kQweatherJsonLonField = "lon";
constexpr const char *kQweatherJsonNowField = "now";
constexpr const char *kQweatherNowJsonTextField = "text";
constexpr const char *kQweatherNowJsonIconField = "icon";
constexpr const char *kQweatherNowJsonTempField = "temp";
constexpr const char *kQweatherNowJsonHumidityField = "humidity";
constexpr const char *kQweatherAlertJsonAlertsField = "alerts";
constexpr const char *kQweatherAlertJsonEventTypeField = "eventType";
constexpr const char *kQweatherAlertJsonColorField = "color";
constexpr const char *kQweatherAlertJsonEventNameField = "name";
constexpr const char *kQweatherAlertJsonColorCodeField = "code";
constexpr const char *kQweatherAlertJsonHeadlineField = "headline";
constexpr const char *kQweatherDailyJsonDailyField = "daily";
constexpr const char *kQweatherDailyJsonDateField = "fxDate";
constexpr const char *kQweatherDailyJsonTextDayField = "textDay";
constexpr const char *kQweatherDailyJsonIconDayField = "iconDay";
constexpr const char *kQweatherDailyJsonTempMaxField = "tempMax";
constexpr const char *kQweatherDailyJsonTempMinField = "tempMin";
constexpr const char *kQweatherDailyJsonHumidityField = "humidity";
constexpr const char *kQweatherDailyJsonWindDirDayField = "windDirDay";
constexpr const char *kQweatherDailyJsonWindScaleDayField = "windScaleDay";
constexpr const char *kQweatherDailyJsonSunriseField = "sunrise";
constexpr const char *kQweatherDailyJsonSunsetField = "sunset";
constexpr const char *kQweatherAirJsonAqiField = "aqi";
constexpr const char *kQweatherAirJsonCategoryField = "category";
constexpr const char *kQweatherAirJsonPrimaryField = "primary";
constexpr const char *kQweatherAirJsonPm25Field = "pm2p5";
#define QWEATHER_URL_INVALID_ARG_FORMAT "qweather url invalid arg stage=%s"
#define QWEATHER_URL_TOO_LONG_FORMAT "qweather %s url too long"
#define QWEATHER_RESPONSE_SIZE_INVALID_FORMAT "qweather %s response size invalid"
#define QWEATHER_RESPONSE_ALLOC_FAILED_FORMAT "qweather %s response alloc failed"
constexpr const char *kIpLocationInvalidArgLog = "ip location invalid arg";
constexpr const char *kIpLocationCoordinateTooLongLog = "ip location coordinate text too long";
constexpr const char *kIpLocationMissingCoordinateLog = "ip location response missing latitude/longitude";
#define IP_LOCATION_RESOLVED_FORMAT "ip location resolved: %s city=%s"
constexpr const char *kQweatherCityInvalidArgLog = "qweather city invalid arg";
constexpr const char *kQweatherCityLocationTooLongLog = "qweather city location too long";
constexpr const char *kQweatherCityHttpFailedLog = "qweather city lookup http failed";
#define QWEATHER_CITY_LOOKUP_FORMAT "qweather city lookup: %s via %s"
#define QWEATHER_CITY_RESOLVED_FORMAT "qweather city resolved: %s id=%s"
#define QWEATHER_CITY_LOOKUP_FAILED_FORMAT "qweather city lookup failed code=%s"
constexpr const char *kQweatherAlertInvalidArgLog = "qweather alert invalid arg";
constexpr const char *kQweatherAlertHttpFailedLog = "qweather alert http failed";
#define QWEATHER_ALERT_LOOKUP_FORMAT "qweather alert lookup: %s,%s via %s"
constexpr const char *kQweatherNowInvalidArgLog = "qweather now invalid arg";
constexpr const char *kQweatherNowLocationTooLongLog = "qweather now location too long";
constexpr const char *kQweatherNowHttpFailedLog = "qweather now http failed";
#define QWEATHER_NOW_LOOKUP_FORMAT "qweather now lookup: %s via %s"
#define QWEATHER_NOW_FAILED_FORMAT "qweather now failed code=%s"
constexpr const char *kQweatherDailyInvalidArgLog = "qweather daily invalid arg";
constexpr const char *kQweatherDailyLocationTooLongLog = "qweather daily location too long";
#define QWEATHER_DAILY_LOOKUP_FORMAT "qweather daily lookup: %s %dd via %s"
#define QWEATHER_DAILY_HTTP_FAILED_FORMAT "qweather daily http failed err=%s"
#define QWEATHER_DAILY_FAILED_FORMAT "qweather daily failed code=%s"
constexpr const char *kQweatherAirInvalidArgLog = "qweather air invalid arg";
constexpr const char *kQweatherAirLocationTooLongLog = "qweather air location too long";
#define QWEATHER_AIR_LOOKUP_FORMAT "qweather air lookup: %s via %s"
#define QWEATHER_AIR_HTTP_FAILED_FORMAT "qweather air http failed err=%s"
#define QWEATHER_AIR_FAILED_FORMAT "qweather air failed code=%s"
#define WEATHER_UPDATED_LOG_FORMAT "weather updated: %s %s %sC %s%% icon=%s forecast=%s air=%s"
constexpr const char *kWeatherFetchStatusOk = "ok";
constexpr const char *kWeatherFetchStatusCached = "cached";
#define WEATHER_UPDATE_MANUAL_CITY_FORMAT "weather update using manual city: %s"
#define WEATHER_MANUAL_CITY_LOOKUP_FAILED_FORMAT "manual weather city lookup failed: %s"
#define WEATHER_MANUAL_CITY_UPDATE_FAILED_FORMAT "weather update failed for manual city: %s"
#define WEATHER_RETRY_IP_CITY_LOOKUP_FORMAT "retry qweather city lookup by ip city: %s"
#define WEATHER_USING_IP_COORDINATES_FORMAT "using ip coordinates for weather now: %s"
constexpr const char *kWeatherIpLookupUpdateFailedLog = "weather update failed after ip lookup";
constexpr const char *kWeatherIpGeolocationLookupFailedLog = "ip geolocation lookup failed";

struct WarningColorInfo {
    const char *code;
    const char *name;
    const char *short_name;
    int rank;
};

constexpr WarningColorInfo kWarningColors[] = {
    {"blue", "蓝色", "蓝", 2},
    {"yellow", "黄色", "黄", 3},
    {"orange", "橙色", "橙", 4},
    {"red", "红色", "红", 5},
    {"white", "白色", "白", 1},
    {"black", "黑色", "黑", 1},
};

const char *qweather_stage_text(const char *stage)
{
    return stage ? stage : kQweatherDefaultStage;
}

void copy_ip_city_without_suffix(char *out, size_t out_len, const char *city_part)
{
    if (!out || out_len == 0) {
        return;
    }
    strlcpy(out, city_part ? city_part : "", out_len);
    size_t len = strlen(out);
    size_t suffix_len = strlen(kIpGeoCitySuffix);
    if (len >= suffix_len && strcmp(out + len - suffix_len, kIpGeoCitySuffix) == 0) {
        out[len - suffix_len] = '\0';
    }
}

bool format_qweather_url(char *out, size_t out_len, const char *stage, const char *fmt, ...)
{
    if (!out || out_len == 0 || !fmt) {
        ESP_LOGW(TAG, QWEATHER_URL_INVALID_ARG_FORMAT, stage ? stage : kQweatherUnknownStage);
        return false;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out, out_len, fmt, args);
    va_end(args);
    if (written < 0 || written >= (int)out_len) {
        out[0] = '\0';
        ESP_LOGW(TAG, QWEATHER_URL_TOO_LONG_FORMAT, qweather_stage_text(stage));
        return false;
    }
    return true;
}

char *alloc_qweather_response(const char *stage, size_t buffer_size)
{
    if (buffer_size == 0) {
        ESP_LOGW(TAG, QWEATHER_RESPONSE_SIZE_INVALID_FORMAT, qweather_stage_text(stage));
        return nullptr;
    }
    char *response = (char *)malloc(buffer_size);
    if (!response) {
        ESP_LOGW(TAG, QWEATHER_RESPONSE_ALLOC_FAILED_FORMAT, qweather_stage_text(stage));
        return nullptr;
    }
    response[0] = '\0';
    return response;
}

class QweatherResponseBuffer {
public:
    QweatherResponseBuffer(const char *stage, size_t buffer_size)
        : data_(alloc_qweather_response(stage, buffer_size)),
          size_(buffer_size)
    {
    }

    ~QweatherResponseBuffer()
    {
        free(data_);
    }

    QweatherResponseBuffer(const QweatherResponseBuffer &) = delete;
    QweatherResponseBuffer &operator=(const QweatherResponseBuffer &) = delete;

    char *get() const
    {
        return data_;
    }

    size_t size() const
    {
        return size_;
    }

    explicit operator bool() const
    {
        return data_ != nullptr;
    }

private:
    char *data_;
    size_t size_;
};

class QweatherJsonRoot {
public:
    explicit QweatherJsonRoot(char *response)
        : root_(cJSON_Parse(response))
    {
    }

    ~QweatherJsonRoot()
    {
        cJSON_Delete(root_);
    }

    QweatherJsonRoot(const QweatherJsonRoot &) = delete;
    QweatherJsonRoot &operator=(const QweatherJsonRoot &) = delete;

    cJSON *get() const
    {
        return root_;
    }

    explicit operator bool() const
    {
        return root_ != nullptr;
    }

private:
    cJSON *root_;
};

bool qweather_code_ok(const cJSON *code)
{
    return cJSON_IsString(code) && strcmp(code->valuestring, kQweatherSuccessCode) == 0;
}

const char *qweather_code_text(const cJSON *code)
{
    return cJSON_IsString(code) ? code->valuestring : kQweatherMissingCodeText;
}

const WarningColorInfo *find_warning_color(const char *code)
{
    if (!code) {
        return nullptr;
    }
    for (const WarningColorInfo &color : kWarningColors) {
        if (strcmp(code, color.code) == 0) {
            return &color;
        }
    }
    return nullptr;
}
} // namespace

bool ip_geolocation_lookup(char *location, size_t location_len, char *city, size_t city_len)
{
    if (!location || location_len == 0 || !city || city_len == 0) {
        ESP_LOGW(TAG, "%s", kIpLocationInvalidArgLog);
        return false;
    }
    QweatherResponseBuffer response(kQweatherStageIpLocation, kIpGeoResponseBufferSize);
    if (!response) {
        return false;
    }
    if (http_get_text(kIpGeolocationUrl, response.get(), response.size()) != ESP_OK) {
        return false;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        return false;
    }
    bool ok = false;
    cJSON *lat = cJSON_GetObjectItem(root.get(), kIpGeoJsonLatitudeField);
    cJSON *lon = cJSON_GetObjectItem(root.get(), kIpGeoJsonLongitudeField);
    cJSON *region = cJSON_GetObjectItem(root.get(), kIpGeoJsonRegionField);
    if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        int written = snprintf(location, location_len, kIpGeoCoordinateFormat, lon->valuedouble, lat->valuedouble);
        if (written < 0 || written >= (int)location_len) {
            location[0] = '\0';
            ESP_LOGW(TAG, "%s", kIpLocationCoordinateTooLongLog);
            return false;
        }
        if (cJSON_IsString(region) && region->valuestring) {
            char region_copy[kIpRegionCopySize] = {};
            strlcpy(region_copy, region->valuestring, sizeof(region_copy));
            char *parts[kIpRegionMaxParts] = {};
            size_t count = 0;
            char *token = strtok(region_copy, kIpRegionDelimiter);
            while (token && count < kIpRegionMaxParts) {
                if (token[0] != '\0') {
                    parts[count++] = token;
                }
                token = strtok(nullptr, kIpRegionDelimiter);
            }
            const char *city_part = count >= kIpRegionCityPartMinCount
                                        ? parts[kIpRegionCityPartIndex]
                                        : (count > 0 ? parts[count - 1] : "");
            copy_ip_city_without_suffix(city, city_len, city_part);
        }
        if (city[0] == '\0') {
            strlcpy(city, location, city_len);
        }
        ESP_LOGI(TAG, IP_LOCATION_RESOLVED_FORMAT, location, city);
        ok = true;
    } else {
        ESP_LOGW(TAG, "%s", kIpLocationMissingCoordinateLog);
    }
    return ok;
}

QweatherCityLookupStatus qweather_lookup_city_status(const char *location,
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
        ESP_LOGW(TAG, "%s", kQweatherCityInvalidArgLog);
        return kQweatherCityLookupError;
    }
    char encoded_location[kQweatherEncodedLocationSize] = {};
    if (!url_encode_component(location, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "%s", kQweatherCityLocationTooLongLog);
        return kQweatherCityLookupNotFound;
    }

    char url[kQweatherApiUrlSize];
    if (!format_qweather_url(url,
                             sizeof(url),
                             kQweatherStageCity,
                             kQweatherCityLookupUrlFormat,
                             encoded_location)) {
        return kQweatherCityLookupError;
    }
    ESP_LOGI(TAG, QWEATHER_CITY_LOOKUP_FORMAT, location, kQweatherGeoApiHost);
    QweatherResponseBuffer response(kQweatherStageCity, kQweatherCityResponseBufferSize);
    if (!response) {
        return kQweatherCityLookupError;
    }
    if (http_get_text(url, response.get(), response.size(), g_weather_api_key) != ESP_OK) {
        ESP_LOGW(TAG, "%s", kQweatherCityHttpFailedLog);
        return kQweatherCityLookupError;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewCityLabel, response.get());
        return kQweatherCityLookupError;
    }
    bool ok = false;
    QweatherCityLookupStatus status = kQweatherCityLookupNotFound;
    cJSON *code = cJSON_GetObjectItem(root.get(), kQweatherJsonCodeField);
    cJSON *locations = cJSON_GetObjectItem(root.get(), kQweatherJsonLocationField);
    cJSON *first = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : nullptr;
    if (qweather_code_ok(code) && cJSON_IsObject(first)) {
        ok = json_copy_string(first, kQweatherJsonIdField, city_id, city_id_len) &&
             json_copy_string(first, kQweatherJsonNameField, city_name, city_name_len);
        if (ok) {
            if (lat_out && lat_len > 0) {
                json_copy_string(first, kQweatherJsonLatField, lat_out, lat_len);
            }
            if (lon_out && lon_len > 0) {
                json_copy_string(first, kQweatherJsonLonField, lon_out, lon_len);
            }
            ESP_LOGI(TAG, QWEATHER_CITY_RESOLVED_FORMAT, city_name, city_id);
        }
        status = ok ? kQweatherCityLookupOk : kQweatherCityLookupError;
    } else {
        ESP_LOGW(TAG, QWEATHER_CITY_LOOKUP_FAILED_FORMAT, qweather_code_text(code));
    }
    return status;
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
    return qweather_lookup_city_status(location,
                                       city_id,
                                       city_id_len,
                                       city_name,
                                       city_name_len,
                                       lat_out,
                                       lat_len,
                                       lon_out,
                                       lon_len) == kQweatherCityLookupOk;
}

static bool lookup_weather_city(const char *location,
                                char *city_id,
                                char *city_name,
                                WeatherData *weather)
{
    if (!city_id || !city_name || !weather) {
        return false;
    }
    return qweather_lookup_city(location,
                                city_id,
                                kQweatherCityIdSize,
                                city_name,
                                kWeatherCityNameSize,
                                weather->lat,
                                sizeof(weather->lat),
                                weather->lon,
                                sizeof(weather->lon));
}

const char *warning_color_name(const char *code)
{
    const WarningColorInfo *color = find_warning_color(code);
    return color ? color->name : "";
}

int warning_color_rank(const char *code)
{
    const WarningColorInfo *color = find_warning_color(code);
    return color ? color->rank : 0;
}

static size_t alert_utf8_char_len(unsigned char ch)
{
    if ((ch & kUtf8AsciiMask) == 0) {
        return kUtf8OneByteLen;
    }
    if ((ch & kUtf8TwoByteMask) == kUtf8TwoBytePrefix) {
        return kUtf8TwoByteLen;
    }
    if ((ch & kUtf8ThreeByteMask) == kUtf8ThreeBytePrefix) {
        return kUtf8ThreeByteLen;
    }
    if ((ch & kUtf8FourByteMask) == kUtf8FourBytePrefix) {
        return kUtf8FourByteLen;
    }
    return kUtf8OneByteLen;
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
    if (alert_utf8_char_count(title) <= kWeatherAlertCompactTitleChars) {
        return;
    }
    replace_all(title, title_len, kWeatherAlertSuffix, "");
    for (const WarningColorInfo &color : kWarningColors) {
        replace_all(title, title_len, color.name, color.short_name);
    }
    if (alert_utf8_char_count(title) > kWeatherAlertCompactTitleChars) {
        char clipped[kWeatherAlertTitleLen] = {};
        alert_utf8_copy_chars(clipped, sizeof(clipped), title, kWeatherAlertCompactTitleChars);
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

static void build_weather_alert_title(char *title,
                                      size_t title_len,
                                      const char *event_name,
                                      const char *color_code,
                                      const char *headline)
{
    if (!title || title_len == 0) {
        return;
    }
    title[0] = '\0';
    const char *color_name = warning_color_name(color_code);
    if (event_name && event_name[0] != '\0' && color_name[0] != '\0') {
        snprintf(title, title_len, kWeatherAlertEventColorFormat, event_name, color_name, kWeatherAlertSuffix);
    } else if (headline && headline[0] != '\0') {
        strlcpy(title, headline, title_len);
    } else if (event_name && event_name[0] != '\0') {
        snprintf(title, title_len, kWeatherAlertEventOnlyFormat, event_name, kWeatherAlertSuffix);
    }
}

bool qweather_fetch_alert(const char *lat, const char *lon, WeatherAlertData *alert)
{
    if (!alert) {
        ESP_LOGW(TAG, "%s", kQweatherAlertInvalidArgLog);
        return false;
    }
    if (!lat || !lon || lat[0] == '\0' || lon[0] == '\0') {
        alert->active = false;
        return true;
    }

    const char *host = qweather_api_host();
    char url[kQweatherAlertUrlSize];
    if (!format_qweather_url(url,
                             sizeof(url),
                             kQweatherStageAlert,
                             kQweatherAlertUrlFormat,
                             host,
                             lat,
                             lon)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_ALERT_LOOKUP_FORMAT, lat, lon, host);
    QweatherResponseBuffer response(kQweatherStageAlert, kQweatherAlertResponseBufferSize);
    if (!response) {
        return false;
    }
    if (http_get_text(url, response.get(), response.size(), g_weather_api_key) != ESP_OK) {
        ESP_LOGW(TAG, "%s", kQweatherAlertHttpFailedLog);
        return false;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewAlertLabel, response.get());
        return false;
    }

    WeatherAlertData next = {};
    bool ok = true;
    cJSON *alerts = cJSON_GetObjectItem(root.get(), kQweatherAlertJsonAlertsField);
    int alert_count = cJSON_IsArray(alerts) ? cJSON_GetArraySize(alerts) : 0;
    for (int i = 0; i < alert_count; ++i) {
        cJSON *item = cJSON_GetArrayItem(alerts, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }
        char event_name[kWeatherAlertEventNameSize] = {};
        char color_code[kWeatherAlertColorCodeSize] = {};
        char headline[kWeatherAlertHeadlineSize] = {};
        cJSON *event = cJSON_GetObjectItem(item, kQweatherAlertJsonEventTypeField);
        cJSON *color = cJSON_GetObjectItem(item, kQweatherAlertJsonColorField);
        if (event) {
            json_copy_string(event, kQweatherAlertJsonEventNameField, event_name, sizeof(event_name));
        }
        if (color) {
            json_copy_string(color, kQweatherAlertJsonColorCodeField, color_code, sizeof(color_code));
        }
        json_copy_string(item, kQweatherAlertJsonHeadlineField, headline, sizeof(headline));

        int rank = warning_color_rank(color_code);

        char title[kWeatherAlertTitleLen] = {};
        build_weather_alert_title(title, sizeof(title), event_name, color_code, headline);
        add_weather_alert_title(&next, title, rank);
    }
    next.active = next.count > 0;
    time(&next.updated_at);
    *alert = next;

    return ok;
}

static bool parse_weather_now(cJSON *now, WeatherData *weather)
{
    if (!cJSON_IsObject(now) || !weather) {
        return false;
    }
    return json_copy_string(now, kQweatherNowJsonTextField, weather->text, sizeof(weather->text)) &&
           json_copy_string(now, kQweatherNowJsonIconField, weather->icon, sizeof(weather->icon)) &&
           json_copy_string(now, kQweatherNowJsonTempField, weather->temp, sizeof(weather->temp)) &&
           json_copy_string(now, kQweatherNowJsonHumidityField, weather->humidity, sizeof(weather->humidity));
}

bool qweather_fetch_now(const char *city_id, WeatherData *weather)
{
    if (!city_id || !weather) {
        ESP_LOGW(TAG, "%s", kQweatherNowInvalidArgLog);
        return false;
    }
    char encoded_location[kQweatherEncodedLocationSize] = {};
    if (!url_encode_component(city_id, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "%s", kQweatherNowLocationTooLongLog);
        return false;
    }

    const char *host = qweather_api_host();
    char url[kQweatherApiUrlSize];
    if (!format_qweather_url(url,
                             sizeof(url),
                             kQweatherStageNow,
                             kQweatherNowUrlFormat,
                             host,
                             encoded_location)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_NOW_LOOKUP_FORMAT, city_id, host);
    QweatherResponseBuffer response(kQweatherStageNow, kQweatherNowResponseBufferSize);
    if (!response) {
        return false;
    }
    if (http_get_text(url, response.get(), response.size(), g_weather_api_key) != ESP_OK) {
        ESP_LOGW(TAG, "%s", kQweatherNowHttpFailedLog);
        return false;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewNowLabel, response.get());
        return false;
    }
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root.get(), kQweatherJsonCodeField);
    cJSON *now = cJSON_GetObjectItem(root.get(), kQweatherJsonNowField);
    if (qweather_code_ok(code) && cJSON_IsObject(now)) {
        ok = parse_weather_now(now, weather);
    } else {
        ESP_LOGW(TAG, QWEATHER_NOW_FAILED_FORMAT, qweather_code_text(code));
    }
    return ok;
}

static int weather_text_to_int(const char *text, int fallback = 0)
{
    return text && text[0] ? atoi(text) : fallback;
}

const char *weather_advice_for_day(const WeatherForecastDay &today)
{
    int temp_max = weather_text_to_int(today.temp_max);
    int temp_min = weather_text_to_int(today.temp_min, temp_max);
    const char *text = today.text;
    if (text && (strstr(text, "雨") || strstr(text, "雪"))) {
        return kWeatherAdviceRainOrSnow;
    }
    if (temp_max >= kWeatherAdviceHotTempC) {
        return kWeatherAdviceHot;
    }
    if (temp_min <= kWeatherAdviceColdTempC) {
        return kWeatherAdviceCold;
    }
    if (temp_max - temp_min >= kWeatherAdviceLargeTempGapC) {
        return kWeatherAdviceLargeTempGap;
    }
    return kWeatherAdviceCalm;
}

static void build_weather_advice(WeatherForecastData *forecast)
{
    if (!forecast || forecast->count <= 0 || !forecast->days[0].valid) {
        return;
    }
    strlcpy(forecast->advice, weather_advice_for_day(forecast->days[0]), sizeof(forecast->advice));
}

static bool parse_weather_forecast_day(cJSON *item, WeatherForecastDay *day)
{
    if (!cJSON_IsObject(item) || !day) {
        return false;
    }
    json_copy_string(item, kQweatherDailyJsonDateField, day->date, sizeof(day->date));
    json_copy_string(item, kQweatherDailyJsonTextDayField, day->text, sizeof(day->text));
    json_copy_string(item, kQweatherDailyJsonIconDayField, day->icon, sizeof(day->icon));
    json_copy_string(item, kQweatherDailyJsonTempMaxField, day->temp_max, sizeof(day->temp_max));
    json_copy_string(item, kQweatherDailyJsonTempMinField, day->temp_min, sizeof(day->temp_min));
    json_copy_string(item, kQweatherDailyJsonHumidityField, day->humidity, sizeof(day->humidity));
    json_copy_string(item, kQweatherDailyJsonWindDirDayField, day->wind_dir, sizeof(day->wind_dir));
    json_copy_string(item, kQweatherDailyJsonWindScaleDayField, day->wind_scale, sizeof(day->wind_scale));
    json_copy_string(item, kQweatherDailyJsonSunriseField, day->sunrise, sizeof(day->sunrise));
    json_copy_string(item, kQweatherDailyJsonSunsetField, day->sunset, sizeof(day->sunset));
    day->valid = day->date[0] != '\0' &&
                 (day->text[0] != '\0' || day->temp_max[0] != '\0' || day->temp_min[0] != '\0');
    return day->valid;
}

static int weather_forecast_parse_count(cJSON *daily)
{
    int count = cJSON_GetArraySize(daily);
    return count > kWeatherForecastDays ? kWeatherForecastDays : count;
}

static bool qweather_fetch_daily_days(const char *city_id, int days, WeatherForecastData *forecast)
{
    if (!city_id || !forecast ||
        (days != kQweatherDaily3DayEndpointDays && days != kQweatherDaily7DayEndpointDays)) {
        ESP_LOGW(TAG, "%s", kQweatherDailyInvalidArgLog);
        return false;
    }
    char encoded_location[kQweatherEncodedLocationSize] = {};
    if (!url_encode_component(city_id, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "%s", kQweatherDailyLocationTooLongLog);
        return false;
    }

    const char *host = qweather_api_host();
    char url[kQweatherApiUrlSize];
    if (!format_qweather_url(url,
                             sizeof(url),
                             kQweatherStageDaily,
                             kQweatherDailyUrlFormat,
                             host,
                             days,
                             encoded_location)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_DAILY_LOOKUP_FORMAT, city_id, days, host);
    QweatherResponseBuffer response(kQweatherStageDaily, kQweatherDailyResponseBufferSize);
    if (!response) {
        return false;
    }
    esp_err_t http_err = http_get_text(url, response.get(), response.size(), g_weather_api_key);
    if (http_err != ESP_OK) {
        ESP_LOGW(TAG, QWEATHER_DAILY_HTTP_FAILED_FORMAT, esp_err_to_name(http_err));
        return false;
    }
    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewDailyLabel, response.get());
        return false;
    }

    WeatherForecastData next = {};
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root.get(), kQweatherJsonCodeField);
    cJSON *daily = cJSON_GetObjectItem(root.get(), kQweatherDailyJsonDailyField);
    if (qweather_code_ok(code) && cJSON_IsArray(daily)) {
        int count = weather_forecast_parse_count(daily);
        for (int i = 0; i < count; ++i) {
            cJSON *item = cJSON_GetArrayItem(daily, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            WeatherForecastDay &day = next.days[next.count];
            if (parse_weather_forecast_day(item, &day)) {
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
        ESP_LOGW(TAG, QWEATHER_DAILY_FAILED_FORMAT, qweather_code_text(code));
    }
    return ok;
}

bool qweather_fetch_daily(const char *city_id, WeatherForecastData *forecast)
{
    if (qweather_fetch_daily_days(city_id, kQweatherDaily7DayEndpointDays, forecast)) {
        return true;
    }
    return qweather_fetch_daily_days(city_id, kQweatherDaily3DayEndpointDays, forecast);
}

static bool parse_weather_air(cJSON *now, WeatherAirData *air)
{
    if (!cJSON_IsObject(now) || !air) {
        return false;
    }
    bool ok = json_copy_string(now, kQweatherAirJsonAqiField, air->aqi, sizeof(air->aqi)) &&
              json_copy_string(now, kQweatherAirJsonCategoryField, air->category, sizeof(air->category));
    json_copy_string(now, kQweatherAirJsonPrimaryField, air->primary, sizeof(air->primary));
    json_copy_string(now, kQweatherAirJsonPm25Field, air->pm2p5, sizeof(air->pm2p5));
    return ok;
}

bool qweather_fetch_air(const char *city_id, WeatherAirData *air)
{
    if (!city_id || !air) {
        ESP_LOGW(TAG, "%s", kQweatherAirInvalidArgLog);
        return false;
    }
    char encoded_location[kQweatherEncodedLocationSize] = {};
    if (!url_encode_component(city_id, encoded_location, sizeof(encoded_location))) {
        ESP_LOGW(TAG, "%s", kQweatherAirLocationTooLongLog);
        return false;
    }

    const char *host = qweather_api_host();
    char url[kQweatherApiUrlSize];
    if (!format_qweather_url(url,
                             sizeof(url),
                             kQweatherStageAir,
                             kQweatherAirUrlFormat,
                             host,
                             encoded_location)) {
        return false;
    }
    ESP_LOGI(TAG, QWEATHER_AIR_LOOKUP_FORMAT, city_id, host);
    QweatherResponseBuffer response(kQweatherStageAir, kQweatherAirResponseBufferSize);
    if (!response) {
        return false;
    }
    esp_err_t http_err = http_get_text(url, response.get(), response.size(), g_weather_api_key);
    if (http_err != ESP_OK) {
        ESP_LOGW(TAG, QWEATHER_AIR_HTTP_FAILED_FORMAT, esp_err_to_name(http_err));
        return false;
    }

    QweatherJsonRoot root(response.get());
    if (!root) {
        log_response_preview(kQweatherPreviewAirLabel, response.get());
        return false;
    }
    WeatherAirData next = {};
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root.get(), kQweatherJsonCodeField);
    cJSON *now = cJSON_GetObjectItem(root.get(), kQweatherJsonNowField);
    if (qweather_code_ok(code) && cJSON_IsObject(now)) {
        ok = parse_weather_air(now, &next);
        next.ready = ok;
        if (ok) {
            time(&next.updated_at);
            *air = next;
        }
    } else {
        ESP_LOGW(TAG, QWEATHER_AIR_FAILED_FORMAT, qweather_code_text(code));
    }
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

static bool fetch_and_commit_weather(const char *city_id, WeatherData *next)
{
    if (!city_id || !next) {
        return false;
    }
    if (!qweather_fetch_now(city_id, next)) {
        return false;
    }

    WeatherAlertData next_alert = {};
    WeatherForecastData next_forecast = {};
    WeatherAirData next_air = {};
    (void)qweather_fetch_alert(next->lat, next->lon, &next_alert);
    bool forecast_ok = qweather_fetch_daily(city_id, &next_forecast);
    bool air_ok = qweather_fetch_air(city_id, &next_air);
    time_t now = 0;
    time(&now);
    portENTER_CRITICAL(&g_weather_state_mux);
    g_weather = *next;
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
    ESP_LOGI(TAG, WEATHER_UPDATED_LOG_FORMAT,
             next->city,
             next->text,
             next->temp,
             next->humidity,
             next->icon,
             forecast_ok ? kWeatherFetchStatusOk : kWeatherFetchStatusCached,
             air_ok ? kWeatherFetchStatusOk : kWeatherFetchStatusCached);
    return true;
}

bool perform_weather_update()
{
    if (!g_have_weather_key || g_low_battery_mode) {
        xEventGroupClearBits(g_app_events, kWeatherReadyBit);
        return false;
    }

    char location[kWeatherLocationTextSize] = {};
    char city_id[kQweatherCityIdSize] = {};
    char ip_city[kWeatherCityNameSize] = {};
    char lookup_city[kWeatherCityNameSize] = {};
    WeatherData next = {};
    char manual_city[kManualWeatherCityLen] = {};
    if (g_has_manual_weather_city) {
        strlcpy(manual_city, g_manual_weather_city, sizeof(manual_city));
        trim_ascii(manual_city);
    }
    if (manual_city[0] != '\0') {
        ESP_LOGI(TAG, WEATHER_UPDATE_MANUAL_CITY_FORMAT, manual_city);
        bool have_city_id = lookup_weather_city(manual_city, city_id, lookup_city, &next);
        if (!have_city_id) {
            ESP_LOGW(TAG, WEATHER_MANUAL_CITY_LOOKUP_FAILED_FORMAT, manual_city);
            return false;
        }
        strlcpy(next.city, lookup_city[0] ? lookup_city : manual_city, sizeof(next.city));
        if (fetch_and_commit_weather(city_id, &next)) {
            return true;
        }
        ESP_LOGW(TAG, WEATHER_MANUAL_CITY_UPDATE_FAILED_FORMAT, manual_city);
        return false;
    }

    if (ip_geolocation_lookup(location, sizeof(location), ip_city, sizeof(ip_city))) {
        trim_ascii(location);
        bool have_city_id = lookup_weather_city(location, city_id, lookup_city, &next);
        if (!have_city_id && ip_city[0] != '\0') {
            ESP_LOGW(TAG, WEATHER_RETRY_IP_CITY_LOOKUP_FORMAT, ip_city);
            have_city_id = lookup_weather_city(ip_city, city_id, lookup_city, &next);
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
            ESP_LOGW(TAG, WEATHER_USING_IP_COORDINATES_FORMAT, city_id);
        }
        if (fetch_and_commit_weather(city_id, &next)) {
            return true;
        } else {
            ESP_LOGW(TAG, "%s", kWeatherIpLookupUpdateFailedLog);
        }
    } else {
        ESP_LOGW(TAG, "%s", kWeatherIpGeolocationLookupFailedLog);
    }
    return false;
}

uint32_t weather_icon_codepoint(const char *code)
{
    if (!code || code[0] == '\0') {
        return kWeatherIconDefaultCodepoint;
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
    return kWeatherIconDefaultCodepoint;
}

const char *weather_icon_text(const char *code)
{
    static char text[kWeatherIconUtf8TextSize];
    uint32_t cp = weather_icon_codepoint(code);
    if (cp <= kUtf8OneByteMaxCodepoint) {
        text[0] = (char)cp;
        text[1] = '\0';
    } else if (cp <= kUtf8TwoByteMaxCodepoint) {
        text[0] = (char)(kUtf8TwoBytePrefix | (cp >> kUtf8Shift6));
        text[1] = (char)(kUtf8ContinuationPrefix | (cp & kUtf8ContinuationPayloadMask));
        text[2] = '\0';
    } else if (cp <= kUtf8ThreeByteMaxCodepoint) {
        text[0] = (char)(kUtf8ThreeBytePrefix | (cp >> kUtf8Shift12));
        text[1] = (char)(kUtf8ContinuationPrefix | ((cp >> kUtf8Shift6) & kUtf8ContinuationPayloadMask));
        text[2] = (char)(kUtf8ContinuationPrefix | (cp & kUtf8ContinuationPayloadMask));
        text[3] = '\0';
    } else {
        text[0] = (char)(kUtf8FourBytePrefix | (cp >> kUtf8Shift18));
        text[1] = (char)(kUtf8ContinuationPrefix | ((cp >> kUtf8Shift12) & kUtf8ContinuationPayloadMask));
        text[2] = (char)(kUtf8ContinuationPrefix | ((cp >> kUtf8Shift6) & kUtf8ContinuationPayloadMask));
        text[3] = (char)(kUtf8ContinuationPrefix | (cp & kUtf8ContinuationPayloadMask));
        text[4] = '\0';
    }
    return text;
}
