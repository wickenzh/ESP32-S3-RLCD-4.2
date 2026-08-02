// 基于 ESP-SR WakeNet 的轻量唤醒词监听；不创建独立 I2S 或电源策略。
#pragma once

#include <stddef.h>
#include <stdint.h>

struct XiaozhiVoiceRuntimeSnapshot {
    bool running = false;
    bool streaming = false;
    bool feed_task = false;
    bool detect_task = false;
    bool afe = false;
    bool model = false;
    bool processed_stream = false;
    bool capture_buffer = false;
};

using XiaozhiVoiceEventCallback = void (*)();

void xiaozhi_voice_set_event_callback(XiaozhiVoiceEventCallback callback);
bool xiaozhi_voice_start();
void xiaozhi_voice_stop();
bool xiaozhi_voice_take_wake_word();
void xiaozhi_voice_trigger_wake();
bool xiaozhi_voice_is_listening();
void xiaozhi_voice_get_runtime_snapshot(XiaozhiVoiceRuntimeSnapshot *out);
void xiaozhi_voice_set_streaming(bool enabled);
void xiaozhi_voice_pause_streaming();
bool xiaozhi_voice_read_processed(int16_t *mono_samples,
                                  size_t sample_count,
                                  uint32_t timeout_ms);
size_t xiaozhi_voice_processed_bytes_available();
