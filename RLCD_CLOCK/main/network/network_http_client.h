// 声明网络文本请求、响应解码和受控日志预览接口。
#pragma once

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
    explicit HttpTextSession(bool allow_reuse = true);
    ~HttpTextSession();

    HttpTextSession(const HttpTextSession &) = delete;
    HttpTextSession &operator=(const HttpTextSession &) = delete;

    esp_err_t get(const char *url,
                  char *out,
                  size_t out_len,
                  const char *api_key = nullptr);
    HttpTextSessionStats stats() const;

private:
    bool ensure_transaction_lock(int timeout_ms);
    bool prepare_client(const char *url,
                        int timeout_ms,
                        const char *api_key,
                        uint8_t trust_mode,
                        bool *reused_client);
    void cleanup_client();
    void release_transaction_lock();

    void *client_ = nullptr;
    HttpBuffer buffer_ = {};
    char origin_url_[192] = {};
    uint32_t request_count_ = 0;
    uint32_t client_create_count_ = 0;
    uint32_t connection_count_ = 0;
    uint32_t reused_request_count_ = 0;
    uint32_t transient_retry_count_ = 0;
    int64_t started_us_ = 0;
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
