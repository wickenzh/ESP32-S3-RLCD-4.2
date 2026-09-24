// 处理固件更新检查、下载、校验、写入和重启提示流程。
#include "ota_services.h"

#include "app_event_group.h"
#include "app_metadata.h"
#include "app_task_readiness.h"
#include "battery_policy.h"
#include "battery_runtime_state.h"
#include "ota_download_policy.h"
#include "ota_download_http.h"
#include "ota_download_progress_policy.h"
#include "ota_flow_policy.h"
#include "ota_manifest_client.h"
#include "ota_manifest_client_internal.h"
#include "ota_manifest_workspace.h"
#include "ota_network_session_internal.h"
#include "ota_runtime_guards.h"
#include "ota_runtime_state_internal.h"
#include "ota_validation.h"
#include "runtime_health.h"

#include "app_constexpr.h"
#include "app_text_format.h"
#include "offline_mode_state.h"
#include "network_task_guards.h"
#include "scoped_heap_buffer.h"
#include "ui_info_page_state_internal.h"
#include "ui_settings_activity_state.h"
#include "ui_settings_navigation.h"
#include "ui_task_notify.h"

#include <esp_app_format.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

struct OtaCrashBreadcrumb {
    uint32_t magic = 0;
    int phase = 0;
    int total = 0;
    int progress = 0;
};

static RTC_DATA_ATTR OtaCrashBreadcrumb s_ota_breadcrumb;

static constexpr uint32_t kOtaBreadcrumbMagic = 0x4f544131;
static constexpr size_t kOtaDownloadStatusTextLen = 48;
static constexpr int64_t kOtaUsPerMs = 1000;
static constexpr uint32_t kOtaFailureHoldMs = 5000;
static constexpr uint32_t kOtaSuccessHoldMs = 6000;
static constexpr uint32_t kOtaOfflineHoldMs = 3500;
static constexpr uint32_t kOtaRebootNoticeDelayMs = 3500;
static constexpr uint32_t kOtaPreRestartDisplayQuietMs = 1500;
static constexpr uint32_t kOtaWifiConnectTimeoutMs = 45000;
static constexpr uint32_t kOtaReadRetryDelayMs = 100;
static constexpr TickType_t kOtaReadRetryDelay = pdMS_TO_TICKS(kOtaReadRetryDelayMs);
static constexpr const char *kOtaExpectedProjectName = "weather_clock";
static_assert(kOtaSha256HexLen + 1 == kOtaSha256Len,
              "OTA SHA256 hex length must match manifest storage");
static_assert(kOtaDownloadStatusTextLen <= kOtaStatusLen,
              "OTA download status scratch text must fit global OTA status storage");
static_assert(kOtaRebootNoticeDelayMs >= kOtaPreRestartDisplayQuietMs,
              "OTA reboot notice must outlast pre-restart display quiet window");
static constexpr const char *kOtaStatusCheckFailed = "Check failed";
static constexpr const char *kOtaStatusCheckingUpdate = "Checking update";
static constexpr const char *kOtaStatusAlreadyLatest = "Already latest";
static constexpr const char *kOtaStatusDownloadFailed = "Download failed";
static constexpr const char *kOtaStatusVerifyFailed = "Verify failed";
static constexpr const char *kOtaStatusUpdateFailed = "Update failed";
static constexpr const char *kOtaStatusUpdateDoneRebooting = "Update done. Rebooting...";
static constexpr const char *kOtaStatusNoWifi = "No WiFi";
static constexpr const char *kOtaStatusLowBattery = "Low battery";
static constexpr const char *kOtaStatusWifiFailed = "WiFi failed";
static constexpr const char *kOtaStatusNoOtaSlot = "No OTA slot";
static constexpr const char *kOtaStatusNoMemory = "No memory";
static constexpr const char *kOtaStatusOfflineMode = "Offline mode";
static constexpr const char *kOtaStatusSetupMode = "Setup mode";
static constexpr const char *kOtaStatusUnavailable = "Update unavailable";
static constexpr const char *kOtaStatusIdlePrompt = "BOOT: Check Update";
static constexpr const char *kOtaStatusInstallingUpdate = "Installing update 0%";
static constexpr const char *kOtaStatusInstallingBackup = "Installing backup 0%";
static constexpr const char *kOtaStatusInstallingProgressFormat = "Installing %d%%  %dKB/s";
static constexpr const char *kOtaStatusNewVersionFormat = "New version %s";
static constexpr const char *kOtaStatusFallbackError = "OTA status error";
static constexpr const char *kOtaRequestFallbackName = "request";
#define OTA_REQUEST_EVENT_GROUP_UNAVAILABLE_FORMAT "OTA %s skipped: event group unavailable"
#define OTA_HEAP_DIAGNOSTIC_FORMAT "OTA heap %s: total=%d progress=%d dma_free=%u dma_largest=%u internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u"
static constexpr const char *kOtaAppMarkedValidLog = "OTA app marked valid";
#define OTA_APP_VALID_MARK_FAILED_FORMAT "OTA app valid mark failed: %s"
#define OTA_PREVIOUS_BREADCRUMB_FORMAT "previous OTA breadcrumb: phase=%d total=%d progress=%d%% reset=%d"
static constexpr const char *kOtaManifestInvalidForInstallLog = "OTA manifest invalid for install";
#define OTA_DOWNLOAD_START_FORMAT \
    "OTA start: reset=%d battery=%d%% %.3fV rssi=%d size=%d url_len=%u"
static constexpr const char *kOtaHttpTransactionLockTimeoutLog =
    "OTA download deferred: HTTP transaction is busy";
#define OTA_BEGIN_FAILED_FORMAT "OTA begin failed: %s"
#define OTA_DOWNLOAD_BUFFER_ALLOC_FAILED_FORMAT "OTA download buffer allocation failed size=%u"
#define OTA_DOWNLOAD_TIMEOUT_FORMAT "OTA download timed out total=%d"
#define OTA_READ_FAILED_NO_PROGRESS_FORMAT "OTA read failed with no progress total=%d"
#define OTA_STALLED_FORMAT "OTA stalled total=%d"
#define OTA_WRITE_FAILED_FORMAT "OTA write failed total=%d chunk=%d err=%s"
#define OTA_SHA_MISMATCH_FORMAT "OTA sha mismatch expected=%s actual=%s"
#define OTA_END_FAILED_FORMAT "OTA end failed: %s"
#define OTA_APP_DESCRIPTION_FAILED_FORMAT "OTA app description failed: %s"
#define OTA_IMAGE_METADATA_MISMATCH_FORMAT \
    "OTA image metadata mismatch: expected_version=%s actual_version=%.*s expected_project=%s actual_project=%.*s"
#define OTA_IMAGE_READY_FORMAT "OTA image ready: version=%s project=%s"
#define OTA_BOOT_PARTITION_FAILED_FORMAT "OTA boot partition failed: %s"
static constexpr const char *kOtaTaskEventGroupUnavailableLog = "OTA task stopped: event group unavailable";
#define OTA_UPDATE_CHECK_FORMAT "OTA update check source=%s remote=%s current=%s"
static constexpr const char *kOtaPrimaryDownloadRetryBackupLog =
    "OTA primary download failed, retrying GitHub backup";

static const char *ota_request_name_or_fallback(const char *name)
{
    return cstr_nonempty(name) ? name : kOtaRequestFallbackName;
}

static const char *ota_status_text_or_fallback(const char *text)
{
    return cstr_nonempty(text) ? text : kOtaStatusFallbackError;
}

static void format_ota_status_text(char *out, size_t out_len, const char *fmt, ...)
{
    if (!app_text::output_buffer_available(out, out_len)) {
        return;
    }
    if (!fmt) {
        strlcpy(out, kOtaStatusFallbackError, out_len);
        return;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out, out_len, fmt, args);
    va_end(args);
    if (app_text::format_failed(written, out_len)) {
        strlcpy(out, kOtaStatusFallbackError, out_len);
    }
}

static void log_ota_heap(const char *stage, int downloaded, int progress)
{
    ESP_LOGI(TAG,
             OTA_HEAP_DIAGNOSTIC_FORMAT,
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

static void ota_note_phase(int phase, int total, int progress)
{
    s_ota_breadcrumb.magic = kOtaBreadcrumbMagic;
    s_ota_breadcrumb.phase = phase;
    s_ota_breadcrumb.total = total;
    s_ota_breadcrumb.progress = progress;
}

static void ota_set_status(int state, const char *text, int progress = -1, uint32_t hold_ms = 0)
{
    TickType_t status_until_tick = 0;
    if (hold_ms > 0) {
        status_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(hold_ms);
    }
    ota_runtime_publish_status(state,
                               ota_status_text_or_fallback(text),
                               progress,
                               status_until_tick,
                               hold_ms > 0);
    notify_ui_task();
}

static void ota_set_download_status(const char *text, int progress, int speed_kbps)
{
    ota_runtime_publish_download_status(ota_status_text_or_fallback(text),
                                        progress,
                                        speed_kbps);
    notify_ui_task();
}

static void ota_set_failed_status(const char *text, uint32_t hold_ms = kOtaFailureHoldMs)
{
    ota_set_status(kOtaFailed, text, -1, hold_ms);
}

static void ota_set_manifest_check_failed_status()
{
    ota_set_failed_status(kOtaStatusCheckFailed);
}

static void enter_ota_reboot_quiet_window()
{
    ota_runtime_reboot_pending_store(true);
    notify_ui_task();
    vTaskDelay(pdMS_TO_TICKS(kOtaPreRestartDisplayQuietMs));
}

static bool set_ota_event_bit(EventBits_t bit, const char *name)
{
    if (!app_event_group_ready()) {
        ESP_LOGW(TAG, OTA_REQUEST_EVENT_GROUP_UNAVAILABLE_FORMAT, ota_request_name_or_fallback(name));
        ota_set_failed_status(kOtaStatusUnavailable);
        return false;
    }
    app_event_group_set_bits(bit);
    return true;
}

static void keep_ota_settings_panel_visible()
{
    TickType_t now = xTaskGetTickCount();
    settings_page_request();
    enter_settings_system_item_navigation(kSystemSettingsOtaItem);
    settings_activity_record(now);
    info_page_hold_until_store(0);
}

static void hold_ota_info_page(uint32_t hold_ms = kOtaFailureHoldMs)
{
    info_page_hold_until_store(xTaskGetTickCount() + pdMS_TO_TICKS(hold_ms));
}

static bool ota_flow_active_at(TickType_t now)
{
    OtaRuntimeTimingSnapshot runtime = {};
    ota_runtime_timing_snapshot_load(&runtime);
    return ota_flow_active_for_tick(
        runtime.state,
        runtime.status_hold_set,
        now,
        runtime.status_until_tick);
}

bool ota_flow_active()
{
    return ota_flow_active_at(xTaskGetTickCount());
}

void ota_reset_status_if_idle()
{
    ota_runtime_reset_status_if_idle(xTaskGetTickCount(), kOtaStatusIdlePrompt);
}

void ota_handle_info_key()
{
    ota_reset_status_if_idle();
    if (offline_mode_enabled_load()) {
        keep_ota_settings_panel_visible();
        ota_set_failed_status(kOtaStatusOfflineMode, kOtaOfflineHoldMs);
        return;
    }
    int ota_state = ota_runtime_state_load();
    if (ota_state == kOtaChecking || ota_state == kOtaUpdating) {
        return;
    }
    keep_ota_settings_panel_visible();
    if (ota_state == kOtaAvailable) {
        if (!set_ota_event_bit(kOtaInstallBit, "install")) {
            return;
        }
        ota_set_download_status(kOtaStatusInstallingUpdate, 0, -1);
        info_page_hold_until_store(0);
        return;
    }
    if (!set_ota_event_bit(kOtaCheckBit, "check")) {
        return;
    }
    ota_set_status(kOtaChecking, kOtaStatusCheckingUpdate);
    info_page_hold_until_store(0);
}

void ota_mark_running_app_valid()
{
    if (s_ota_breadcrumb.magic == kOtaBreadcrumbMagic) {
        ESP_LOGW(TAG,
                 OTA_PREVIOUS_BREADCRUMB_FORMAT,
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
            ESP_LOGI(TAG, "%s", kOtaAppMarkedValidLog);
        } else {
            ESP_LOGW(TAG, OTA_APP_VALID_MARK_FAILED_FORMAT, esp_err_to_name(err));
        }
    }
}

static void report_ota_download_progress(int total,
                                         int expected,
                                         int64_t started_us,
                                         OtaDownloadProgressState &state)
{
    if (expected <= 0) {
        return;
    }

    const int progress = ota_download_progress_percent(total, expected);
    if (ota_download_progress_note_due(state, progress)) {
        ota_note_phase(3, total, progress);
        ota_download_progress_mark_noted(state, progress);
    }
    if (ota_download_heap_log_due(state, progress)) {
        log_ota_heap("download", total, progress);
        ota_download_progress_mark_heap_logged(state, progress);
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t status_interval_us =
        static_cast<int64_t>(kOtaStatusMinIntervalMs) * kOtaUsPerMs;
    if (!ota_download_status_due(state,
                                 now_us,
                                 status_interval_us,
                                 progress)) {
        return;
    }

    const int speed_window_bytes = ota_download_status_window_bytes(state,
                                                                    total);
    const int64_t speed_window_us = ota_download_status_window_us(state,
                                                                  started_us,
                                                                  now_us);
    const int speed_kbps = ota_speed_kbps_for_window(speed_window_bytes,
                                                     speed_window_us);
    char status_text[kOtaDownloadStatusTextLen] = {};
    format_ota_status_text(status_text,
                           sizeof(status_text),
                           kOtaStatusInstallingProgressFormat,
                           progress,
                           speed_kbps);
    ota_set_download_status(status_text, progress, speed_kbps);
    ota_download_progress_mark_status_published(state, now_us, total);
}

static void log_ota_download_start(const OtaManifest &manifest)
{
    wifi_ap_record_t ap_info = {};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    BatteryRuntimeSnapshot battery = {};
    (void)battery_runtime_snapshot_load(&battery);
    ESP_LOGI(TAG,
             OTA_DOWNLOAD_START_FORMAT,
             (int)esp_reset_reason(),
             battery.percent,
             battery.voltage,
             rssi,
             manifest.size,
             static_cast<unsigned>(strlen(manifest.url)));
    log_ota_heap("start", 0, 0);
}

static bool verify_downloaded_ota_sha(const uint8_t *hash,
                                      const OtaManifest &manifest)
{
    char actual_sha[kOtaSha256Len] = {};
    ota_sha256_to_hex(hash, actual_sha, sizeof(actual_sha));
    if (strcasecmp(actual_sha, manifest.sha256) == 0) {
        return true;
    }
    ESP_LOGW(TAG, OTA_SHA_MISMATCH_FORMAT, manifest.sha256, actual_sha);
    ota_set_failed_status(kOtaStatusVerifyFailed);
    return false;
}

static bool validate_downloaded_ota_description(
    const esp_partition_t *update_partition,
    const OtaManifest &manifest)
{
    esp_app_desc_t app_desc = {};
    esp_err_t err =
        esp_ota_get_partition_description(update_partition, &app_desc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, OTA_APP_DESCRIPTION_FAILED_FORMAT, esp_err_to_name(err));
        ota_set_failed_status(kOtaStatusVerifyFailed);
        return false;
    }
    if (!ota_app_description_matches_expected(
            app_desc.version,
            sizeof(app_desc.version),
            app_desc.project_name,
            sizeof(app_desc.project_name),
            manifest.version,
            kOtaExpectedProjectName)) {
        ESP_LOGW(TAG,
                 OTA_IMAGE_METADATA_MISMATCH_FORMAT,
                 manifest.version,
                 static_cast<int>(sizeof(app_desc.version)),
                 app_desc.version,
                 kOtaExpectedProjectName,
                 static_cast<int>(sizeof(app_desc.project_name)),
                 app_desc.project_name);
        ota_set_failed_status(kOtaStatusVerifyFailed);
        return false;
    }
    ESP_LOGI(TAG, OTA_IMAGE_READY_FORMAT, app_desc.version, app_desc.project_name);
    return true;
}

static OtaInstallAttemptResult stream_ota_image(
    esp_http_client_handle_t client,
    esp_ota_handle_t ota_handle,
    const OtaManifest &manifest,
    int content_len,
    ScopedHeapBuffer<uint8_t> &buffer,
    mbedtls_sha256_context &sha_ctx,
    OtaTaskWatchdogGuard &wdt,
    int64_t started_us,
    int &total)
{
    OtaDownloadProgressState progress_state;
    int64_t last_progress_us = started_us;
    for (;;) {
        wdt.reset();
        int64_t now_us = esp_timer_get_time();
        if (now_us - started_us >
            static_cast<int64_t>(kOtaMaxDownloadMs) * kOtaUsPerMs) {
            ESP_LOGW(TAG, OTA_DOWNLOAD_TIMEOUT_FORMAT, total);
            return OtaInstallAttemptResult::kRetryBackupSource;
        }
        int read = esp_http_client_read(client,
                                        reinterpret_cast<char *>(buffer.data()),
                                        static_cast<int>(buffer.size()));
        wdt.reset();
        if (read < 0) {
            if (esp_timer_get_time() - last_progress_us >
                static_cast<int64_t>(kOtaNoProgressTimeoutMs) * kOtaUsPerMs) {
                ESP_LOGW(TAG, OTA_READ_FAILED_NO_PROGRESS_FORMAT, total);
                return OtaInstallAttemptResult::kRetryBackupSource;
            }
            vTaskDelay(kOtaReadRetryDelay);
            continue;
        }
        if (read == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                return OtaInstallAttemptResult::kSucceeded;
            }
            if (esp_timer_get_time() - last_progress_us >
                static_cast<int64_t>(kOtaNoProgressTimeoutMs) * kOtaUsPerMs) {
                ESP_LOGW(TAG, OTA_STALLED_FORMAT, total);
                return OtaInstallAttemptResult::kRetryBackupSource;
            }
            vTaskDelay(kOtaReadRetryDelay);
            continue;
        }
        last_progress_us = esp_timer_get_time();
        mbedtls_sha256_update(&sha_ctx, buffer.data(), read);
        esp_err_t err = esp_ota_write(ota_handle, buffer.data(), read);
        wdt.reset();
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     OTA_WRITE_FAILED_FORMAT,
                     total,
                     read,
                     esp_err_to_name(err));
            return OtaInstallAttemptResult::kTerminalFailure;
        }
        vTaskDelay(pdMS_TO_TICKS(kOtaChunkDelayMs));
        total += read;
        int expected = content_len > 0 ? content_len : manifest.size;
        report_ota_download_progress(total,
                                     expected,
                                     started_us,
                                     progress_state);
    }
}

static OtaInstallAttemptResult download_and_apply_ota(
    const OtaManifest &manifest)
{
    if (manifest.url[0] == '\0' || !ota_valid_sha256_string(manifest.sha256)) {
        ESP_LOGW(TAG, "%s", kOtaManifestInvalidForInstallLog);
        ota_set_failed_status(kOtaStatusDownloadFailed);
        return OtaInstallAttemptResult::kRetryBackupSource;
    }
    ota_note_phase(1, 0, 0);
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        ota_set_failed_status(kOtaStatusNoOtaSlot);
        return OtaInstallAttemptResult::kTerminalFailure;
    }

    log_ota_download_start(manifest);

    NetworkHttpTransactionGuard transaction_lock(
        pdMS_TO_TICKS(kOtaWifiConnectTimeoutMs));
    if (!transaction_lock.locked()) {
        ESP_LOGW(TAG, "%s", kOtaHttpTransactionLockTimeoutLog);
        ota_set_failed_status(kOtaStatusDownloadFailed);
        return OtaInstallAttemptResult::kTerminalFailure;
    }
    OtaDownloadHttpSession http_session;
    if (!http_session.open(manifest.url)) {
        ota_set_failed_status(kOtaStatusDownloadFailed);
        return OtaInstallAttemptResult::kRetryBackupSource;
    }
    esp_http_client_handle_t client = http_session.handle();
    int content_len = http_session.content_length();

    esp_ota_handle_t ota_handle = 0;
    ota_note_phase(2, 0, 0);
    esp_err_t err = esp_ota_begin(update_partition,
                                  manifest.size > 0 ? manifest.size : OTA_SIZE_UNKNOWN,
                                  &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, OTA_BEGIN_FAILED_FORMAT, esp_err_to_name(err));
        ota_set_failed_status(kOtaStatusUpdateFailed);
        return OtaInstallAttemptResult::kTerminalFailure;
    }
    OtaWriteHandleGuard ota_writer(ota_handle);

    ScopedHeapBuffer<uint8_t> buffer(kOtaDownloadBufferSize);
    if (!buffer) {
        ESP_LOGW(TAG,
                 OTA_DOWNLOAD_BUFFER_ALLOC_FAILED_FORMAT,
                 static_cast<unsigned>(kOtaDownloadBufferSize));
        ota_set_failed_status(kOtaStatusNoMemory);
        return OtaInstallAttemptResult::kTerminalFailure;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    int total = 0;
    int64_t started_us = esp_timer_get_time();
    OtaTaskWatchdogGuard wdt;
    OtaInstallAttemptResult stream_result =
        stream_ota_image(client,
                         ota_writer.handle(),
                         manifest,
                         content_len,
                         buffer,
                         sha_ctx,
                         wdt,
                         started_us,
                         total);

    uint8_t hash[kOtaSha256ByteCount];
    wdt.reset();
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);
    bool complete = esp_http_client_is_complete_data_received(client);
    http_session.close();

    if (!ota_install_attempt_succeeded(stream_result) || !complete) {
        ota_set_failed_status(kOtaStatusDownloadFailed);
        return ota_install_attempt_succeeded(stream_result)
                   ? OtaInstallAttemptResult::kRetryBackupSource
                   : stream_result;
    }
    buffer.reset();

    ota_note_phase(4, total, 100);
    if (!verify_downloaded_ota_sha(hash, manifest)) {
        return OtaInstallAttemptResult::kRetryBackupSource;
    }

    wdt.reset();
    ota_note_phase(5, total, 100);
    err = ota_writer.finish();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, OTA_END_FAILED_FORMAT, esp_err_to_name(err));
        ota_set_failed_status(kOtaStatusUpdateFailed);
        return OtaInstallAttemptResult::kTerminalFailure;
    }
    if (!validate_downloaded_ota_description(update_partition, manifest)) {
        return OtaInstallAttemptResult::kTerminalFailure;
    }
    wdt.reset();
    ota_note_phase(6, total, 100);
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, OTA_BOOT_PARTITION_FAILED_FORMAT, esp_err_to_name(err));
        ota_set_failed_status(kOtaStatusUpdateFailed);
        return OtaInstallAttemptResult::kTerminalFailure;
    }

    s_ota_breadcrumb.magic = 0;
    return OtaInstallAttemptResult::kSucceeded;
}

static void ota_set_network_start_failure_status(
    OtaNetworkSessionStartResult result)
{
    switch (result) {
    case OtaNetworkSessionStartResult::kOfflineMode:
        ota_set_failed_status(kOtaStatusOfflineMode, kOtaOfflineHoldMs);
        break;
    case OtaNetworkSessionStartResult::kNoWifi:
        ota_set_failed_status(kOtaStatusNoWifi);
        break;
    case OtaNetworkSessionStartResult::kLowBattery:
        ota_set_failed_status(kOtaStatusLowBattery);
        break;
    case OtaNetworkSessionStartResult::kSetupPortal:
        ota_set_failed_status(kOtaStatusSetupMode);
        break;
    case OtaNetworkSessionStartResult::kAwakeLockUnavailable:
    case OtaNetworkSessionStartResult::kRadioStartFailed:
    case OtaNetworkSessionStartResult::kConnectionTimedOut:
        ota_set_failed_status(kOtaStatusWifiFailed);
        break;
    case OtaNetworkSessionStartResult::kReady:
        break;
    }
}

static void handle_ota_check_request()
{
    OtaManifestWorkspaceGuard manifest_workspace;
    OtaManifest &manifest = manifest_workspace.primary();
    ota_set_status(kOtaChecking, kOtaStatusCheckingUpdate);
    char manifest_source[kOtaManifestSourceNameLen] = {};
    if (!ota_manifest_fetch(&manifest,
                            manifest_source,
                            sizeof(manifest_source),
                            ota_set_manifest_check_failed_status)) {
        ota_network_session_finish(OtaWifiFinishPolicy::kReleaseAwakeLock);
        hold_ota_info_page();
        return;
    }
    ESP_LOGI(TAG,
             OTA_UPDATE_CHECK_FORMAT,
             ota_manifest_source_name_or_unknown(manifest_source),
             manifest.version,
             APP_VERSION);
    if (ota_compare_versions(manifest.version, APP_VERSION) <= 0) {
        ota_set_status(kOtaNoUpdate, kOtaStatusAlreadyLatest, -1, kOtaFailureHoldMs);
        ota_network_session_finish(OtaWifiFinishPolicy::kReleaseAwakeLock);
        hold_ota_info_page();
        return;
    }
    ota_manifest_store_cached(manifest);
    char status_text[kOtaStatusLen] = {};
    format_ota_status_text(status_text,
                           sizeof(status_text),
                           kOtaStatusNewVersionFormat,
                           manifest.version);
    ota_set_status(kOtaAvailable,
                   status_text,
                   -1,
                   kOtaAvailableConfirmTimeoutMs);
    ota_network_session_finish(OtaWifiFinishPolicy::kReleaseAwakeLock);
}

static void handle_ota_install_request()
{
    OtaManifestWorkspaceGuard manifest_workspace;
    OtaManifest &manifest = manifest_workspace.primary();
    ota_manifest_load_cached(&manifest);
    bool ok = false;
    {
        OtaDisplayQuietGuard display_quiet;
        OtaInstallAttemptResult attempt = download_and_apply_ota(manifest);
        ok = ota_install_attempt_succeeded(attempt);
        if (ota_install_backup_allowed(attempt)) {
            OtaManifest &backup_manifest = manifest_workspace.backup();
            if (ota_manifest_fetch_backup_for_install(
                    manifest,
                    &backup_manifest,
                    ota_set_manifest_check_failed_status) &&
                strcmp(backup_manifest.url, manifest.url) != 0) {
                ESP_LOGW(TAG, "%s", kOtaPrimaryDownloadRetryBackupLog);
                ota_set_status(kOtaUpdating, kOtaStatusInstallingBackup, 0);
                attempt = download_and_apply_ota(backup_manifest);
                ok = ota_install_attempt_succeeded(attempt);
            }
        }
        manifest_workspace.clear();
        ota_network_session_finish(ota_wifi_finish_policy(ok));
        if (ok) {
            keep_ota_settings_panel_visible();
            ota_set_status(kOtaSucceeded,
                           kOtaStatusUpdateDoneRebooting,
                           100,
                           kOtaSuccessHoldMs);
            vTaskDelay(pdMS_TO_TICKS(kOtaRebootNoticeDelayMs));
            enter_ota_reboot_quiet_window();
            esp_restart();
            ota_network_session_finish(
                OtaWifiFinishPolicy::kReleaseAwakeLock);
        }
    }
    if (!ok) {
        hold_ota_info_page();
    }
}

void ota_task(void *)
{
    if (!app_event_group_ready()) {
        ESP_LOGW(TAG, "%s", kOtaTaskEventGroupUnavailableLog);
        vTaskDelete(nullptr);
        return;
    }
    regular_app_task_mark_ready(RegularAppTaskId::kOta);
    for (;;) {
        runtime_health_record_current_task_stack(RegularAppTaskId::kOta);
        EventBits_t bits = app_event_group_wait_bits(kOtaCheckBit | kOtaInstallBit,
                                                     pdTRUE,
                                                     pdFALSE,
                                                     portMAX_DELAY);
        bool install = (bits & kOtaInstallBit) != 0;
        bool check = (bits & kOtaCheckBit) != 0;
        if (!install && !check) {
            continue;
        }

        const OtaNetworkSessionStartResult network_result =
            ota_network_session_start(kOtaWifiConnectTimeoutMs);
        if (network_result != OtaNetworkSessionStartResult::kReady) {
            ota_set_network_start_failure_status(network_result);
            hold_ota_info_page();
            continue;
        }

        if (install) {
            handle_ota_install_request();
        } else {
            handle_ota_check_request();
        }
    }
}
