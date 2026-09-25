// 声明网络文本请求、响应解码和受控日志预览接口。
#pragma once

#include "http_request_timing.h"

#include "esp_err.h"
#include "esp_http_client.h"

#include <stddef.h>
#include <stdint.h>

struct HttpBuffer {
    char *data;
    size_t len;
    size_t cap;
    bool truncated;
    uint32_t *connection_count;
    HttpRequestTimingState *timing;
};

struct HttpTextSessionPolicy {
    int first_qweather_timeout_ms = 0;
};

struct HttpTextRequestStats {
    uint32_t request_index = 0;
    uint32_t attempt_count = 0;
    uint32_t connected_ms = kHttpRequestTimingUnavailableMs;
    uint32_t first_data_ms = kHttpRequestTimingUnavailableMs;
    uint32_t elapsed_ms = 0;
    int status_code = 0;
    esp_err_t result = static_cast<esp_err_t>(0);
    bool reused_client = false;
    bool transient_retry = false;
};

struct HttpTextSessionStats {
    uint32_t request_count = 0;
    uint32_t client_create_count = 0;
    uint32_t connection_count = 0;
    uint32_t reused_request_count = 0;
    uint32_t transient_retry_count = 0;
    uint64_t elapsed_ms = 0;
};

class HttpTextSession {
public:
    explicit HttpTextSession(bool allow_reuse = true,
                             HttpTextSessionPolicy policy = {});
    ~HttpTextSession();

    HttpTextSession(const HttpTextSession &) = delete;
    HttpTextSession &operator=(const HttpTextSession &) = delete;

    esp_err_t get(const char *url,
                  char *out,
                  size_t out_len,
                  const char *api_key = nullptr,
                  const char *diagnostic_stage = nullptr);
    HttpTextSessionStats stats() const;
    HttpTextRequestStats last_request_stats() const;

private:
    bool ensure_transaction_lock(int timeout_ms);
    bool prepare_client(const char *url,
                        int timeout_ms,
                        const char *api_key,
                        uint8_t trust_mode,
                        bool *reused_client);
    void cleanup_client();
    void release_transaction_lock();
    void finish_request_timing(const char *diagnostic_stage,
                               uint32_t attempt_count,
                               bool reused_client,
                               bool transient_retry,
                               int status_code,
                               esp_err_t result);

    void *client_ = nullptr;
    HttpBuffer buffer_ = {};
    char origin_url_[192] = {};
    uint32_t request_count_ = 0;
    uint32_t client_create_count_ = 0;
    uint32_t connection_count_ = 0;
    uint32_t reused_request_count_ = 0;
    uint32_t transient_retry_count_ = 0;
    HttpRequestTimingState request_timing_ = {};
    HttpTextRequestStats last_request_stats_ = {};
    int64_t started_us_ = 0;
    int first_qweather_timeout_ms_ = 0;
    uint8_t trust_mode_ = 0;
    bool allow_reuse_ = true;
    bool transaction_locked_ = false;
};

esp_err_t http_event_handler(esp_http_client_event_t *evt);
esp_err_t decode_http_body(char *out, size_t out_len, size_t *body_len);
esp_err_t http_get_text(const char *url,
                        char *out,
                        size_t out_len,
                        const char *api_key = nullptr);
void log_response_preview(const char *stage, const char *response);
