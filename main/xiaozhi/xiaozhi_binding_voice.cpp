// 播放小智首次绑定提示和逐位数字语音，并管理独立播放任务。
#include "xiaozhi_binding_voice.h"

#include "app_state.h"
#include "audio_services.h"
#include "scoped_heap_buffer.h"
#include "single_pending_task_gate.h"

#include <esp_codec_dev_types.h>
#include <esp_log.h>

#include <cstdint>
#include <string.h>

namespace {
constexpr int kBindingPcmSampleRate = 16000;
constexpr size_t kBindingPauseSamples = 1280;
constexpr uint32_t kBindingVoiceTaskStackBytes = 6144;
constexpr UBaseType_t kBindingVoiceTaskPriority = 3;
constexpr size_t kBindingCodeStorageSize = 24;
#define XIAOZHI_BINDING_COPY_ALLOC_FAILED_LOG "xiaozhi binding code copy allocation failed"
#define XIAOZHI_BINDING_TASK_CREATE_FAILED_LOG "xiaozhi binding voice task creation failed"

static_assert(kBindingVoiceTaskStackBytes > 0,
              "Xiaozhi binding task stack must be positive");
static_assert(kBindingCodeStorageSize > 1,
              "Xiaozhi binding code storage must fit text and NUL");

extern const uint8_t prompt_pcm_start[] asm("_binary_prompt_pcm_start");
extern const uint8_t prompt_pcm_end[] asm("_binary_prompt_pcm_end");
extern const uint8_t digit_0_pcm_start[] asm("_binary_digit_0_pcm_start");
extern const uint8_t digit_0_pcm_end[] asm("_binary_digit_0_pcm_end");
extern const uint8_t digit_1_pcm_start[] asm("_binary_digit_1_pcm_start");
extern const uint8_t digit_1_pcm_end[] asm("_binary_digit_1_pcm_end");
extern const uint8_t digit_2_pcm_start[] asm("_binary_digit_2_pcm_start");
extern const uint8_t digit_2_pcm_end[] asm("_binary_digit_2_pcm_end");
extern const uint8_t digit_3_pcm_start[] asm("_binary_digit_3_pcm_start");
extern const uint8_t digit_3_pcm_end[] asm("_binary_digit_3_pcm_end");
extern const uint8_t digit_4_pcm_start[] asm("_binary_digit_4_pcm_start");
extern const uint8_t digit_4_pcm_end[] asm("_binary_digit_4_pcm_end");
extern const uint8_t digit_5_pcm_start[] asm("_binary_digit_5_pcm_start");
extern const uint8_t digit_5_pcm_end[] asm("_binary_digit_5_pcm_end");
extern const uint8_t digit_6_pcm_start[] asm("_binary_digit_6_pcm_start");
extern const uint8_t digit_6_pcm_end[] asm("_binary_digit_6_pcm_end");
extern const uint8_t digit_7_pcm_start[] asm("_binary_digit_7_pcm_start");
extern const uint8_t digit_7_pcm_end[] asm("_binary_digit_7_pcm_end");
extern const uint8_t digit_8_pcm_start[] asm("_binary_digit_8_pcm_start");
extern const uint8_t digit_8_pcm_end[] asm("_binary_digit_8_pcm_end");
extern const uint8_t digit_9_pcm_start[] asm("_binary_digit_9_pcm_start");
extern const uint8_t digit_9_pcm_end[] asm("_binary_digit_9_pcm_end");

struct EmbeddedPcm {
    const uint8_t *start;
    const uint8_t *end;
};

constexpr EmbeddedPcm kBindingPromptPcm = {prompt_pcm_start, prompt_pcm_end};
constexpr EmbeddedPcm kBindingDigitPcm[] = {
    {digit_0_pcm_start, digit_0_pcm_end},
    {digit_1_pcm_start, digit_1_pcm_end},
    {digit_2_pcm_start, digit_2_pcm_end},
    {digit_3_pcm_start, digit_3_pcm_end},
    {digit_4_pcm_start, digit_4_pcm_end},
    {digit_5_pcm_start, digit_5_pcm_end},
    {digit_6_pcm_start, digit_6_pcm_end},
    {digit_7_pcm_start, digit_7_pcm_end},
    {digit_8_pcm_start, digit_8_pcm_end},
    {digit_9_pcm_start, digit_9_pcm_end},
};
static_assert(sizeof(kBindingDigitPcm) / sizeof(kBindingDigitPcm[0]) == 10,
              "Xiaozhi binding digit audio must cover 0 through 9");

char s_last_announced_binding_code[kBindingCodeStorageSize] = {};
portMUX_TYPE s_binding_code_mux = portMUX_INITIALIZER_UNLOCKED;
SinglePendingTaskGate s_binding_voice_task_gate;

bool binding_code_needs_announcement(const char *binding_code)
{
    char last_announced[kBindingCodeStorageSize] = {};
    portENTER_CRITICAL(&s_binding_code_mux);
    memcpy(last_announced,
           s_last_announced_binding_code,
           sizeof(last_announced));
    portEXIT_CRITICAL(&s_binding_code_mux);
    return xiaozhi_binding_voice::should_announce(binding_code, last_announced);
}

void record_announced_binding_code(const char *binding_code)
{
    portENTER_CRITICAL(&s_binding_code_mux);
    strlcpy(s_last_announced_binding_code,
            binding_code ? binding_code : "",
            sizeof(s_last_announced_binding_code));
    portEXIT_CRITICAL(&s_binding_code_mux);
}

bool play_embedded_pcm(const EmbeddedPcm &pcm)
{
    if (!pcm.start || !pcm.end || pcm.end <= pcm.start) {
        return false;
    }
    size_t bytes = static_cast<size_t>(pcm.end - pcm.start);
    if (bytes % sizeof(int16_t) != 0) {
        return false;
    }
    return write_xiaozhi_speaker(reinterpret_cast<const int16_t *>(pcm.start),
                                 bytes / sizeof(int16_t),
                                 kBindingPcmSampleRate) == ESP_CODEC_DEV_OK;
}

bool play_binding_id_voice(char *raw_binding_code)
{
    ScopedHeapBuffer<char> binding_code(raw_binding_code, kBindingCodeStorageSize);
    if (!binding_code) {
        return false;
    }
    if (!start_xiaozhi_audio_session()) {
        return false;
    }
    bool played = play_embedded_pcm(kBindingPromptPcm);
    static const int16_t silence[kBindingPauseSamples] = {};
    if (played) {
        played = write_xiaozhi_speaker(silence,
                                       kBindingPauseSamples,
                                       kBindingPcmSampleRate) == ESP_CODEC_DEV_OK;
    }
    for (const char *cursor = binding_code.data(); played && *cursor; ++cursor) {
        int index = xiaozhi_binding_voice::digit_index(*cursor);
        if (index < 0) {
            continue;
        }
        played = play_embedded_pcm(kBindingDigitPcm[index]);
        if (played) {
            played = write_xiaozhi_speaker(silence,
                                           kBindingPauseSamples,
                                           kBindingPcmSampleRate) == ESP_CODEC_DEV_OK;
        }
    }
    ESP_LOGI(TAG, "xiaozhi binding code playback %s", played ? "complete" : "failed");
    stop_xiaozhi_audio_session();
    return played;
}

void binding_id_voice_task(void *arg)
{
    char *binding_code = static_cast<char *>(arg);
    char announced_code[kBindingCodeStorageSize] = {};
    strlcpy(announced_code, binding_code ? binding_code : "", sizeof(announced_code));
    bool played = play_binding_id_voice(binding_code);
    if (played) {
        record_announced_binding_code(announced_code);
    }
    s_binding_voice_task_gate.release();
    vTaskDelete(nullptr);
}
} // namespace

void xiaozhi_announce_binding_id_once(const char *binding_code)
{
    if (!binding_code_needs_announcement(binding_code) ||
        !s_binding_voice_task_gate.try_acquire()) {
        return;
    }
    ScopedHeapBuffer<char> code_copy(kBindingCodeStorageSize, HeapBufferInit::kZeroed);
    if (!code_copy) {
        ESP_LOGW(TAG, XIAOZHI_BINDING_COPY_ALLOC_FAILED_LOG);
        s_binding_voice_task_gate.release();
        return;
    }
    strlcpy(code_copy.data(), binding_code, code_copy.size());
    if (xTaskCreate(binding_id_voice_task,
                    "xiaozhi_bind",
                    kBindingVoiceTaskStackBytes,
                    code_copy.data(),
                    kBindingVoiceTaskPriority,
                    nullptr) != pdPASS) {
        ESP_LOGW(TAG, XIAOZHI_BINDING_TASK_CREATE_FAILED_LOG);
        s_binding_voice_task_gate.release();
        return;
    }
    (void)code_copy.release();
}
