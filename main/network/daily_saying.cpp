// 获取、解析和缓存图片时钟底部每日文字。
#include "network_services.h"

#include "ui_views.h"

namespace {
constexpr size_t kDailySayingResponseBufferSize = 768;
constexpr int kMaxSayingChars = 22;
constexpr int kMaxSayingAttempts = 8;
constexpr int kMaxSayingJsonDepth = 8;

static bool copy_json_saying_field(cJSON *obj, char *out, size_t out_len, int depth)
{
    static const char *const kFields[] = {
        "content",
        "saying",
        "text",
        "sentence",
        "hitokoto",
        "quote",
        "data",
    };
    if (!obj || !out || out_len == 0) {
        return false;
    }
    if (depth > kMaxSayingJsonDepth) {
        return false;
    }
    if (cJSON_IsString(obj) && obj->valuestring) {
        strlcpy(out, obj->valuestring, out_len);
        trim_ascii(out);
        return out[0] != '\0';
    }
    if (!cJSON_IsObject(obj)) {
        return false;
    }
    for (const char *field : kFields) {
        cJSON *item = cJSON_GetObjectItem(obj, field);
        if (!item) {
            continue;
        }
        if (cJSON_IsString(item) && item->valuestring) {
            strlcpy(out, item->valuestring, out_len);
            trim_ascii(out);
            return out[0] != '\0';
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
    cJSON *root = cJSON_Parse(response);
    if (root) {
        bool ok = copy_json_saying_field(root, out, out_len, 0);
        cJSON_Delete(root);
        if (ok) {
            return true;
        }
    }
    strlcpy(out, response, out_len);
    trim_ascii(out);
    return out[0] != '\0' && out[0] != '{' && out[0] != '[';
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
    char *response = (char *)calloc(kDailySayingResponseBufferSize, 1);
    if (!response) {
        ESP_LOGW(TAG, "daily saying response alloc failed");
        return false;
    }
    int http_failures = 0;
    int parse_failures = 0;
    int long_responses = 0;
    for (int attempt = 1; attempt <= kMaxSayingAttempts; ++attempt) {
        memset(response, 0, kDailySayingResponseBufferSize);
        esp_err_t err = http_get_text(kDailySayingUrl, response, kDailySayingResponseBufferSize, nullptr);
        if (err != ESP_OK) {
            ++http_failures;
            ESP_LOGW(TAG, "daily saying http failed err=%s", esp_err_to_name(err));
            continue;
        }
        bool ok = extract_daily_saying(response, next, sizeof(next));
        if (!ok) {
            ++parse_failures;
            ESP_LOGW(TAG, "daily saying parse failed");
            continue;
        }
        int chars = utf8_char_count(next);
        if (chars <= kMaxSayingChars) {
            break;
        }
        ++long_responses;
        ESP_LOGW(TAG, "daily saying too long chars=%d attempt=%d", chars, attempt);
        next[0] = '\0';
    }
    free(response);
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
