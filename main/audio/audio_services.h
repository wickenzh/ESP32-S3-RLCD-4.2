// 声明音频播放、提示音选择和音频电源管理接口。
#pragma once
#include "app_state.h"

void hourly_chime_task(void *arg);
void setup_prompt_task(void *);
bool start_chime_playback(int source_slot);
bool start_setup_prompt_playback();
bool is_audio_playing();
void request_setup_prompt_once();
void request_settings_confirmation_chime();
void play_hourly_chime(int hour, bool enforce_quiet_hours = true);
