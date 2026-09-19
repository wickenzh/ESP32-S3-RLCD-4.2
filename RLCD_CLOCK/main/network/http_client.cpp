// 提供 HTTPS 文本请求、gzip 解码和响应日志预览工具。
#include "network_http_client.h"

#include "app_constexpr.h"
#include "app_metadata.h"
#include "app_text_format.h"
#include "http_response_policy.h"
#include "http_tls_retry_policy.h"
#include "http_timeout_policy.h"
#include "network_boot_sync.h"
#include "network_gzip.h"
#include "network_task_guards.h"
#include "qweather_ca.h"
#include "scoped_heap_buffer.h"
#include "scoped_http_client.h"

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"

#include <stdint.h>
#include <string.h>

namespace {
constexpr size_t kGzipHeaderProbeSize = 3;
constexpr int kHttpStatusOkMin = 200;
constexpr int kHttpStatusOkMax = 300;
constexpr size_t kHttpPreviewMaxChars = 120;
constexpr size_t kCStringTerminatorSize = 1;
constexpr size_t kHttpPreviewBufferSize = kHttpPreviewMaxChars + kCStringTerminatorSize;
constexpr const char *kHttpAcceptHeaderName = "Accept";
constexpr const char *kHttpAcceptHeader = "application/json,text/plain,*/*";
constexpr const char *kHttpAcceptEncodingHeaderName = "Accept-Encoding";
constexpr const char *kHttpAcceptEncodingHeader = "identity";
constexpr const char *kQweatherApiKeyHeader = "X-QW-Api-Key";
constexpr const char *kQweatherApiHostSuffix = ".qweatherapi.com/";
constexpr const char *kHttpPreviewDefaultStage = "http";
constexpr const char *kHttpDecodeInvalidArgLog = "decode http body invalid arg";
constexpr const char *kHttpGetInvalidArgLog = "http get invalid arg";
constexpr const char *kHttpBootBudgetExhaustedLog = "http get skipped: boot sync time budget exhausted";
constexpr const char *kHttpClientInitFailedLog = "http client init failed";
constexpr const char *kHttpTransactionLockTimeoutLog = "http transaction deferred: TLS session is busy";
constexpr const char *kHttpQweatherTlsFallbackLog =
    "qweather TLS certificate verification failed, retrying with legacy CA";
constexpr TickType_t kHttpTlsFallbackDelay = pdMS_TO_TICKS(350);

// All HTTPS/WSS activity is serialized by NetworkHttpTransactionGuard. Keep
// the relatively large ESP-IDF request descriptor out of the network task
// stack and clear borrowed pointers before releasing that transaction lock.
EXT_RAM_BSS_ATTR esp_http_client_config_t s_http_text_config_workspace;

class HttpTextConfigWorkspaceGuard {
public:
    HttpTextConfigWorkspaceGuard()
    {
        clear();
    }

    ~HttpTextConfigWorkspaceGuard()
    {
        clear();
    }

    HttpTextConfigWorkspaceGuard(const HttpTextConfigWorkspaceGuard &) = delete;
    HttpTextConfigWorkspaceGuard &operator=(const HttpTextConfigWorkspaceGuard &) = delete;

    esp_http_client_config_t &config()
    {
        return s_http_text_config_workspace;
    }

private:
    static void clear()
    {
        volatile uint8_t *bytes =
            reinterpret_cast<volatile uint8_t *>(&s_http_text_config_workspace);
        for (size_t remaining = sizeof(s_http_text_config_workspace);
             remaining > 0;
             --remaining) {
            *bytes++ = 0;
        }
    }
};

static_assert(kGzipHeaderProbeSize >= 3, "gzip header probe must cover magic and compression method");
static_assert(kHttpStatusOkMin >= 100 && kHttpStatusOkMin < kHttpStatusOkMax,
              "HTTP success lower bound must be a valid status below upper bound");
static_assert(kHttpStatusOkMax <= 600, "HTTP success upper bound must stay within valid status space");
static_assert(kCStringTerminatorSize == 1, "C string terminator reservation must be one byte");
static_assert(kHttpPreviewBufferSize == kHttpPreviewMaxChars + kCStringTerminatorSize,
              "HTTP preview buffer must include NUL terminator space");
#define HTTP_TEMP_BUFFER_ALLOC_FAILED_FORMAT "http temp buffer alloc failed len=%u"
#define HTTP_GZIP_HEADER_INVALID_FORMAT "gzip response header invalid len=%u"
#define HTTP_GZIP_DECOMPRESS_FAILED_FORMAT "gzip response decompress failed payload_len=%u"
#define HTTP_GZIP_DECOMPRESSED_FORMAT "gzip response decompressed len=%u"
#define HTTP_PARSE_EMPTY_RESPONSE_FORMAT "%s parse failed: empty response pointer"
#define HTTP_PARSE_FAILED_FORMAT "%s parse failed len=%u head=%02x %02x %02x %02x body=%s"
#define HTTP_GET_FAILED_WITH_BODY_FORMAT "http get failed status=%d err=%s body=%s"
#define HTTP_GET_FAILED_FORMAT "http get failed status=%d err=%s"
#define HTTP_RESPONSE_TRUNCATED_FORMAT \
    "http response truncated status=%d content_len=%lld received=%u buffer=%u overflow=%d"
#define HTTP_GET_OK_FORMAT "http get ok status=%d len=%u gzip=%d"
#define HTTP_SET_HEADER_FAILED_FORMAT "http set header failed name=%s err=%s"

bool is_qweather_url(const char *url)
{
    return url && strstr(url, kQweatherApiHostSuffix);
}

bool http_status_ok(int status)
{
    return status >= kHttpStatusOkMin && status < kHttpStatusOkMax;
}

bool compute_http_timeout_ms(int *timeout_ms)
{
    if (!timeout_ms) {
        return false;
    }
    int remaining_ms = boot_sync_remaining_ms();
    *timeout_ms = network_http_timeout_for_budget(remaining_ms);
    if (*timeout_ms <= 0) {
        ESP_LOGW(TAG, "%s", kHttpBootBudgetExhaustedLog);
        return false;
    }
    return true;
}

bool decode_http_body_args_valid(char *out, size_t out_len, const size_t *body_len)
{
    return app_text::output_buffer_available(out, out_len) && body_len;
}

bool http_get_text_args_valid(const char *url, char *out, size_t out_len)
{
    return cstr_nonempty(url) && app_text::output_buffer_available(out, out_len);
}

void copy_log_preview(char *out, size_t out_len, const char *text)
{
    if (!app_text::output_buffer_available(out, out_len)) {
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

esp_err_t set_http_header_checked(esp_http_client_handle_t client,
                                  const char *name,
                                  const char *value)
{
    if (!client || !cstr_nonempty(name) || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_http_client_set_header(client, name, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, HTTP_SET_HEADER_FAILED_FORMAT, name, esp_err_to_name(err));
    }
    return err;
}

esp_err_t configure_http_request_headers(esp_http_client_handle_t client,
                                         const char *api_key)
{
    esp_err_t err = set_http_header_checked(client, kHttpAcceptHeaderName, kHttpAcceptHeader);
    if (err == ESP_OK) {
        err = set_http_header_checked(client,
                                      kHttpAcceptEncodingHeaderName,
                                      kHttpAcceptEncodingHeader);
    }
    if (err == ESP_OK && cstr_nonempty(api_key)) {
        err = set_http_header_checked(client, kQweatherApiKeyHeader, api_key);
    }
    return err;
}
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
    if (!evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (!buffer->data || buffer->cap == 0 || buffer->len >= buffer->cap) {
        buffer->truncated = true;
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
    if (copy_len < event_len) {
        buffer->truncated = true;
    }
    return ESP_OK;
}

esp_err_t decode_http_body(char *out, size_t out_len, size_t *body_len)
{
    if (!decode_http_body_args_valid(out, out_len, body_len)) {
        ESP_LOGW(TAG, "%s", kHttpDecodeInvalidArgLog);
        return ESP_ERR_INVALID_ARG;
    }
    if (*body_len < kGzipHeaderProbeSize ||
        !network_gzip_detail::has_magic_prefix(out, *body_len)) {
        return ESP_OK;
    }

    size_t payload_offset = 0;
    size_t payload_len = 0;
    if (!gzip_payload_range((const uint8_t *)out, *body_len, &payload_offset, &payload_len)) {
        ESP_LOGW(TAG, HTTP_GZIP_HEADER_INVALID_FORMAT, (unsigned)*body_len);
        return ESP_FAIL;
    }

    ScopedHeapBuffer<uint8_t> compressed(*body_len,
                                         HeapBufferInit::kUninitialized,
                                         HeapBufferStorage::kPsramPreferred);
    if (!compressed) {
        ESP_LOGW(TAG, HTTP_TEMP_BUFFER_ALLOC_FAILED_FORMAT, (unsigned)*body_len);
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
    if (!http_get_text_args_valid(url, out, out_len)) {
        ESP_LOGW(TAG, "%s", kHttpGetInvalidArgLog);
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    int timeout_ms = 0;
    if (!compute_http_timeout_ms(&timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    NetworkHttpTransactionGuard transaction_lock(pdMS_TO_TICKS(timeout_ms));
    if (!transaction_lock.locked()) {
        ESP_LOGW(TAG, "%s", kHttpTransactionLockTimeoutLog);
        return ESP_ERR_TIMEOUT;
    }
    HttpTextConfigWorkspaceGuard config_workspace;
    esp_http_client_config_t &config = config_workspace.config();
    HttpBuffer buffer = {out, 0, out_len, false};
    config.url = url;
    config.event_handler = http_event_handler;
    config.user_data = &buffer;
    config.timeout_ms = timeout_ms;
    const bool qweather_url = is_qweather_url(url);
    const size_t attempt_count = http_tls_attempt_count(qweather_url);
    esp_err_t err = ESP_FAIL;
    int status = 0;
    int64_t content_length = 0;
    for (size_t attempt = 0; attempt < attempt_count; ++attempt) {
        // Lock acquisition and the first handshake may consume the boot budget.
        if (!compute_http_timeout_ms(&timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        config.timeout_ms = timeout_ms;
        int tls_error = 0;
        int tls_flags = 0;
        buffer.len = 0;
        buffer.truncated = false;
        out[0] = '\0';
        config.cert_pem = nullptr;
        config.crt_bundle_attach = nullptr;
        if (http_tls_trust_mode(qweather_url, attempt) ==
            HttpTlsTrustMode::kQweatherLegacyCa) {
            config.cert_pem = kQweatherCaDvR36Pem;
        } else {
            config.crt_bundle_attach = esp_crt_bundle_attach;
        }
        {
            ScopedHttpClient client(&config);
            if (!client) {
                ESP_LOGW(TAG, "%s", kHttpClientInitFailedLog);
                return ESP_FAIL;
            }
            esp_err_t header_err = configure_http_request_headers(client.get(), api_key);
            if (header_err != ESP_OK) {
                return header_err;
            }
            err = esp_http_client_perform(client.get());
            if (err == ESP_ERR_HTTP_CONNECT) {
                const esp_err_t tls_result = esp_http_client_get_and_clear_last_tls_error(
                    client.get(), &tls_error, &tls_flags);
                ESP_LOGW(TAG, "http TLS failure: detail=%s code=%d flags=0x%x",
                         esp_err_to_name(tls_result), tls_error,
                         static_cast<unsigned>(tls_flags));
            }
            status = esp_http_client_get_status_code(client.get());
            content_length = esp_http_client_get_content_length(client.get());
        }
        if (err == ESP_OK || !http_tls_should_retry(
                                 qweather_url,
                                 attempt,
                                 err == ESP_ERR_HTTP_CONNECT,
                                 tls_flags != 0)) {
            break;
        }
        ESP_LOGW(TAG, "%s", kHttpQweatherTlsFallbackLog);
        vTaskDelay(kHttpTlsFallbackDelay);
    }
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
    if (http_response_is_truncated(buffer.truncated, content_length, out_len)) {
        ESP_LOGW(TAG, HTTP_RESPONSE_TRUNCATED_FORMAT,
                 status,
                 (long long)content_length,
                 (unsigned)buffer.len,
                 (unsigned)out_len,
                 buffer.truncated);
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, HTTP_GET_OK_FORMAT,
             status,
             (unsigned)buffer.len,
             network_gzip_detail::has_magic_prefix(out, buffer.len));
    return decode_http_body(out, out_len, &buffer.len);
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
