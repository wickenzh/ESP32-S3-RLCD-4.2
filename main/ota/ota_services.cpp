// 处理固件更新检查、下载、校验、写入和重启提示流程。
#include "ota_services.h"

#include "network_services.h"
#include "sensor_services.h"
#include "ui_views.h"

#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "display_bsp.h"

struct OtaManifest {
    char version[kOtaVersionLen] = {};
    char url[kOtaUrlLen] = {};
    char sha256[kOtaSha256Len] = {};
    char notes[kOtaNotesLen] = {};
    int size = 0;
};

struct OtaCrashBreadcrumb {
    uint32_t magic = 0;
    int phase = 0;
    int total = 0;
    int progress = 0;
};

struct OtaHttpContext {
    char *redirect_url = nullptr;
    size_t redirect_url_len = 0;
};

struct OtaManifestSource {
    const char *name = nullptr;
    const char *url = nullptr;
};

static RTC_DATA_ATTR OtaCrashBreadcrumb s_ota_breadcrumb;
static constexpr uint32_t kOtaBreadcrumbMagic = 0x4f544131;
static constexpr int kOtaMaxRedirects = 5;
static constexpr size_t kOtaRedirectUrlLen = 1024;
static constexpr int kOtaHttpTxBufferSize = 2048;
static constexpr size_t kOtaManifestResponseBufferSize = 2048;
static constexpr int kOtaManifestSourceNameLen = 16;
static constexpr int kSemverComponentCount = 3;
static constexpr size_t kSha256ByteCount = 32;
static constexpr size_t kSha256HexLen = kSha256ByteCount * 2;
static constexpr size_t kOtaDownloadStatusTextLen = 48;
static constexpr uint32_t kOtaFailureHoldMs = 5000;
static constexpr uint32_t kOtaSuccessHoldMs = 6000;
static constexpr uint32_t kOtaOfflineHoldMs = 3500;
static constexpr uint32_t kOtaRebootNoticeDelayMs = 3500;
static constexpr uint32_t kOtaWifiConnectTimeoutMs = 45000;
static constexpr TickType_t kOtaReadRetryDelay = pdMS_TO_TICKS(100);

static void log_ota_heap(const char *stage, int downloaded, int progress)
{
    ESP_LOGI(TAG,
             "OTA heap %s: total=%d progress=%d dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage,
             downloaded,
             progress,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

class OtaDisplayQuietGuard {
public:
    OtaDisplayQuietGuard()
    {
        Display_SetOtaQuietMode(true);
    }

    ~OtaDisplayQuietGuard()
    {
        Display_SetOtaQuietMode(false);
    }
};

class OtaTaskWatchdogGuard {
public:
    OtaTaskWatchdogGuard()
    {
        if (esp_task_wdt_status(nullptr) == ESP_OK) {
            active_ = true;
            return;
        }
        esp_err_t err = esp_task_wdt_add(nullptr);
        if (err == ESP_OK) {
            active_ = true;
            added_ = true;
        } else {
            ESP_LOGW(TAG, "OTA task watchdog subscribe skipped: %s", esp_err_to_name(err));
        }
    }

    ~OtaTaskWatchdogGuard()
    {
        if (added_) {
            esp_err_t err = esp_task_wdt_delete(nullptr);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "OTA task watchdog unsubscribe failed: %s", esp_err_to_name(err));
            }
        }
    }

    void reset()
    {
        if (active_) {
            esp_task_wdt_reset();
        }
    }

private:
    bool active_ = false;
    bool added_ = false;
};

class OtaManifestResponseBuffer {
public:
    explicit OtaManifestResponseBuffer(size_t size)
        : data_((char *)malloc(size))
    {
        if (data_) {
            data_[0] = '\0';
        }
    }

    ~OtaManifestResponseBuffer()
    {
        free(data_);
    }

    OtaManifestResponseBuffer(const OtaManifestResponseBuffer &) = delete;
    OtaManifestResponseBuffer &operator=(const OtaManifestResponseBuffer &) = delete;

    char *data() const
    {
        return data_;
    }

private:
    char *data_ = nullptr;
};

class OtaJsonRoot {
public:
    explicit OtaJsonRoot(const char *json)
        : root_(cJSON_Parse(json))
    {
    }

    ~OtaJsonRoot()
    {
        cJSON_Delete(root_);
    }

    OtaJsonRoot(const OtaJsonRoot &) = delete;
    OtaJsonRoot &operator=(const OtaJsonRoot &) = delete;

    cJSON *get() const
    {
        return root_;
    }

private:
    cJSON *root_ = nullptr;
};

class OtaDownloadBuffer {
public:
    explicit OtaDownloadBuffer(size_t size)
        : data_((uint8_t *)malloc(size))
    {
    }

    ~OtaDownloadBuffer()
    {
        free(data_);
    }

    OtaDownloadBuffer(const OtaDownloadBuffer &) = delete;
    OtaDownloadBuffer &operator=(const OtaDownloadBuffer &) = delete;

    uint8_t *data() const
    {
        return data_;
    }

private:
    uint8_t *data_ = nullptr;
};

static void ota_note_phase(int phase, int total, int progress)
{
    s_ota_breadcrumb.magic = kOtaBreadcrumbMagic;
    s_ota_breadcrumb.phase = phase;
    s_ota_breadcrumb.total = total;
    s_ota_breadcrumb.progress = progress;
}

static void ota_set_status(int state, const char *text, int progress = -1, uint32_t hold_ms = 0)
{
    g_ota_state = state;
    g_ota_progress = progress;
    strlcpy(g_ota_status, text ? text : "OTA status error", sizeof(g_ota_status));
    g_ota_status_until_tick = hold_ms > 0 ? xTaskGetTickCount() + pdMS_TO_TICKS(hold_ms) : 0;
    notify_ui_task();
}

static void cleanup_ota_http_client(esp_http_client_handle_t *client)
{
    if (!client || !*client) {
        return;
    }
    esp_http_client_cleanup(*client);
    *client = nullptr;
}

static void close_ota_http_client(esp_http_client_handle_t *client)
{
    if (!client || !*client) {
        return;
    }
    esp_http_client_close(*client);
    cleanup_ota_http_client(client);
}

static bool set_ota_event_bit(EventBits_t bit, const char *name)
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "OTA %s skipped: event group unavailable", name ? name : "request");
        ota_set_status(kOtaFailed, "Update unavailable", -1, kOtaFailureHoldMs);
        return false;
    }
    xEventGroupSetBits(g_app_events, bit);
    return true;
}

static void keep_ota_settings_panel_visible()
{
    TickType_t now = xTaskGetTickCount();
    g_settings_requested = true;
    g_settings_focus_secondary = true;
    g_settings_page_order_mode = false;
    g_settings_primary_selection = kSettingsPrimarySystem;
    g_settings_selection = 4;
    g_settings_last_activity_tick = now;
    g_info_page_until_tick = 0;
}

static bool is_http_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt)
{
    if (!evt) {
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data) {
        return ESP_OK;
    }
    OtaHttpContext *ctx = (OtaHttpContext *)evt->user_data;
    if (!ctx->redirect_url || ctx->redirect_url_len == 0 ||
        !evt->header_key || !evt->header_value) {
        return ESP_OK;
    }
    if (strcasecmp(evt->header_key, "Location") == 0) {
        strlcpy(ctx->redirect_url, evt->header_value, ctx->redirect_url_len);
    }
    return ESP_OK;
}

bool ota_flow_active()
{
    return g_ota_state == kOtaChecking ||
           (g_ota_state == kOtaAvailable &&
            g_ota_status_until_tick != 0 &&
            xTaskGetTickCount() < g_ota_status_until_tick) ||
           g_ota_state == kOtaUpdating ||
           (g_ota_state == kOtaSucceeded &&
            g_ota_status_until_tick != 0 &&
            xTaskGetTickCount() < g_ota_status_until_tick);
}

void ota_reset_status_if_idle()
{
    if (!ota_flow_active() &&
        g_ota_state != kOtaIdle &&
        g_ota_status_until_tick != 0 &&
        xTaskGetTickCount() >= g_ota_status_until_tick) {
        g_ota_state = kOtaIdle;
        g_ota_status_until_tick = 0;
    }
    if (g_ota_state == kOtaIdle) {
        strlcpy(g_ota_status, "BOOT: Check Update", sizeof(g_ota_status));
        g_ota_progress = -1;
        g_ota_speed_kbps = -1;
    }
}

void ota_handle_info_key()
{
    ota_reset_status_if_idle();
    if (g_offline_mode_ui_enabled) {
        keep_ota_settings_panel_visible();
        ota_set_status(kOtaFailed, "Offline mode", -1, kOtaOfflineHoldMs);
        return;
    }
    if (g_ota_state == kOtaChecking || g_ota_state == kOtaUpdating) {
        return;
    }
    keep_ota_settings_panel_visible();
    if (g_ota_state == kOtaAvailable) {
        if (!set_ota_event_bit(kOtaInstallBit, "install")) {
            return;
        }
        g_ota_speed_kbps = -1;
        ota_set_status(kOtaUpdating, "Installing update 0%", 0);
        g_info_page_until_tick = 0;
        return;
    }
    if (!set_ota_event_bit(kOtaCheckBit, "check")) {
        return;
    }
    ota_set_status(kOtaChecking, "Checking update");
    g_info_page_until_tick = 0;
}

void ota_mark_running_app_valid()
{
    if (s_ota_breadcrumb.magic == kOtaBreadcrumbMagic) {
        ESP_LOGW(TAG,
                 "previous OTA breadcrumb: phase=%d total=%d progress=%d%% reset=%d",
                 s_ota_breadcrumb.phase,
                 s_ota_breadcrumb.total,
                 s_ota_breadcrumb.progress,
                 (int)esp_reset_reason());
        s_ota_breadcrumb.magic = 0;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA app marked valid");
        } else {
            ESP_LOGW(TAG, "OTA app valid mark failed: %s", esp_err_to_name(err));
        }
    }
}

static int parse_semver_component(const char **cursor)
{
    if (!cursor || !*cursor) {
        return 0;
    }
    int value = 0;
    while (**cursor >= '0' && **cursor <= '9') {
        value = value * 10 + (**cursor - '0');
        ++(*cursor);
    }
    if (**cursor == '.') {
        ++(*cursor);
    }
    return value;
}

static int compare_versions(const char *remote, const char *current)
{
    if (!remote || !current) {
        return 0;
    }
    if (*remote == 'v' || *remote == 'V') ++remote;
    if (*current == 'v' || *current == 'V') ++current;
    for (int i = 0; i < kSemverComponentCount; ++i) {
        int r = parse_semver_component(&remote);
        int c = parse_semver_component(&current);
        if (r != c) {
            return r > c ? 1 : -1;
        }
    }
    return strcmp(remote, current);
}

static bool valid_sha256_string(const char *text)
{
    if (!text) {
        return false;
    }
    if (strlen(text) != kSha256HexLen) {
        return false;
    }
    for (const char *p = text; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') ||
              (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F'))) {
            return false;
        }
    }
    return true;
}

static void sha256_to_hex(const uint8_t *hash, char *out, size_t out_len)
{
    static const char kHex[] = "0123456789abcdef";
    if (!out) {
        return;
    }
    if (out_len <= kSha256HexLen) {
        if (out_len > 0) out[0] = '\0';
        return;
    }
    if (!hash) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < kSha256ByteCount; ++i) {
        out[i * 2] = kHex[hash[i] >> 4];
        out[i * 2 + 1] = kHex[hash[i] & 0x0F];
    }
    out[kSha256HexLen] = '\0';
}

static bool parse_ota_manifest(const char *json, OtaManifest *manifest)
{
    if (!json || !manifest) {
        ESP_LOGW(TAG, "OTA manifest parse invalid arg");
        return false;
    }
    OtaJsonRoot root(json);
    if (!root.get()) {
        ESP_LOGW(TAG, "OTA manifest JSON parse failed");
        return false;
    }
    bool have_version = json_copy_string(root.get(), "version", manifest->version, sizeof(manifest->version)) &&
                        manifest->version[0] != '\0';
    bool have_url = json_copy_string(root.get(), "url", manifest->url, sizeof(manifest->url)) &&
                    manifest->url[0] != '\0';
    bool have_sha = json_copy_string(root.get(), "sha256", manifest->sha256, sizeof(manifest->sha256));
    cJSON *size = cJSON_GetObjectItem(root.get(), "size");
    if (cJSON_IsNumber(size)) {
        manifest->size = size->valueint;
    }
    (void)json_copy_string(root.get(), "notes", manifest->notes, sizeof(manifest->notes));
    if (!have_version || !have_url || !have_sha) {
        ESP_LOGW(TAG, "OTA manifest missing required fields version=%d url=%d sha=%d",
                 have_version,
                 have_url,
                 have_sha);
        return false;
    }
    if (!valid_sha256_string(manifest->sha256)) {
        ESP_LOGW(TAG, "OTA manifest sha invalid len=%u", (unsigned)strlen(manifest->sha256));
        return false;
    }
    return true;
}

static bool ota_manifest_source_valid(const OtaManifestSource &source)
{
    return source.name && source.name[0] &&
           source.url && source.url[0] &&
           strstr(source.url, "example.invalid") == nullptr;
}

static bool fetch_ota_manifest_from_source(const OtaManifestSource &source, OtaManifest *manifest)
{
    if (!manifest) {
        ota_set_status(kOtaFailed, "Check failed", -1, kOtaFailureHoldMs);
        return false;
    }
    if (!ota_manifest_source_valid(source)) {
        ESP_LOGW(TAG, "OTA manifest source skipped: %s", source.name ? source.name : "unknown");
        return false;
    }
    OtaManifestResponseBuffer response(kOtaManifestResponseBufferSize);
    if (!response.data()) {
        ESP_LOGW(TAG, "OTA manifest response alloc failed");
        ota_set_status(kOtaFailed, "Check failed", -1, kOtaFailureHoldMs);
        return false;
    }
    esp_err_t err = http_get_text(source.url, response.data(), kOtaManifestResponseBufferSize);
    if (err != ESP_OK || !parse_ota_manifest(response.data(), manifest)) {
        ESP_LOGW(TAG, "OTA manifest failed source=%s err=%s", source.name, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "OTA manifest loaded source=%s version=%s", source.name, manifest->version);
    return true;
}

static bool fetch_ota_manifest(OtaManifest *manifest, char *source_name = nullptr, size_t source_name_len = 0)
{
    static const OtaManifestSource kSources[] = {
        {"R2", kOtaManifestUrl},
        {"GitHub", kOtaBackupManifestUrl},
    };
    for (const auto &source : kSources) {
        if (fetch_ota_manifest_from_source(source, manifest)) {
            if (source_name && source_name_len > 0) {
                strlcpy(source_name, source.name, source_name_len);
            }
            return true;
        }
    }
    ota_set_status(kOtaFailed, "Check failed", -1, kOtaFailureHoldMs);
    return false;
}

static bool fetch_backup_manifest_for_install(const OtaManifest &current, OtaManifest *backup)
{
    if (!backup || current.version[0] == '\0' || !valid_sha256_string(current.sha256)) {
        return false;
    }
    OtaManifest candidate;
    const OtaManifestSource backup_source = {"GitHub", kOtaBackupManifestUrl};
    if (!fetch_ota_manifest_from_source(backup_source, &candidate)) {
        return false;
    }
    if (strcmp(candidate.version, current.version) != 0 ||
        strcasecmp(candidate.sha256, current.sha256) != 0 ||
        (current.size > 0 && candidate.size > 0 && current.size != candidate.size)) {
        ESP_LOGW(TAG,
                 "OTA backup manifest mismatch current=%s backup=%s",
                 current.version,
                 candidate.version);
        return false;
    }
    *backup = candidate;
    return true;
}

static bool download_and_apply_ota(const OtaManifest &manifest)
{
    if (manifest.url[0] == '\0' || !valid_sha256_string(manifest.sha256)) {
        ESP_LOGW(TAG, "OTA manifest invalid for install");
        ota_set_status(kOtaFailed, "Download failed", -1, kOtaFailureHoldMs);
        return false;
    }
    ota_note_phase(1, 0, 0);
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        ota_set_status(kOtaFailed, "No OTA slot", -1, kOtaFailureHoldMs);
        return false;
    }

    wifi_ap_record_t ap_info = {};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    ESP_LOGI(TAG,
             "OTA start: reset=%d battery=%d%% %.3fV rssi=%d size=%d url=%s",
             (int)esp_reset_reason(),
             g_battery_percent,
             g_battery_voltage,
             rssi,
             manifest.size,
             manifest.url);
    log_ota_heap("start", 0, 0);

    char current_url[kOtaRedirectUrlLen];
    strlcpy(current_url, manifest.url, sizeof(current_url));
    esp_http_client_handle_t client = nullptr;
    int content_len = 0;
    esp_err_t err = ESP_FAIL;
    for (int redirect = 0; redirect <= kOtaMaxRedirects; ++redirect) {
        char redirect_url[kOtaRedirectUrlLen] = {};
        OtaHttpContext http_ctx = {redirect_url, sizeof(redirect_url)};
        esp_http_client_config_t config = {};
        config.url = current_url;
        config.timeout_ms = kOtaHttpTimeoutMs;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.disable_auto_redirect = true;
        config.max_redirection_count = kOtaMaxRedirects;
        config.keep_alive_enable = true;
        config.buffer_size = kOtaDownloadBufferSize;
        config.buffer_size_tx = kOtaHttpTxBufferSize;
        config.event_handler = ota_http_event_handler;
        config.user_data = &http_ctx;

        client = esp_http_client_init(&config);
        if (!client) {
            ota_set_status(kOtaFailed, "Download failed", -1, kOtaFailureHoldMs);
            return false;
        }
        esp_http_client_set_header(client, "Accept", "application/octet-stream,*/*");
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA http open failed: %s", esp_err_to_name(err));
            cleanup_ota_http_client(&client);
            ota_set_status(kOtaFailed, "Download failed", -1, kOtaFailureHoldMs);
            return false;
        }
        content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (is_http_redirect_status(status)) {
            ESP_LOGI(TAG, "OTA redirect status=%d location=%s", status, redirect_url[0] ? redirect_url : "--");
            close_ota_http_client(&client);
            if (redirect_url[0] == '\0' || strlen(redirect_url) >= sizeof(current_url)) {
                ota_set_status(kOtaFailed, "Download failed", -1, kOtaFailureHoldMs);
                return false;
            }
            strlcpy(current_url, redirect_url, sizeof(current_url));
            continue;
        }
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "OTA http status=%d content_len=%d", status, content_len);
            close_ota_http_client(&client);
            ota_set_status(kOtaFailed, "Download failed", -1, kOtaFailureHoldMs);
            return false;
        }
        break;
    }
    if (!client) {
        ESP_LOGW(TAG, "OTA redirect limit reached");
        ota_set_status(kOtaFailed, "Download failed", -1, kOtaFailureHoldMs);
        return false;
    }

    esp_ota_handle_t ota_handle = 0;
    ota_note_phase(2, 0, 0);
    err = esp_ota_begin(update_partition, manifest.size > 0 ? manifest.size : OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        close_ota_http_client(&client);
        ota_set_status(kOtaFailed, "Update failed", -1, kOtaFailureHoldMs);
        return false;
    }

    OtaDownloadBuffer buffer(kOtaDownloadBufferSize);
    if (!buffer.data()) {
        esp_ota_abort(ota_handle);
        close_ota_http_client(&client);
        ota_set_status(kOtaFailed, "No memory", -1, kOtaFailureHoldMs);
        return false;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    int total = 0;
    int last_progress = -1;
    int last_speed_kbps = -1;
    int last_heap_progress = -25;
    int64_t started_us = esp_timer_get_time();
    int64_t last_progress_us = started_us;
    int64_t last_status_us = 0;
    bool ok = true;
    OtaTaskWatchdogGuard wdt;
    OtaDisplayQuietGuard display_quiet;
    for (;;) {
        wdt.reset();
        int64_t now_us = esp_timer_get_time();
        if (now_us - started_us > (int64_t)kOtaMaxDownloadMs * 1000) {
            ESP_LOGW(TAG, "OTA download timed out total=%d", total);
            ok = false;
            break;
        }
        int read = esp_http_client_read(client, (char *)buffer.data(), kOtaDownloadBufferSize);
        wdt.reset();
        if (read < 0) {
            if (esp_timer_get_time() - last_progress_us > (int64_t)kOtaNoProgressTimeoutMs * 1000) {
                ESP_LOGW(TAG, "OTA read failed with no progress total=%d", total);
                ok = false;
                break;
            }
            vTaskDelay(kOtaReadRetryDelay);
            continue;
        }
        if (read == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (esp_timer_get_time() - last_progress_us > (int64_t)kOtaNoProgressTimeoutMs * 1000) {
                ESP_LOGW(TAG, "OTA stalled total=%d", total);
                ok = false;
                break;
            }
            vTaskDelay(kOtaReadRetryDelay);
            continue;
        }
        last_progress_us = esp_timer_get_time();
        mbedtls_sha256_update(&sha_ctx, buffer.data(), read);
        err = esp_ota_write(ota_handle, buffer.data(), read);
        wdt.reset();
        if (err != ESP_OK) {
            ok = false;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(kOtaChunkDelayMs));
        total += read;
        int expected = content_len > 0 ? content_len : manifest.size;
        if (expected > 0) {
            int progress = (total * 100) / expected;
            if (progress > 100) progress = 100;
            if (progress != last_progress) {
                ota_note_phase(3, total, progress);
            }
            if (progress >= last_heap_progress + 25 || progress >= 100) {
                log_ota_heap("download", total, progress);
                last_heap_progress = progress;
            }
            int64_t elapsed_us = esp_timer_get_time() - started_us;
            int64_t now_us = esp_timer_get_time();
            int speed_kbps = 0;
            if (elapsed_us > 0) {
                speed_kbps = (int)((int64_t)total * 1000000 / elapsed_us / 1024);
            }
            bool progress_step = progress != last_progress;
            bool speed_step = last_speed_kbps < 0 || abs(speed_kbps - last_speed_kbps) >= 8;
            bool status_due = last_status_us == 0 ||
                              now_us - last_status_us >= (int64_t)kOtaStatusMinIntervalMs * 1000 ||
                              progress >= 100;
            if (progress_step && status_due) {
                char status_text[kOtaDownloadStatusTextLen];
                snprintf(status_text, sizeof(status_text), "Installing %d%%  %dKB/s", progress, speed_kbps);
                g_ota_speed_kbps = speed_kbps;
                ota_set_status(kOtaUpdating, status_text, progress);
                last_status_us = now_us;
                last_progress = progress;
                last_speed_kbps = speed_kbps;
            } else if (speed_step && status_due) {
                g_ota_speed_kbps = speed_kbps;
                last_status_us = now_us;
                last_speed_kbps = speed_kbps;
            }
        }
    }

    uint8_t hash[kSha256ByteCount];
    wdt.reset();
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);
    bool complete = esp_http_client_is_complete_data_received(client);
    close_ota_http_client(&client);

    if (!ok || !complete) {
        esp_ota_abort(ota_handle);
        ota_set_status(kOtaFailed, "Download failed", -1, kOtaFailureHoldMs);
        return false;
    }

    char actual_sha[kOtaSha256Len];
    sha256_to_hex(hash, actual_sha, sizeof(actual_sha));
    ota_note_phase(4, total, 100);
    if (strcasecmp(actual_sha, manifest.sha256) != 0) {
        ESP_LOGW(TAG, "OTA sha mismatch expected=%s actual=%s", manifest.sha256, actual_sha);
        esp_ota_abort(ota_handle);
        ota_set_status(kOtaFailed, "Verify failed", -1, kOtaFailureHoldMs);
        return false;
    }

    wdt.reset();
    ota_note_phase(5, total, 100);
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA end failed: %s", esp_err_to_name(err));
        ota_set_status(kOtaFailed, "Update failed", -1, kOtaFailureHoldMs);
        return false;
    }
    esp_app_desc_t app_desc = {};
    err = esp_ota_get_partition_description(update_partition, &app_desc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA app description failed: %s", esp_err_to_name(err));
        ota_set_status(kOtaFailed, "Verify failed", -1, kOtaFailureHoldMs);
        return false;
    }
    ESP_LOGI(TAG, "OTA image ready: version=%s project=%s", app_desc.version, app_desc.project_name);
    wdt.reset();
    ota_note_phase(6, total, 100);
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA boot partition failed: %s", esp_err_to_name(err));
        ota_set_status(kOtaFailed, "Update failed", -1, kOtaFailureHoldMs);
        return false;
    }

    ota_set_status(kOtaSucceeded, "Update done. Rebooting...", 100, kOtaSuccessHoldMs);
    s_ota_breadcrumb.magic = 0;
    return true;
}

static bool prepare_ota_wifi()
{
    if (!g_have_wifi_creds) {
        ota_set_status(kOtaFailed, "No WiFi", -1, kOtaFailureHoldMs);
        return false;
    }
    if (g_low_battery_mode || (g_battery_percent >= 0 && g_battery_percent < kLowBatteryEnterPercent)) {
        ota_set_status(kOtaFailed, "Low battery", -1, kOtaFailureHoldMs);
        return false;
    }
    acquire_network_awake_lock();
    if (!start_wifi_radio(false)) {
        release_network_awake_lock();
        ota_set_status(kOtaFailed, "WiFi failed", -1, kOtaFailureHoldMs);
        return false;
    }
    if (!wait_for_wifi_connected(kOtaWifiConnectTimeoutMs)) {
        stop_wifi_radio();
        release_network_awake_lock();
        ota_set_status(kOtaFailed, "WiFi failed", -1, kOtaFailureHoldMs);
        return false;
    }
    return true;
}

static void finish_ota_wifi()
{
    stop_wifi_radio();
    release_network_awake_lock();
}

void ota_task(void *)
{
    if (!g_app_events) {
        ESP_LOGW(TAG, "OTA task stopped: event group unavailable");
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(g_app_events,
                                               kOtaCheckBit | kOtaInstallBit,
                                               pdTRUE,
                                               pdFALSE,
                                               portMAX_DELAY);
        bool install = (bits & kOtaInstallBit) != 0;
        bool check = (bits & kOtaCheckBit) != 0;
        if (!install && !check) {
            continue;
        }

        if (!prepare_ota_wifi()) {
            g_info_page_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(kOtaFailureHoldMs);
            continue;
        }

        OtaManifest manifest;
        if (install) {
            strlcpy(manifest.version, g_ota_version, sizeof(manifest.version));
            strlcpy(manifest.url, g_ota_url, sizeof(manifest.url));
            strlcpy(manifest.sha256, g_ota_sha256, sizeof(manifest.sha256));
            strlcpy(manifest.notes, g_ota_notes, sizeof(manifest.notes));
            manifest.size = g_ota_size;
        } else {
            ota_set_status(kOtaChecking, "Checking update");
            char manifest_source[kOtaManifestSourceNameLen] = {};
            if (!fetch_ota_manifest(&manifest, manifest_source, sizeof(manifest_source))) {
                finish_ota_wifi();
                g_info_page_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(kOtaFailureHoldMs);
                continue;
            }
            ESP_LOGI(TAG,
                     "OTA update check source=%s remote=%s current=%s",
                     manifest_source[0] ? manifest_source : "unknown",
                     manifest.version,
                     APP_VERSION);
            if (compare_versions(manifest.version, APP_VERSION) <= 0) {
                ota_set_status(kOtaNoUpdate, "Already latest", -1, kOtaFailureHoldMs);
                finish_ota_wifi();
                g_info_page_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(kOtaFailureHoldMs);
                continue;
            }
            strlcpy(g_ota_version, manifest.version, sizeof(g_ota_version));
            strlcpy(g_ota_url, manifest.url, sizeof(g_ota_url));
            strlcpy(g_ota_sha256, manifest.sha256, sizeof(g_ota_sha256));
            strlcpy(g_ota_notes, manifest.notes, sizeof(g_ota_notes));
            g_ota_size = manifest.size;
            char status_text[kOtaStatusLen];
            snprintf(status_text, sizeof(status_text), "New version %s", manifest.version);
            ota_set_status(kOtaAvailable, status_text, -1, kOtaAvailableConfirmTimeoutMs);
            finish_ota_wifi();
            continue;
        }

        bool ok = download_and_apply_ota(manifest);
        if (!ok) {
            OtaManifest backup_manifest;
            if (fetch_backup_manifest_for_install(manifest, &backup_manifest) &&
                strcmp(backup_manifest.url, manifest.url) != 0) {
                ESP_LOGW(TAG, "OTA primary download failed, retrying GitHub backup");
                ota_set_status(kOtaUpdating, "Installing backup 0%", 0);
                ok = download_and_apply_ota(backup_manifest);
            }
        }
        finish_ota_wifi();
        if (ok) {
            keep_ota_settings_panel_visible();
            ota_set_status(kOtaSucceeded, "Update done. Rebooting...", 100, kOtaSuccessHoldMs);
            vTaskDelay(pdMS_TO_TICKS(kOtaRebootNoticeDelayMs));
            esp_restart();
        } else {
            g_info_page_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(kOtaFailureHoldMs);
        }
    }
}
