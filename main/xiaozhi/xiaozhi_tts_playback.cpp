// 管理小智 TTS PCM 队列、播放任务及其一次会话资源。
#include "xiaozhi_tts_playback.h"

#include "app_state.h"
#include "audio_services.h"
#include "checked_size.h"

#include <esp_codec_dev_types.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <atomic>
#include <stdlib.h>

namespace {
constexpr int kHardwareSampleRate = 16000;
constexpr size_t kStreamBytes = 32 * 1024;
constexpr size_t kChunkSamples = 960;
constexpr size_t kPrebufferBytes = kChunkSamples * sizeof(int16_t) * 3;
constexpr uint32_t kPrebufferWaitMs = 150;
constexpr uint32_t kTaskStackBytes = 6144;
constexpr UBaseType_t kTaskPriority = 5;
constexpr int kStopRetryCount = 200;
constexpr uint32_t kStopRetryDelayMs = 10;
constexpr const char *kStreamCreateFailedLog =
    "xiaozhi TTS stream buffer creation failed";
constexpr const char *kTaskCreateFailedLog =
    "xiaozhi TTS playback task creation failed";
constexpr const char *kWakeSignalCreateFailedLog =
    "xiaozhi TTS wake signal creation failed";
constexpr const char *kSampleCountOverflowLog =
    "xiaozhi TTS sample count overflow";
#define XIAOZHI_TTS_TASK_STOP_TIMEOUT_FORMAT \
    "xiaozhi TTS playback task stop timeout: retries=%d delay_ms=%u"

std::atomic<bool> s_running{false};
std::atomic<bool> s_exited{true};
std::atomic<bool> s_busy{false};
std::atomic<bool> s_failed{false};
TaskHandle_t s_task = nullptr;
StackType_t *s_task_stack = nullptr;
StaticTask_t *s_task_buffer = nullptr;
uint8_t *s_storage = nullptr;
StaticStreamBuffer_t *s_stream_buffer = nullptr;
StreamBufferHandle_t s_stream = nullptr;
StaticSemaphore_t s_wake_signal_storage = {};
SemaphoreHandle_t s_wake_signal = nullptr;

static_assert(kStreamBytes > kPrebufferBytes,
              "TTS stream must exceed its prebuffer threshold");
static_assert(kTaskStackBytes > 0, "TTS playback task stack must be positive");
static_assert(kStopRetryCount > 0 && kStopRetryDelayMs > 0,
              "TTS playback stop retry settings must be positive");

void release_storage()
{
    if (s_stream) {
        vStreamBufferDelete(s_stream);
    }
    s_stream = nullptr;
    free(s_storage);
    free(s_stream_buffer);
    free(s_task_stack);
    free(s_task_buffer);
    s_storage = nullptr;
    s_stream_buffer = nullptr;
    s_task_stack = nullptr;
    s_task_buffer = nullptr;
}

bool prepare_wake_signal()
{
    if (!s_wake_signal) {
        s_wake_signal = xSemaphoreCreateBinaryStatic(&s_wake_signal_storage);
        if (!s_wake_signal) {
            ESP_LOGW(TAG, "%s", kWakeSignalCreateFailedLog);
            return false;
        }
    }
    while (xSemaphoreTake(s_wake_signal, 0) == pdTRUE) {
    }
    return true;
}

void signal_playback_task()
{
    if (s_wake_signal) {
        (void)xSemaphoreGive(s_wake_signal);
    }
}

void playback_task(void *)
{
    int16_t pcm[kChunkSamples] = {};
    bool primed = false;
    TickType_t pending_since = 0;
    while (s_running.load()) {
        size_t available = s_stream ? xStreamBufferBytesAvailable(s_stream) : 0;
        if (!primed) {
            if (available == 0) {
                pending_since = 0;
                (void)xSemaphoreTake(s_wake_signal, portMAX_DELAY);
                continue;
            }
            TickType_t now = xTaskGetTickCount();
            if (pending_since == 0) {
                pending_since = now;
            }
            TickType_t elapsed = now - pending_since;
            const TickType_t prebuffer_wait = pdMS_TO_TICKS(kPrebufferWaitMs);
            if (available < kPrebufferBytes && elapsed < prebuffer_wait) {
                (void)xSemaphoreTake(s_wake_signal, prebuffer_wait - elapsed);
                continue;
            }
            primed = true;
        }
        // 先标记 busy，避免取走最后一块数据后会话线程提前关闭扬声器。
        s_busy.store(true);
        size_t received = xStreamBufferReceive(s_stream,
                                               pcm,
                                               sizeof(pcm),
                                               pdMS_TO_TICKS(100));
        if (received == 0) {
            s_busy.store(false);
            primed = false;
            pending_since = 0;
            continue;
        }
        received -= received % sizeof(int16_t);
        if (received == 0) {
            s_busy.store(false);
            continue;
        }
        int result = write_xiaozhi_speaker(pcm,
                                           received / sizeof(int16_t),
                                           kHardwareSampleRate);
        s_busy.store(false);
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Xiaozhi queued speaker write failed: %d", result);
            s_failed.store(true);
            s_running.store(false);
        }
    }
    s_busy.store(false);
    s_exited.store(true);
    vTaskSuspend(nullptr);
}
} // namespace

void xiaozhi_tts_playback_stop()
{
    s_running.store(false);
    signal_playback_task();
    for (int retry = 0;
         s_task && !s_exited.load() && retry < kStopRetryCount;
         ++retry) {
        vTaskDelay(pdMS_TO_TICKS(kStopRetryDelayMs));
    }
    if (s_task && !s_exited.load()) {
        ESP_LOGW(TAG,
                 XIAOZHI_TTS_TASK_STOP_TIMEOUT_FORMAT,
                 kStopRetryCount,
                 static_cast<unsigned>(kStopRetryDelayMs));
    }
    if (s_task) {
        vTaskDelete(s_task);
        s_task = nullptr;
    }
    s_exited.store(true);
    s_busy.store(false);
    release_storage();
}

bool xiaozhi_tts_playback_start()
{
    if (s_running.load() && s_task) {
        return true;
    }
    xiaozhi_tts_playback_stop();
    if (!prepare_wake_signal()) {
        return false;
    }
    s_storage = static_cast<uint8_t *>(heap_caps_calloc(
        1, kStreamBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_stream_buffer = static_cast<StaticStreamBuffer_t *>(heap_caps_calloc(
        1, sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_task_stack = static_cast<StackType_t *>(heap_caps_calloc(
        1, kTaskStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_task_buffer = static_cast<StaticTask_t *>(heap_caps_calloc(
        1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_storage || !s_stream_buffer || !s_task_stack || !s_task_buffer) {
        ESP_LOGW(TAG, "Xiaozhi TTS playback storage allocation failed");
        release_storage();
        return false;
    }
    s_stream = xStreamBufferCreateStatic(kStreamBytes,
                                         sizeof(int16_t),
                                         s_storage,
                                         s_stream_buffer);
    if (!s_stream) {
        ESP_LOGW(TAG, "%s", kStreamCreateFailedLog);
        release_storage();
        return false;
    }
    s_failed.store(false);
    s_busy.store(false);
    s_exited.store(false);
    s_running.store(true);
    s_task = xTaskCreateStaticPinnedToCore(playback_task,
                                           "xiaozhi_tts",
                                           kTaskStackBytes,
                                           nullptr,
                                           kTaskPriority,
                                           s_task_stack,
                                           s_task_buffer,
                                           1);
    if (!s_task) {
        ESP_LOGW(TAG, "%s", kTaskCreateFailedLog);
        s_running.store(false);
        s_exited.store(true);
        release_storage();
        return false;
    }
    ESP_LOGI(TAG,
             "Xiaozhi TTS queue ready: psram=%u prebuffer=%u ms",
             static_cast<unsigned>(kStreamBytes + kTaskStackBytes),
             static_cast<unsigned>(kPrebufferWaitMs));
    return true;
}

bool xiaozhi_tts_playback_enqueue(const int16_t *samples, size_t sample_count)
{
    if (!samples || sample_count == 0 || !s_running.load() ||
        !s_stream || s_failed.load()) {
        return false;
    }
    size_t bytes = 0;
    if (!app_memory::checked_size_multiply(sample_count, sizeof(int16_t), &bytes)) {
        ESP_LOGW(TAG, "%s", kSampleCountOverflowLog);
        return false;
    }
    size_t sent = xStreamBufferSend(s_stream,
                                    samples,
                                    bytes,
                                    pdMS_TO_TICKS(250));
    if (sent > 0) {
        signal_playback_task();
    }
    if (sent != bytes) {
        ESP_LOGW(TAG,
                 "Xiaozhi TTS queue full: sent=%u expected=%u free=%u",
                 static_cast<unsigned>(sent),
                 static_cast<unsigned>(bytes),
                 static_cast<unsigned>(xStreamBufferSpacesAvailable(s_stream)));
        return false;
    }
    return true;
}

bool xiaozhi_tts_playback_drained()
{
    return s_stream &&
           xStreamBufferBytesAvailable(s_stream) == 0 &&
           !s_busy.load();
}

bool xiaozhi_tts_playback_failed()
{
    return s_failed.load();
}

void xiaozhi_tts_playback_get_snapshot(XiaozhiTtsPlaybackSnapshot *out)
{
    if (!out) {
        return;
    }
    out->task_created = s_task != nullptr;
    out->running = s_running.load();
    out->busy = s_busy.load();
    out->queued_bytes = s_stream ? xStreamBufferBytesAvailable(s_stream) : 0;
}
