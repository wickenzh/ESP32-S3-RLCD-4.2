// 封装 ES8311/ES7210 音频 codec 和 I2S 播放录音接口。
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include "codec_bsp.h"
#include "i2c_bsp.h"

static const char *TAG = "CodecPort";
static constexpr const char *kCodecNameEs8311 = "es8311";
static constexpr const char *kCodecNameEs7210 = "es7210";
static constexpr int kCodecTdmChannelCount = 4;
static constexpr uint32_t kCodecTdmChannelMask = 0x0F;
static constexpr uint32_t kCodecXiaozhiInputMask = 0x03;
static constexpr int kCodecTdmMclkMultiple = 256;
static constexpr size_t kCodecPcmPlaybackSlotBufferSize = 4096;
static constexpr int kCodecPcmPlaybackSampleRateHz = 24000;
static constexpr int kCodecPcmPlaybackChannelCount = kCodecTdmChannelCount;
static constexpr int kCodecPcmPlaybackBitsPerSample = 16;
static constexpr int kCodecXiaozhiSampleRateHz = 16000;
static constexpr int kCodecXiaozhiInputSlotCount = 4;
static constexpr int kCodecXiaozhiSpeakerVolume = 80;
static_assert(kCodecTdmChannelCount > 0, "Codec TDM channel count must be positive");
static_assert(kCodecTdmChannelMask != 0, "Codec TDM channel mask must not be empty");
static_assert(kCodecXiaozhiInputMask != 0, "Xiaozhi input mask must not be empty");
static_assert(kCodecTdmMclkMultiple > 0, "Codec TDM MCLK multiple must be positive");
static_assert(kCodecPcmPlaybackSlotBufferSize > 0, "Codec PCM playback buffer size must be positive");
static_assert(kCodecPcmPlaybackSampleRateHz > 0, "Codec PCM playback sample rate must be positive");
static_assert(kCodecPcmPlaybackChannelCount > 0, "Codec PCM playback channel count must be positive");
static_assert(kCodecPcmPlaybackBitsPerSample > 0, "Codec PCM playback bits per sample must be positive");
static_assert(kCodecXiaozhiSampleRateHz > 0, "Xiaozhi sample rate must be positive");
static_assert(kCodecXiaozhiSpeakerVolume >= 0 && kCodecXiaozhiSpeakerVolume <= 100,
              "Xiaozhi speaker volume must be in range");

extern const uint8_t hourly_chime_pcm_start[] asm("_binary_hourly_chime_pcm_start");
extern const uint8_t hourly_chime_pcm_end[] asm("_binary_hourly_chime_pcm_end");
extern const uint8_t chime_1_pcm_start[] asm("_binary_chime_1_pcm_start");
extern const uint8_t chime_1_pcm_end[] asm("_binary_chime_1_pcm_end");
extern const uint8_t chime_2_pcm_start[] asm("_binary_chime_2_pcm_start");
extern const uint8_t chime_2_pcm_end[] asm("_binary_chime_2_pcm_end");
extern const uint8_t chime_3_pcm_start[] asm("_binary_chime_3_pcm_start");
extern const uint8_t chime_3_pcm_end[] asm("_binary_chime_3_pcm_end");
extern const uint8_t wifi_prompt_pcm_start[] asm("_binary_wifi_prompt_pcm_start");
extern const uint8_t wifi_prompt_pcm_end[] asm("_binary_wifi_prompt_pcm_end");

CodecPort::CodecPort(I2cMasterBus& i2cbus,const char *strName)
{
    if (!i2cbus.IsReady()) {
        ESP_LOGW(TAG, "codec init failed: %s", esp_err_to_name(ESP_ERR_INVALID_STATE));
        return;
    }
    set_codec_board_type(strName);
    codec_init_cfg_t codec_cfg = {};
    // 与官方同板卡 BoxAudioCodec 保持一致：ES8311 走标准 TX，ES7210
    // 走四时隙 TDM RX，从 slot0/1 取得麦克风与播放参考用于设备端 AEC。
    codec_cfg.in_mode          = CODEC_I2S_MODE_TDM;
    codec_cfg.out_mode         = CODEC_I2S_MODE_STD;
    codec_cfg.in_use_tdm       = true;
    codec_cfg.reuse_dev        = false;
    esp_err_t err = init_codec(&codec_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec init failed: %s", esp_err_to_name(err));
        return;
    }
    playback = get_playback_handle();
    record   = get_record_handle();
    initialized = playback != NULL;
}

CodecPort::~CodecPort() {
    CodecPort_CloseSpeaker();
    CodecPort_CloseMic();
    deinit_codec();
    initialized = false;
    playback = nullptr;
    record = nullptr;
}

void CodecPort::CodecPort_SetSpeakerVol(int vol) {
    if (!initialized || !playback) return;
	esp_codec_dev_set_out_vol(playback, vol);
}

void CodecPort::CodecPort_SetMicGain(float db_value) {
    if (!initialized || !record) return;
	esp_codec_dev_set_in_gain(record, db_value);
}

bool CodecPort::CodecPort_CloseSpeaker(void) {
    if (!initialized || !playback || !speaker_open) return true;
	int ret = esp_codec_dev_close(playback);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "speaker close failed: %d", ret);
        return false;
    }
    speaker_open = false;
    speaker_sample_rate = 0;
    return true;
}

bool CodecPort::CodecPort_CloseMic(void) {
    if (!initialized || !record || !mic_open) return true;
	int ret = esp_codec_dev_close(record);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "mic close failed: %d", ret);
        return false;
    }
    mic_open = false;
    return true;
}

int CodecPort::CodecPort_PlayWrite(void *ptr,int ptr_len) {
    if (!initialized || !playback) return ESP_FAIL;
	return esp_codec_dev_write(playback, ptr, ptr_len);
}

int CodecPort::CodecPort_EchoRead(void *ptr,int ptr_len) {
    if (!initialized || !record) return ESP_FAIL;
	return esp_codec_dev_read(record, ptr, ptr_len);
}

bool CodecPort::CodecPort_OpenXiaozhiMic(void) {
    if (!CodecPort_IsMicrophoneReady()) {
        ESP_LOGW(TAG, "Xiaozhi microphone handle is unavailable");
        return false;
    }
    if (mic_open) {
        return true;
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = kCodecXiaozhiSampleRateHz;
    fs.channel = kCodecXiaozhiInputSlotCount;
    fs.channel_mask = kCodecXiaozhiInputMask;
    fs.bits_per_sample = kCodecPcmPlaybackBitsPerSample;
    fs.mclk_multiple = kCodecTdmMclkMultiple;
    int ret = esp_codec_dev_open(record, &fs);
    mic_open = ret == ESP_CODEC_DEV_OK;
    if (!mic_open) {
        ESP_LOGW(TAG, "Xiaozhi duplex microphone open failed: %d", ret);
    }
    return mic_open;
}

bool CodecPort::CodecPort_OpenXiaozhiSpeaker(int sample_rate) {
    if (sample_rate <= 0) {
        return false;
    }
    if (speaker_open && speaker_sample_rate == sample_rate) {
        return true;
    }
    if (speaker_open && !CodecPort_CloseSpeaker()) {
        return false;
    }
    bool opened = CodecPort_SetInfo(kCodecNameEs8311,
                                    1,
                                    sample_rate,
                                    1,
                                    kCodecPcmPlaybackBitsPerSample);
    if (opened) {
        speaker_sample_rate = sample_rate;
        CodecPort_SetSpeakerVol(kCodecXiaozhiSpeakerVolume);
        ESP_LOGI(TAG, "Xiaozhi speaker opened: %d Hz volume=%d%%",
                 sample_rate,
                 kCodecXiaozhiSpeakerVolume);
    }
    return opened;
}

bool CodecPort::CodecPort_SetInfo(const char *strName,
                                  int open_en,
                                  int sample_rate,
                                  int channel,
                                  int bits_per_sample)
{
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = sample_rate;
    fs.channel = channel;
    fs.bits_per_sample = bits_per_sample;
    if (channel == kCodecTdmChannelCount) {
        fs.channel_mask = kCodecTdmChannelMask;
        fs.mclk_multiple = kCodecTdmMclkMultiple;
    }
    if (open_en) {
        if (!initialized) {
            return false;
        }
        int ret = ESP_CODEC_DEV_OK;
        if (!strcmp(strName, kCodecNameEs8311)) {
            if (!playback) {
                return false;
            }
            ret = esp_codec_dev_open(playback, &fs);
            speaker_open = ret == ESP_CODEC_DEV_OK;
            speaker_sample_rate = speaker_open ? sample_rate : 0;
        } else if (!strcmp(strName, kCodecNameEs7210)) {
            if (!record) {
                return false;
            }
            ret = esp_codec_dev_open(record, &fs);
            mic_open = ret == ESP_CODEC_DEV_OK;
        } else {
            if (!playback || !record) {
                return false;
            }
            ret = esp_codec_dev_open(playback, &fs);
            speaker_open = ret == ESP_CODEC_DEV_OK;
            speaker_sample_rate = speaker_open ? sample_rate : 0;
            if (ret == ESP_CODEC_DEV_OK) {
                ret = esp_codec_dev_open(record, &fs);
                mic_open = ret == ESP_CODEC_DEV_OK;
            }
        }
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "codec open failed: %d", ret);
            return false;
        }
    }
    return true;
}

bool CodecPort::CodecPort_IsReady(void) const {
    return initialized && playback != NULL;
}

bool CodecPort::CodecPort_IsMicrophoneReady(void) const {
    return initialized && record != NULL;
}

static bool play_pcm_to_slot0(CodecPort *codec,
                              const uint8_t *pcm_start,
                              const uint8_t *pcm_end,
                              int source_slot,
                              int volume,
                              bool (*stop_requested)() = nullptr)
{
    static int16_t mono_buffer[kCodecPcmPlaybackSlotBufferSize / sizeof(int16_t)];
    constexpr int kSampleRate = kCodecPcmPlaybackSampleRateHz;
    constexpr int kSourceChannels = kCodecPcmPlaybackChannelCount;
    constexpr int kBytesPerSample = sizeof(int16_t);
    constexpr int kSourceFrameBytes = kSourceChannels * kBytesPerSample;
    constexpr int kWarmupMs = 90;
    constexpr int kFadeInMs = 80;
    constexpr int kFadeOutMs = 36;
    constexpr int kTailSilenceMs = 40;
    const size_t warmup_frames = kSampleRate * kWarmupMs / 1000;
    const size_t fade_frames = kSampleRate * kFadeInMs / 1000;
    const size_t fade_out_frames = kSampleRate * kFadeOutMs / 1000;
    const size_t tail_frames = kSampleRate * kTailSilenceMs / 1000;

    if (!codec || !codec->CodecPort_IsReady()) {
        ESP_LOGW(TAG, "codec is not ready");
        return false;
    }
    if (source_slot < 0 || source_slot >= kSourceChannels) {
        ESP_LOGW(TAG, "invalid pcm source slot: %d", source_slot);
        return false;
    }
    if (!pcm_start || !pcm_end || pcm_end <= pcm_start) {
        ESP_LOGW(TAG, "invalid pcm range");
        return false;
    }
    if (!codec->CodecPort_SetInfo(kCodecNameEs8311, 1, kCodecPcmPlaybackSampleRateHz, 1, kCodecPcmPlaybackBitsPerSample)) {
        return false;
    }
    codec->CodecPort_SetSpeakerVol(0);
    memset(mono_buffer, 0, sizeof(mono_buffer));
    size_t warmup_written = 0;
    while (warmup_written < warmup_frames) {
        if (stop_requested && stop_requested()) {
            codec->CodecPort_CloseSpeaker();
            return true;
        }
        size_t frames = warmup_frames - warmup_written;
        if (frames > sizeof(mono_buffer) / sizeof(mono_buffer[0])) {
            frames = sizeof(mono_buffer) / sizeof(mono_buffer[0]);
        }
        if (codec->CodecPort_PlayWrite(mono_buffer, (int)(frames * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "audio warmup write failed");
            codec->CodecPort_CloseSpeaker();
            return false;
        }
        warmup_written += frames;
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    codec->CodecPort_SetSpeakerVol(volume);
    const size_t bytes_size = pcm_end - pcm_start;
    const size_t total_frames = bytes_size / kSourceFrameBytes;
    const int16_t *source = reinterpret_cast<const int16_t *>(pcm_start);
    size_t frames_written = 0;
    while (frames_written < total_frames) {
        if (stop_requested && stop_requested()) {
            codec->CodecPort_SetSpeakerVol(0);
            codec->CodecPort_CloseSpeaker();
            return true;
        }
        size_t frames = total_frames - frames_written;
        if (frames > sizeof(mono_buffer) / sizeof(mono_buffer[0])) {
            frames = sizeof(mono_buffer) / sizeof(mono_buffer[0]);
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            size_t global_frame = frames_written + frame;
            int16_t selected_sample = source[global_frame * kSourceChannels + source_slot];
            if (global_frame < fade_frames) {
                selected_sample = (int16_t)(((int32_t)selected_sample * (int32_t)global_frame) / (int32_t)fade_frames);
            }
            if (fade_out_frames > 0 && global_frame < total_frames) {
                size_t frames_left = total_frames - global_frame;
                if (frames_left < fade_out_frames) {
                    selected_sample = (int16_t)(((int32_t)selected_sample * (int32_t)frames_left) / (int32_t)fade_out_frames);
                }
            }
            mono_buffer[frame] = selected_sample;
        }
        if (codec->CodecPort_PlayWrite(mono_buffer, (int)(frames * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "pcm write failed");
            codec->CodecPort_CloseSpeaker();
            return false;
        }
        frames_written += frames;
    }
    size_t tail_written = 0;
    memset(mono_buffer, 0, sizeof(mono_buffer));
    while (tail_written < tail_frames) {
        if (stop_requested && stop_requested()) {
            codec->CodecPort_CloseSpeaker();
            return true;
        }
        size_t frames = tail_frames - tail_written;
        if (frames > sizeof(mono_buffer) / sizeof(mono_buffer[0])) {
            frames = sizeof(mono_buffer) / sizeof(mono_buffer[0]);
        }
        if (codec->CodecPort_PlayWrite(mono_buffer, (int)(frames * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "audio tail silence write failed");
            codec->CodecPort_CloseSpeaker();
            return false;
        }
        tail_written += frames;
    }
    codec->CodecPort_CloseSpeaker();
    return true;
}

bool CodecPort::CodecPort_PlayChimeSound(int sound_index,
                                         int volume_percent,
                                         bool (*stop_requested)()) {
    const uint8_t *start = hourly_chime_pcm_start;
    const uint8_t *end = hourly_chime_pcm_end;
    int source_slot = 3;
    switch (sound_index) {
    case 1:
        start = chime_1_pcm_start;
        end = chime_1_pcm_end;
        source_slot = 0;
        break;
    case 2:
        start = chime_2_pcm_start;
        end = chime_2_pcm_end;
        source_slot = 0;
        break;
    case 3:
        start = chime_3_pcm_start;
        end = chime_3_pcm_end;
        source_slot = 0;
        break;
    default:
        break;
    }
    return play_pcm_to_slot0(this,
                             start,
                             end,
                             source_slot,
                             volume_percent,
                             stop_requested);
}

bool CodecPort::CodecPort_PlayWifiPrompt(void) {
    return play_pcm_to_slot0(this, wifi_prompt_pcm_start, wifi_prompt_pcm_end, 0, 90);
}
