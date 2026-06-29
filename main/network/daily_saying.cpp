// 获取、解析和缓存图片时钟底部每日文字。
#include "network_services.h"

#include "ui_views.h"

namespace {
constexpr size_t kDailySayingResponseBufferSize = 768;
constexpr int kMaxSayingChars = 22;
constexpr int kMaxSayingAttempts = 8;
constexpr int kMaxSayingJsonDepth = 8;
constexpr const char *const kDailySayingJsonFields[] = {
    "content",
    "saying",
    "text",
    "sentence",
    "hitokoto",
    "quote",
    "data",
};

class DailySayingResponseBuffer {
public:
    DailySayingResponseBuffer()
        : data_((char *)calloc(kDailySayingResponseBufferSize, 1))
    {
        if (!data_) {
            ESP_LOGW(TAG, "daily saying response alloc failed");
        }
    }

    ~DailySayingResponseBuffer()
    {
        free(data_);
    }

    DailySayingResponseBuffer(const DailySayingResponseBuffer &) = delete;
    DailySayingResponseBuffer &operator=(const DailySayingResponseBuffer &) = delete;

    char *get() const
    {
        return data_;
    }

    void clear() const
    {
        if (data_) {
            memset(data_, 0, kDailySayingResponseBufferSize);
        }
    }

    explicit operator bool() const
    {
        return data_ != nullptr;
    }

private:
    char *data_;
};

class DailySayingJsonRoot {
public:
    explicit DailySayingJsonRoot(const char *response)
        : root_(cJSON_Parse(response))
    {
    }

    ~DailySayingJsonRoot()
    {
        cJSON_Delete(root_);
    }

    DailySayingJsonRoot(const DailySayingJsonRoot &) = delete;
    DailySayingJsonRoot &operator=(const DailySayingJsonRoot &) = delete;

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

static bool copy_trimmed_saying_text(const char *text, char *out, size_t out_len)
{
    if (!text || !out || out_len == 0) {
        return false;
    }
    strlcpy(out, text, out_len);
    trim_ascii(out);
    return out[0] != '\0';
}

static bool plain_text_saying_candidate(const char *text)
{
    return text && text[0] != '\0' && text[0] != '{' && text[0] != '[';
}

static bool copy_json_saying_field(cJSON *obj, char *out, size_t out_len, int depth)
{
    if (!obj || !out || out_len == 0) {
        return false;
    }
    if (depth > kMaxSayingJsonDepth) {
        return false;
    }
    if (cJSON_IsString(obj) && obj->valuestring) {
        return copy_trimmed_saying_text(obj->valuestring, out, out_len);
    }
    if (!cJSON_IsObject(obj)) {
        return false;
    }
    for (const char *field : kDailySayingJsonFields) {
        cJSON *item = cJSON_GetObjectItem(obj, field);
        if (!item) {
            continue;
        }
        if (cJSON_IsString(item) && item->valuestring) {
            return copy_trimmed_saying_text(item->valuestring, out, out_len);
        }
        if (cJSON_IsObject(item) && copy_json_saying_field(item, out, out_len, depth + 1)) {
            return true;
        }
    }
    return false;
}

static bool extract_daily_saying(const char *response, char *out, size_t out_len)
{
    if (!response || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    DailySayingJsonRoot root(response);
    if (root) {
        bool ok = copy_json_saying_field(root.get(), out, out_len, 0);
        if (ok) {
            return true;
        }
    }
    strlcpy(out, response, out_len);
    trim_ascii(out);
    return plain_text_saying_candidate(out);
}

static int utf8_char_count(const char *text)
{
    if (!text) {
        return 0;
    }
    int count = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if ((*p & 0xC0) != 0x80) {
            ++count;
        }
        ++p;
    }
    return count;
}

static bool saying_within_length(const char *text, int *chars_out)
{
    int chars = utf8_char_count(text);
    if (chars_out) {
        *chars_out = chars;
    }
    return chars <= kMaxSayingChars;
}
} // namespace

void load_daily_saying_cache()
{
    g_daily_saying[0] = '\0';
    g_last_saying_sync_time = 0;
}

bool perform_daily_saying_update()
{
    if (g_low_battery_mode) {
        return false;
    }
    char next[kDailySayingLen] = {};
    DailySayingResponseBuffer response;
    if (!response) {
        return false;
    }
    int http_failures = 0;
    int parse_failures = 0;
    int long_responses = 0;
    for (int attempt = 1; attempt <= kMaxSayingAttempts; ++attempt) {
        response.clear();
        esp_err_t err = http_get_text(kDailySayingUrl, response.get(), kDailySayingResponseBufferSize, nullptr);
        if (err != ESP_OK) {
            ++http_failures;
            ESP_LOGW(TAG, "daily saying http failed err=%s", esp_err_to_name(err));
            continue;
        }
        bool ok = extract_daily_saying(response.get(), next, sizeof(next));
        if (!ok) {
            ++parse_failures;
            ESP_LOGW(TAG, "daily saying parse failed");
            continue;
        }
        int chars = 0;
        if (saying_within_length(next, &chars)) {
            break;
        }
        ++long_responses;
        ESP_LOGW(TAG, "daily saying too long chars=%d attempt=%d", chars, attempt);
        next[0] = '\0';
    }
    if (next[0] == '\0') {
        ESP_LOGW(TAG,
                 "daily saying update failed attempts=%d http=%d parse=%d long=%d",
                 kMaxSayingAttempts,
                 http_failures,
                 parse_failures,
                 long_responses);
        return false;
    }
    strlcpy(g_daily_saying, next, sizeof(g_daily_saying));
    time(&g_last_saying_sync_time);
    notify_ui_task();
    ESP_LOGI(TAG, "daily saying updated");
    return true;
}
