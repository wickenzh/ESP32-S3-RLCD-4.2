// 提供 HTTPS 文本请求、gzip 解码、URL 编码和 JSON 字段读取工具。
#include "network_services.h"

#include "qweather_ca.h"

namespace {
constexpr uint8_t kGzipMagic0 = 0x1F;
constexpr uint8_t kGzipMagic1 = 0x8B;
constexpr uint8_t kGzipDeflateMethod = 8;
constexpr uint8_t kGzipFlagHeaderCrc = 0x02;
constexpr uint8_t kGzipFlagExtra = 0x04;
constexpr uint8_t kGzipFlagName = 0x08;
constexpr uint8_t kGzipFlagComment = 0x10;
constexpr size_t kGzipMinSize = 18;
constexpr size_t kGzipBaseHeaderSize = 10;
constexpr size_t kGzipTrailerSize = 8;
constexpr size_t kGzipExtraLengthFieldSize = 2;
constexpr size_t kGzipMagicPrefixSize = 2;
constexpr size_t kGzipHeaderProbeSize = 3;
constexpr int kHttpStatusOkMin = 200;
constexpr int kHttpStatusOkMax = 300;
constexpr size_t kHttpPreviewMaxChars = 120;
constexpr size_t kCStringTerminatorSize = 1;
constexpr size_t kHttpPreviewBufferSize = kHttpPreviewMaxChars + kCStringTerminatorSize;
constexpr size_t kUrlEncodedPlainCharSize = 1;
constexpr size_t kUrlEncodedEscapedCharSize = 3;
constexpr const char *kUrlHexDigits = "0123456789ABCDEF";
constexpr const char *kHttpAcceptHeaderName = "Accept";
constexpr const char *kHttpAcceptHeader = "application/json,text/plain,*/*";
constexpr const char *kHttpAcceptEncodingHeaderName = "Accept-Encoding";
constexpr const char *kHttpAcceptEncodingHeader = "identity";
constexpr const char *kQweatherApiKeyHeader = "X-QW-Api-Key";
constexpr const char *kQweatherGeoHost = "://geoapi.qweather.com/";
constexpr const char *kQweatherDevHost = "://devapi.qweather.com/";
constexpr const char *kHttpPreviewDefaultStage = "http";
constexpr const char *kHttpDecodeInvalidArgLog = "decode http body invalid arg";
constexpr const char *kHttpGetInvalidArgLog = "http get invalid arg";
constexpr const char *kHttpBootBudgetExhaustedLog = "http get skipped: boot sync time budget exhausted";
constexpr const char *kHttpClientInitFailedLog = "http client init failed";
#define HTTP_TEMP_BUFFER_ALLOC_FAILED_FORMAT "http temp buffer alloc failed len=%u"
#define HTTP_GZIP_HEADER_INVALID_FORMAT "gzip response header invalid len=%u"
#define HTTP_GZIP_DECOMPRESS_FAILED_FORMAT "gzip response decompress failed payload_len=%u"
#define HTTP_GZIP_DECOMPRESSED_FORMAT "gzip response decompressed len=%u"
#define HTTP_PARSE_EMPTY_RESPONSE_FORMAT "%s parse failed: empty response pointer"
#define HTTP_PARSE_FAILED_FORMAT "%s parse failed len=%u head=%02x %02x %02x %02x body=%s"
#define HTTP_GET_FAILED_WITH_BODY_FORMAT "http get failed status=%d err=%s body=%s"
#define HTTP_GET_FAILED_FORMAT "http get failed status=%d err=%s"
#define HTTP_RESPONSE_TRUNCATED_FORMAT "http response may be truncated status=%d content_len=%lld buffer=%u"
#define HTTP_GET_OK_FORMAT "http get ok status=%d len=%u gzip=%d"

bool is_qweather_url(const char *url)
{
    return url &&
           (strstr(url, kQweatherGeoHost) ||
            strstr(url, kQweatherDevHost));
}

bool has_gzip_magic_prefix(const char *data, size_t len)
{
    return data && len >= kGzipMagicPrefixSize &&
           (uint8_t)data[0] == kGzipMagic0 &&
           (uint8_t)data[1] == kGzipMagic1;
}

bool http_status_ok(int status)
{
    return status >= kHttpStatusOkMin && status < kHttpStatusOkMax;
}

bool http_response_may_be_truncated(int64_t content_length, size_t received_len, size_t out_len)
{
    bool content_length_fills_buffer = content_length >= 0 && (uint64_t)content_length >= out_len;
    bool received_fills_buffer = received_len + kCStringTerminatorSize >= out_len;
    return content_length_fills_buffer || received_fills_buffer;
}

bool http_ascii_space(char ch)
{
    return isspace((unsigned char)ch);
}

bool advance_gzip_pos(size_t *pos, size_t amount, size_t len)
{
    if (!pos || amount > len || *pos > len - amount) {
        return false;
    }
    *pos += amount;
    return true;
}

bool skip_gzip_zero_terminated_field(const uint8_t *data, size_t len, size_t *pos)
{
    if (!data || !pos) {
        return false;
    }
    while (*pos < len && data[*pos] != 0) {
        ++(*pos);
    }
    return advance_gzip_pos(pos, 1, len);
}

void copy_log_preview(char *out, size_t out_len, const char *text)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!text) {
        out[0] = '\0';
        return;
    }
    strlcpy(out, text, out_len);
    for (char *p = out; *p; ++p) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            *p = ' ';
        }
    }
}

class HttpByteBuffer {
public:
    explicit HttpByteBuffer(size_t len)
        : data_((uint8_t *)malloc(len)),
          size_(len)
    {
        if (!data_) {
            ESP_LOGW(TAG, HTTP_TEMP_BUFFER_ALLOC_FAILED_FORMAT, (unsigned)len);
        }
    }

    ~HttpByteBuffer()
    {
        free(data_);
    }

    HttpByteBuffer(const HttpByteBuffer &) = delete;
    HttpByteBuffer &operator=(const HttpByteBuffer &) = delete;

    uint8_t *get() const
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
    uint8_t *data_;
    size_t size_;
};
} // namespace

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (!evt) {
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data) {
        return ESP_OK;
    }
    HttpBuffer *buffer = (HttpBuffer *)evt->user_data;
    if (!buffer->data || buffer->cap == 0 || buffer->len >= buffer->cap) {
        return ESP_OK;
    }
    if (!evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (buffer->len + kCStringTerminatorSize >= buffer->cap) {
        return ESP_OK;
    }
    size_t room = buffer->cap - buffer->len - kCStringTerminatorSize;
    size_t event_len = (size_t)evt->data_len;
    size_t copy_len = event_len < room ? event_len : room;
    if (copy_len > 0) {
        memcpy(buffer->data + buffer->len, evt->data, copy_len);
        buffer->len += copy_len;
        buffer->data[buffer->len] = '\0';
    }
    return ESP_OK;
}

bool gzip_payload_range(const uint8_t *data, size_t len, size_t *payload_offset, size_t *payload_len)
{
    if (!data || !payload_offset || !payload_len) {
        return false;
    }
    if (len < kGzipMinSize || data[0] != kGzipMagic0 || data[1] != kGzipMagic1 || data[2] != kGzipDeflateMethod) {
        return false;
    }

    uint8_t flags = data[3];
    size_t pos = kGzipBaseHeaderSize;
    if (flags & kGzipFlagExtra) {
        if (pos + kGzipExtraLengthFieldSize > len) return false;
        size_t extra_len = data[pos] | (data[pos + 1] << 8);
        if (!advance_gzip_pos(&pos, kGzipExtraLengthFieldSize, len) ||
            !advance_gzip_pos(&pos, extra_len, len)) {
            return false;
        }
    }
    if (flags & kGzipFlagName) {
        if (!skip_gzip_zero_terminated_field(data, len, &pos)) return false;
    }
    if (flags & kGzipFlagComment) {
        if (!skip_gzip_zero_terminated_field(data, len, &pos)) return false;
    }
    if (flags & kGzipFlagHeaderCrc) {
        if (!advance_gzip_pos(&pos, kGzipExtraLengthFieldSize, len)) return false;
    }
    if (pos + kGzipTrailerSize > len) {
        return false;
    }

    *payload_offset = pos;
    *payload_len = len - pos - kGzipTrailerSize;
    return true;
}

esp_err_t decode_http_body(char *out, size_t out_len, size_t *body_len)
{
    if (!out || out_len == 0 || !body_len) {
        ESP_LOGW(TAG, "%s", kHttpDecodeInvalidArgLog);
        return ESP_ERR_INVALID_ARG;
    }
    if (*body_len < kGzipHeaderProbeSize || !has_gzip_magic_prefix(out, *body_len)) {
        return ESP_OK;
    }

    size_t payload_offset = 0;
    size_t payload_len = 0;
    if (!gzip_payload_range((const uint8_t *)out, *body_len, &payload_offset, &payload_len)) {
        ESP_LOGW(TAG, HTTP_GZIP_HEADER_INVALID_FORMAT, (unsigned)*body_len);
        return ESP_FAIL;
    }

    HttpByteBuffer compressed(*body_len);
    if (!compressed) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(compressed.get(), out, compressed.size());

    size_t written = tinfl_decompress_mem_to_mem(out,
                                                 out_len - kCStringTerminatorSize,
                                                 compressed.get() + payload_offset,
                                                 payload_len,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (written == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        out[0] = '\0';
        *body_len = 0;
        ESP_LOGW(TAG, HTTP_GZIP_DECOMPRESS_FAILED_FORMAT, (unsigned)payload_len);
        return ESP_FAIL;
    }

    out[written] = '\0';
    *body_len = written;
    ESP_LOGI(TAG, HTTP_GZIP_DECOMPRESSED_FORMAT, (unsigned)written);
    return ESP_OK;
}

esp_err_t http_get_text(const char *url, char *out, size_t out_len, const char *api_key)
{
    if (!url || url[0] == '\0' || !out || out_len == 0) {
        ESP_LOGW(TAG, "%s", kHttpGetInvalidArgLog);
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    HttpBuffer buffer = {out, 0, out_len};
    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.user_data = &buffer;
    int timeout_ms = g_http_timeout_ms;
    int remaining_ms = boot_sync_remaining_ms();
    if (remaining_ms <= 0) {
        ESP_LOGW(TAG, "%s", kHttpBootBudgetExhaustedLog);
        return ESP_ERR_TIMEOUT;
    }
    if (remaining_ms != INT32_MAX && timeout_ms > remaining_ms) {
        timeout_ms = remaining_ms;
    }
    config.timeout_ms = timeout_ms;
    if (is_qweather_url(url)) {
        config.cert_pem = kQweatherCaDvR36Pem;
    } else {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "%s", kHttpClientInitFailedLog);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, kHttpAcceptHeaderName, kHttpAcceptHeader);
    esp_http_client_set_header(client, kHttpAcceptEncodingHeaderName, kHttpAcceptEncodingHeader);
    if (api_key && api_key[0] != '\0') {
        esp_http_client_set_header(client, kQweatherApiKeyHeader, api_key);
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || !http_status_ok(status)) {
        if (buffer.len > 0) {
            char preview[kHttpPreviewBufferSize] = {};
            copy_log_preview(preview, sizeof(preview), out);
            ESP_LOGW(TAG, HTTP_GET_FAILED_WITH_BODY_FORMAT, status, esp_err_to_name(err), preview);
        } else {
            ESP_LOGW(TAG, HTTP_GET_FAILED_FORMAT, status, esp_err_to_name(err));
        }
        return err == ESP_OK ? ESP_FAIL : err;
    }
    if (http_response_may_be_truncated(content_length, buffer.len, out_len)) {
        ESP_LOGW(TAG, HTTP_RESPONSE_TRUNCATED_FORMAT,
                 status,
                 (long long)content_length,
                 (unsigned)out_len);
    }
    ESP_LOGI(TAG, HTTP_GET_OK_FORMAT,
             status,
             (unsigned)buffer.len,
             has_gzip_magic_prefix(out, buffer.len));
    return decode_http_body(out, out_len, &buffer.len);
}

void trim_ascii(char *text)
{
    if (!text) {
        return;
    }
    size_t len = strlen(text);
    while (len > 0 && http_ascii_space(text[len - 1])) {
        text[--len] = '\0';
    }
    char *start = text;
    while (*start && http_ascii_space(*start)) {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + kCStringTerminatorSize);
    }
}

bool json_copy_string(cJSON *obj, const char *name, char *out, size_t out_len)
{
    if (!obj || !name || !out || out_len == 0) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItem(obj, name);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    strlcpy(out, item->valuestring, out_len);
    return true;
}

bool url_is_unreserved(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
           ch == '.' || ch == '~';
}

bool url_encode_component(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) {
        return false;
    }
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        if (url_is_unreserved((char)*p)) {
            if (pos + kUrlEncodedPlainCharSize >= out_len) {
                return false;
            }
            out[pos++] = (char)*p;
        } else {
            if (pos + kUrlEncodedEscapedCharSize >= out_len) {
                return false;
            }
            out[pos++] = '%';
            out[pos++] = kUrlHexDigits[*p >> 4];
            out[pos++] = kUrlHexDigits[*p & 0x0F];
        }
    }
    out[pos] = '\0';
    return true;
}

void log_response_preview(const char *stage, const char *response)
{
    const char *label = stage ? stage : kHttpPreviewDefaultStage;
    if (!response) {
        ESP_LOGW(TAG, HTTP_PARSE_EMPTY_RESPONSE_FORMAT, label);
        return;
    }
    char preview[kHttpPreviewBufferSize] = {};
    copy_log_preview(preview, sizeof(preview), response);
    size_t response_len = strlen(response);
    const unsigned char *bytes = (const unsigned char *)response;
    ESP_LOGW(TAG, HTTP_PARSE_FAILED_FORMAT,
             label,
             (unsigned)response_len,
             response_len > 0 ? bytes[0] : 0,
             response_len > 1 ? bytes[1] : 0,
             response_len > 2 ? bytes[2] : 0,
             response_len > 3 ? bytes[3] : 0,
             preview);
}
