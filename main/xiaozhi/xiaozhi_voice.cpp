// 使用 ESP-SR 模型分区监听唤醒词，采集链路由现有音频服务统一仲裁。
#include "xiaozhi_voice.h"

#include "app_tick_time.h"
#include "audio_services.h"
#include "checked_size.h"
#include "xiaozhi_voice_read_health.h"

#include <esp_codec_dev_types.h>
#include <esp_afe_config.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <model_path.h>

#include <atomic>
#include <cstring>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

namespace {
constexpr const char *kTag = "XiaozhiVoice";
constexpr int kMicChannels = 2;
constexpr uint32_t kFeedTaskStackBytes = 6144;
constexpr uint32_t kDetectTaskStackBytes = 4096;
constexpr UBaseType_t kTaskPriority = 5;
constexpr TickType_t kFetchWaitTicks = pdMS_TO_TICKS(100);
constexpr int kTaskStopRetries = 100;
constexpr uint32_t kTaskStopWaitMs = 20;
constexpr TickType_t kTaskStopWaitTicks = pdMS_TO_TICKS(kTaskStopWaitMs);
constexpr float kWakeNetThreshold = 0.60f;
constexpr TickType_t kLevelLogIntervalTicks = pdMS_TO_TICKS(3000);
constexpr TickType_t kFetchWarningIntervalTicks = pdMS_TO_TICKS(3000);
constexpr uint32_t kStreamFullWarningIntervalMs = 1000;
constexpr uint32_t kMicrophoneReadRetryDelayMs = 40;
constexpr uint32_t kMicrophoneReadFailureLimit = 25;
constexpr TickType_t kStreamFullWarningIntervalTicks =
    pdMS_TO_TICKS(kStreamFullWarningIntervalMs);
constexpr size_t kProcessedStreamBytes = 16 * 1024;
constexpr const char *kProcessedReadSizeOverflowLog =
    "AEC processed read sample count overflow";
constexpr const char *kCaptureSizeOverflowLog =
    "MR AEC capture size overflow";
#define XIAOZHI_VOICE_TASK_STOP_TIMEOUT_FORMAT "MR AEC task stop timeout: feed_pending=%d detect_pending=%d"

std::atomic<bool> s_running{false};
std::atomic<bool> s_detected{false};
std::atomic<bool> s_streaming{false};
std::atomic<bool> s_feed_exited{true};
std::atomic<bool> s_detect_exited{true};
std::atomic<XiaozhiVoiceEventCallback> s_event_callback{nullptr};
TaskHandle_t s_feed_task = nullptr;
TaskHandle_t s_detect_task = nullptr;
StackType_t *s_feed_task_stack = nullptr;
StackType_t *s_detect_task_stack = nullptr;
StaticTask_t *s_feed_task_buffer = nullptr;
StaticTask_t *s_detect_task_buffer = nullptr;
std::atomic<int16_t *> s_capture_buffer{nullptr};
int s_capture_codec_bytes = 0;
int s_capture_chunk_samples = 0;
int s_capture_channels = 0;
srmodel_list_t *s_models = nullptr;
const esp_afe_sr_iface_t *s_afe_iface = nullptr;
esp_afe_sr_data_t *s_afe_data = nullptr;
StreamBufferHandle_t s_processed_stream = nullptr;
uint8_t *s_processed_stream_storage = nullptr;
StaticStreamBuffer_t *s_processed_stream_control = nullptr;

void notify_voice_event()
{
    XiaozhiVoiceEventCallback callback =
        s_event_callback.load(std::memory_order_acquire);
    if (callback) {
        callback();
    }
}

void report_voice_runtime_failure()
{
    s_running.store(false);
    notify_voice_event();
}

bool ensure_processed_stream()
{
    if (s_processed_stream) {
        xStreamBufferReset(s_processed_stream);
        return true;
    }
    s_processed_stream_storage = static_cast<uint8_t *>(heap_caps_calloc(
        1, kProcessedStreamBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_processed_stream_control = static_cast<StaticStreamBuffer_t *>(heap_caps_calloc(
        1, sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_processed_stream_storage && s_processed_stream_control) {
        s_processed_stream = xStreamBufferCreateStatic(kProcessedStreamBytes,
                                                        1,
                                                        s_processed_stream_storage,
                                                        s_processed_stream_control);
    }
    if (!s_processed_stream) {
        ESP_LOGW(kTag,
                 "AEC output stream allocation failed: storage=%p control=%p "
                 "internal_largest=%u psram_largest=%u",
                 s_processed_stream_storage,
                 s_processed_stream_control,
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        free(s_processed_stream_storage);
        free(s_processed_stream_control);
        s_processed_stream_storage = nullptr;
        s_processed_stream_control = nullptr;
        return false;
    }
    ESP_LOGI(kTag,
             "AEC output stream ready: psram=%u control_internal=%u",
             static_cast<unsigned>(kProcessedStreamBytes),
             static_cast<unsigned>(sizeof(StaticStreamBuffer_t)));
    return true;
}

static_assert(kTaskStopRetries > 0, "voice task stop retries must be positive");
static_assert(kTaskStopWaitMs > 0, "voice task stop wait must be positive");
static_assert(kTaskStopWaitTicks > 0, "voice task stop wait tick conversion must be positive");
static_assert(kLevelLogIntervalTicks > 0, "voice level log interval must be positive");
static_assert(kFetchWarningIntervalTicks > 0, "voice fetch warning interval must be positive");
static_assert(kStreamFullWarningIntervalMs > 0,
              "voice stream-full warning interval must be positive");
static_assert(kStreamFullWarningIntervalTicks > 0,
              "voice stream-full warning tick conversion must be positive");
static_assert(kMicrophoneReadRetryDelayMs > 0,
              "microphone read retry delay must be positive");
static_assert(kMicrophoneReadFailureLimit > 0,
              "microphone read failure limit must be positive");

void release_task_storage()
{
    free(s_feed_task_stack);
    free(s_detect_task_stack);
    free(s_feed_task_buffer);
    free(s_detect_task_buffer);
    s_feed_task_stack = nullptr;
    s_detect_task_stack = nullptr;
    s_feed_task_buffer = nullptr;
    s_detect_task_buffer = nullptr;
}

bool allocate_task_storage()
{
    release_task_storage();
    // ESP-IDF accepts the static stack depth in bytes.  Keeping the large stacks
    // in PSRAM avoids depending on a pair of contiguous internal-RAM blocks after
    // ESP-SR has created its AFE/AEC pipeline.  TCBs must remain in internal RAM.
    s_feed_task_stack = static_cast<StackType_t *>(heap_caps_calloc(
        1, kFeedTaskStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_detect_task_stack = static_cast<StackType_t *>(heap_caps_calloc(
        1, kDetectTaskStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_feed_task_buffer = static_cast<StaticTask_t *>(heap_caps_calloc(
        1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_detect_task_buffer = static_cast<StaticTask_t *>(heap_caps_calloc(
        1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_feed_task_stack || !s_detect_task_stack ||
        !s_feed_task_buffer || !s_detect_task_buffer) {
        ESP_LOGW(kTag,
                 "MR AEC task storage allocation failed: feed_stack=%p detect_stack=%p "
                 "feed_tcb=%p detect_tcb=%p",
                 s_feed_task_stack,
                 s_detect_task_stack,
                 s_feed_task_buffer,
                 s_detect_task_buffer);
        release_task_storage();
        return false;
    }
    ESP_LOGI(kTag,
             "MR AEC task storage: feed_psram=%u detect_psram=%u tcb_internal=%u",
             static_cast<unsigned>(kFeedTaskStackBytes),
             static_cast<unsigned>(kDetectTaskStackBytes),
             static_cast<unsigned>(sizeof(StaticTask_t) * 2));
    return true;
}

void release_model()
{
    if (s_afe_data && s_afe_iface) {
        s_afe_iface->destroy(s_afe_data);
    }
    s_afe_data = nullptr;
    s_afe_iface = nullptr;
    if (s_models) {
        esp_srmodel_deinit(s_models);
    }
    s_models = nullptr;
}

bool create_model()
{
    s_models = esp_srmodel_init("model");
    if (!s_models || s_models->num <= 0) {
        ESP_LOGW(kTag, "WakeNet model partition is unavailable");
        release_model();
        return false;
    }
    // 官方同板卡使用一路麦克风与一路播放参考（MR），设备端 AEC 后的
    // 单声道结果同时供 WakeNet 和 realtime Opus 上行使用。
    afe_config_t *config = afe_config_init("MR", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!config) {
        ESP_LOGW(kTag, "MR AEC configuration failed");
        release_model();
        return false;
    }
    config->aec_init = true;
    config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->afe_perferred_core = 1;
    config->afe_perferred_priority = kTaskPriority;
    config = afe_config_check(config);
    s_afe_iface = config ? esp_afe_handle_from_config(config) : nullptr;
    s_afe_data = s_afe_iface ? s_afe_iface->create_from_config(config) : nullptr;
    char model_name[64] = {};
    if (config && config->wakenet_model_name) {
        strlcpy(model_name, config->wakenet_model_name, sizeof(model_name));
    }
    afe_config_free(config);
    if (!s_afe_iface || !s_afe_data) {
        ESP_LOGW(kTag, "MR AEC WakeNet initialization failed");
        release_model();
        return false;
    }
    // 阈值越低越灵敏，0.3 比模型默认值更容易触发
    (void)s_afe_iface->set_wakenet_threshold(s_afe_data, 1, 0.20f);
    s_afe_iface->print_pipeline(s_afe_data);
    ESP_LOGI(kTag,
             "MR AEC WakeNet ready: %s (%d Hz, feed=%d, fetch=%d, channels=%d)",
             model_name[0] ? model_name : "unknown",
             s_afe_iface->get_samp_rate(s_afe_data),
             s_afe_iface->get_feed_chunksize(s_afe_data),
             s_afe_iface->get_fetch_chunksize(s_afe_data),
             s_afe_iface->get_feed_channel_num(s_afe_data));
    return true;
}

void release_capture_storage()
{
    free(s_capture_buffer.exchange(nullptr, std::memory_order_acq_rel));
    s_capture_codec_bytes = 0;
    s_capture_chunk_samples = 0;
    s_capture_channels = 0;
}

bool allocate_capture_storage()
{
    release_capture_storage();
    const int chunk_samples =
        s_afe_iface ? s_afe_iface->get_feed_chunksize(s_afe_data) : 0;
    const int channels =
        s_afe_iface ? s_afe_iface->get_feed_channel_num(s_afe_data) : 0;
    if (chunk_samples <= 0 || channels != kMicChannels) {
        ESP_LOGW(kTag,
                 "Unexpected AFE feed format: samples=%d channels=%d",
                 chunk_samples,
                 channels);
        return false;
    }
    size_t capture_samples = 0;
    size_t capture_bytes = 0;
    int codec_bytes = 0;
    if (!app_memory::checked_size_multiply(static_cast<size_t>(chunk_samples),
                                           static_cast<size_t>(channels),
                                           &capture_samples) ||
        !app_memory::checked_size_multiply(capture_samples,
                                           sizeof(int16_t),
                                           &capture_bytes) ||
        !app_memory::checked_size_to_int(capture_bytes, &codec_bytes)) {
        ESP_LOGW(kTag, "%s", kCaptureSizeOverflowLog);
        return false;
    }
    int16_t *capture_buffer = static_cast<int16_t *>(heap_caps_malloc(
        capture_bytes, MALLOC_CAP_SPIRAM));
    if (!capture_buffer) {
        ESP_LOGW(kTag, "MR AEC capture buffer allocation failed");
        return false;
    }
    s_capture_codec_bytes = codec_bytes;
    s_capture_chunk_samples = chunk_samples;
    s_capture_channels = channels;
    s_capture_buffer.store(capture_buffer, std::memory_order_release);
    ESP_LOGI(kTag,
             "MR AEC capture storage: psram=%u samples=%d channels=%d",
             static_cast<unsigned>(capture_bytes),
             chunk_samples,
             channels);
    return true;
}

void feed_task(void *)
{
    int16_t *stereo = s_capture_buffer.load(std::memory_order_acquire);
    const int codec_bytes = s_capture_codec_bytes;
    const int chunk_samples = s_capture_chunk_samples;
    const int channels = s_capture_channels;
    if (!stereo || codec_bytes <= 0 || chunk_samples <= 0 ||
        channels != kMicChannels) {
        ESP_LOGW(kTag, "MR AEC capture storage is unavailable");
        report_voice_runtime_failure();
        s_feed_exited.store(true);
        vTaskSuspend(nullptr);
        return;
    }
    TickType_t next_level_log = xTaskGetTickCount() + kLevelLogIntervalTicks;
    uint32_t consecutive_read_failures = 0;
    while (s_running.load()) {
        int result = read_xiaozhi_microphone(stereo,
                                             static_cast<size_t>(codec_bytes));
        if (result != ESP_CODEC_DEV_OK) {
            XiaozhiVoiceReadHealthResult health =
                xiaozhi_voice_read_health_after_result(
                    consecutive_read_failures,
                    false,
                    kMicrophoneReadFailureLimit);
            consecutive_read_failures = health.consecutive_failures;
            if (health.should_log) {
                ESP_LOGW(kTag,
                         "Microphone read failed: %d consecutive=%u",
                         result,
                         static_cast<unsigned>(consecutive_read_failures));
            }
            if (health.should_rebuild) {
                report_voice_runtime_failure();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(kMicrophoneReadRetryDelayMs));
            continue;
        }
        consecutive_read_failures = 0;
        TickType_t now = xTaskGetTickCount();
        if (app_tick_deadline_reached(now, next_level_log)) {
            int peak_mic = 0;
            int peak_reference = 0;
            for (int sample = 0; sample < chunk_samples; ++sample) {
                int mic = abs(static_cast<int>(stereo[sample * channels]));
                int reference = abs(static_cast<int>(stereo[sample * channels + 1]));
                peak_mic = mic > peak_mic ? mic : peak_mic;
                peak_reference = reference > peak_reference ? reference : peak_reference;
            }
            ESP_LOGI(kTag, "MR AEC level: mic=%d reference=%d", peak_mic, peak_reference);
            next_level_log = now + kLevelLogIntervalTicks;
        }
        if (s_afe_iface->feed(s_afe_data, stereo) < 0) {
            ESP_LOGW(kTag, "AFE feed failed");
            report_voice_runtime_failure();
            break;
        }
    }
    s_feed_exited.store(true);
    vTaskSuspend(nullptr);
}

void detect_task(void *)
{
    TickType_t next_fetch_warning = 0;
    TickType_t next_stream_full_warning = 0;
    bool fetch_warning_scheduled = false;
    bool stream_full_warning_scheduled = false;
    uint32_t dropped_stream_frames = 0;
    while (s_running.load()) {
        afe_fetch_result_t *result = s_afe_iface->fetch_with_delay(s_afe_data, kFetchWaitTicks);
        if (!result) {
            continue;
        }
        if (result->ret_value == ESP_FAIL) {
            // Opening the full-duplex speaker for the wake feedback can briefly
            // starve AFE input. This is recoverable once I2S RX continues, so do
            // not tear down the entire voice session on a single empty fetch.
            TickType_t now = xTaskGetTickCount();
            if (!fetch_warning_scheduled ||
                app_tick_deadline_reached(now, next_fetch_warning)) {
                ESP_LOGW(kTag, "AFE fetch temporarily empty; listener kept alive");
                next_fetch_warning = now + kFetchWarningIntervalTicks;
                fetch_warning_scheduled = true;
            }
            continue;
        }
        if (s_streaming.load() && s_processed_stream && result->data && result->data_size > 0) {
            size_t sent = xStreamBufferSend(s_processed_stream,
                                            result->data,
                                            result->data_size,
                                            0);
            if (sent != result->data_size) {
                ++dropped_stream_frames;
                TickType_t now = xTaskGetTickCount();
                if (!stream_full_warning_scheduled ||
                    app_tick_deadline_reached(now, next_stream_full_warning)) {
                    ESP_LOGW(kTag,
                             "AEC output stream full: dropped=%u sent=%u expected=%u available=%u",
                             static_cast<unsigned>(dropped_stream_frames),
                             static_cast<unsigned>(sent),
                             static_cast<unsigned>(result->data_size),
                             static_cast<unsigned>(xStreamBufferBytesAvailable(s_processed_stream)));
                    dropped_stream_frames = 0;
                    next_stream_full_warning = now + kStreamFullWarningIntervalTicks;
                    stream_full_warning_scheduled = true;
                }
            }
        }
        if (result->wakeup_state == WAKENET_DETECTED) {
            s_afe_iface->disable_wakenet(s_afe_data);
            ESP_LOGI(kTag,
                     "Wake word detected: channel=%d volume=%.1f dB",
                     result->trigger_channel_id,
                     static_cast<double>(result->data_volume));
            s_detected.store(true);
            notify_voice_event();
        }
    }
    s_detect_exited.store(true);
    vTaskSuspend(nullptr);
}

void wait_for_tasks_to_stop()
{
    for (int retry = 0;
         ((!s_feed_exited.load() && s_feed_task) ||
         (!s_detect_exited.load() && s_detect_task)) &&
         retry < kTaskStopRetries;
         ++retry) {
        vTaskDelay(kTaskStopWaitTicks);
    }
    bool feed_pending = !s_feed_exited.load() && s_feed_task;
    bool detect_pending = !s_detect_exited.load() && s_detect_task;
    if (feed_pending || detect_pending) {
        ESP_LOGW(kTag,
                 XIAOZHI_VOICE_TASK_STOP_TIMEOUT_FORMAT,
                 feed_pending ? 1 : 0,
                 detect_pending ? 1 : 0);
    }
}

void delete_voice_tasks()
{
    if (s_feed_task) {
        vTaskDelete(s_feed_task);
        s_feed_task = nullptr;
    }
    if (s_detect_task) {
        vTaskDelete(s_detect_task);
        s_detect_task = nullptr;
    }
    s_feed_exited.store(true);
    s_detect_exited.store(true);
}
} // namespace

void xiaozhi_voice_set_event_callback(XiaozhiVoiceEventCallback callback)
{
    s_event_callback.store(callback, std::memory_order_release);
}

bool xiaozhi_voice_start()
{
    if (s_running.load()) {
        return true;
    }
    // A previous capture error can stop the workers before the page lifecycle
    // reaches xiaozhi_voice_stop(). Reclaim those suspended static tasks before
    // allocating a fresh pair of PSRAM stacks.
    if (s_feed_task || s_detect_task ||
        s_capture_buffer.load(std::memory_order_acquire)) {
        wait_for_tasks_to_stop();
        delete_voice_tasks();
        release_capture_storage();
        release_task_storage();
        release_model();
        stop_xiaozhi_audio_session();
    }
    s_detected.store(false);
    s_streaming.store(false);
    if (!ensure_processed_stream()) {
        return false;
    }
    if (!is_audio_playing() && !start_xiaozhi_audio_session()) {
        xStreamBufferReset(s_processed_stream);
        return false;
    }
    // A completed detection leaves the model allocated until the AI task has
    // consumed the event.  Re-arm cleanly before creating the next listener.
    release_model();
    if (!create_model()) {
        stop_xiaozhi_audio_session();
        xStreamBufferReset(s_processed_stream);
        return false;
    }
    if (!allocate_capture_storage()) {
        release_model();
        stop_xiaozhi_audio_session();
        xStreamBufferReset(s_processed_stream);
        return false;
    }
    if (!allocate_task_storage()) {
        release_capture_storage();
        release_model();
        stop_xiaozhi_audio_session();
        xStreamBufferReset(s_processed_stream);
        return false;
    }
    s_running.store(true);
    ESP_LOGI(kTag,
             "WakeNet task heap: internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    s_feed_exited.store(false);
    s_detect_exited.store(false);
    s_feed_task = xTaskCreateStaticPinnedToCore(
        feed_task, "xiaozhi_feed", kFeedTaskStackBytes, nullptr,
        kTaskPriority, s_feed_task_stack, s_feed_task_buffer, 0);
    if (s_feed_task) {
        s_detect_task = xTaskCreateStaticPinnedToCore(
            detect_task, "xiaozhi_detect", kDetectTaskStackBytes, nullptr,
            kTaskPriority, s_detect_task_stack, s_detect_task_buffer, 1);
    }
    if (!s_feed_task || !s_detect_task) {
        ESP_LOGW(kTag,
                 "MR AEC WakeNet task creation failed: feed=%d detect=%d "
                 "internal_free=%u internal_largest=%u",
                 s_feed_task ? 1 : 0,
                 s_detect_task ? 1 : 0,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        s_running.store(false);
        if (!s_feed_task) {
            s_feed_exited.store(true);
        }
        if (!s_detect_task) {
            s_detect_exited.store(true);
        }
        wait_for_tasks_to_stop();
        delete_voice_tasks();
        release_capture_storage();
        release_task_storage();
        release_model();
        stop_xiaozhi_audio_session();
        xStreamBufferReset(s_processed_stream);
        return false;
    }
    return true;
}

void xiaozhi_voice_stop()
{
    s_running.store(false);
    s_streaming.store(false);
    wait_for_tasks_to_stop();
    delete_voice_tasks();
    release_capture_storage();
    release_task_storage();
    release_model();
    if (s_processed_stream) {
        xStreamBufferReset(s_processed_stream);
    }
    stop_xiaozhi_audio_session();
}

bool xiaozhi_voice_take_wake_word()
{
    return s_detected.exchange(false);
}

void xiaozhi_voice_trigger_wake()
{
    if (!s_running.load()) {
        return;
    }
    bool was_false = s_detected.exchange(true);
    if (!was_false) {
        ESP_LOGI(kTag, "KEY button triggered wake");
        XiaozhiVoiceEventCallback cb = s_event_callback.load(std::memory_order_acquire);
        if (cb) {
            cb();
        }
    }
}

bool xiaozhi_voice_is_listening()
{
    return s_running.load();
}

void xiaozhi_voice_get_runtime_snapshot(XiaozhiVoiceRuntimeSnapshot *out)
{
    if (!out) {
        return;
    }
    out->running = s_running.load();
    out->streaming = s_streaming.load();
    out->feed_task = s_feed_task != nullptr;
    out->detect_task = s_detect_task != nullptr;
    out->afe = s_afe_iface != nullptr || s_afe_data != nullptr;
    out->model = s_models != nullptr;
    out->processed_stream = s_processed_stream != nullptr;
    out->capture_buffer =
        s_capture_buffer.load(std::memory_order_acquire) != nullptr;
}

void xiaozhi_voice_set_streaming(bool enabled)
{
    if (!s_running.load() || !s_afe_iface || !s_afe_data || !s_processed_stream) {
        return;
    }
    s_streaming.store(false);
    xStreamBufferReset(s_processed_stream);
    s_afe_iface->reset_buffer(s_afe_data);
    s_detected.store(false);
    s_afe_iface->enable_wakenet(s_afe_data);
    if (enabled) {
        s_streaming.store(true);
    }
    ESP_LOGI(kTag, "Xiaozhi AEC stream %s", enabled ? "realtime" : "wake-word");
}

void xiaozhi_voice_pause_streaming()
{
    if (!s_running.load() || !s_afe_iface || !s_afe_data || !s_processed_stream) {
        return;
    }
    s_streaming.store(false);
    xStreamBufferReset(s_processed_stream);
    s_detected.store(false);
    s_afe_iface->disable_wakenet(s_afe_data);
    ESP_LOGI(kTag, "Xiaozhi AEC stream paused for blocking operation");
}

bool xiaozhi_voice_read_processed(int16_t *mono_samples,
                                  size_t sample_count,
                                  uint32_t timeout_ms)
{
    if (!mono_samples || sample_count == 0 || !s_streaming.load() || !s_processed_stream) {
        return false;
    }
    uint8_t *out = reinterpret_cast<uint8_t *>(mono_samples);
    size_t expected = 0;
    if (!app_memory::checked_size_multiply(sample_count, sizeof(int16_t), &expected)) {
        ESP_LOGW(kTag, "%s", kProcessedReadSizeOverflowLog);
        return false;
    }
    size_t received = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (received < expected) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = app_tick_deadline_remaining(now, deadline);
        size_t chunk = xStreamBufferReceive(s_processed_stream,
                                            out + received,
                                            expected - received,
                                            wait);
        if (chunk == 0) {
            return false;
        }
        received += chunk;
    }
    return true;
}

size_t xiaozhi_voice_processed_bytes_available()
{
    if (!s_streaming.load() || !s_processed_stream) {
        return 0;
    }
    return xStreamBufferBytesAvailable(s_processed_stream);
}
