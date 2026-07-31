// 管理小智 TTS PCM 队列、播放任务及其一次会话资源。
#pragma once

#include <stddef.h>
#include <stdint.h>

struct XiaozhiTtsPlaybackSnapshot {
    bool task_created = false;
    bool running = false;
    bool busy = false;
    size_t queued_bytes = 0;
};

bool xiaozhi_tts_playback_start();
void xiaozhi_tts_playback_stop();
bool xiaozhi_tts_playback_enqueue(const int16_t *samples, size_t sample_count);
bool xiaozhi_tts_playback_drained();
bool xiaozhi_tts_playback_failed();
void xiaozhi_tts_playback_get_snapshot(XiaozhiTtsPlaybackSnapshot *out);
