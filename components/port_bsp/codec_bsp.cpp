// 封装 ES8311/ES7210 音频 codec 和 I2S 播放录音接口。
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include "codec_bsp.h"
#include "i2c_bsp.h"

static const char *TAG = "CodecPort";

extern const uint8_t hourly_chime_pcm_start[] asm("_binary_hourly_chime_pcm_start");
extern const uint8_t hourly_chime_pcm_end[] asm("_binary_hourly_chime_pcm_end");
extern const uint8_t chime_1_pcm_start[] asm("_binary_chime_1_pcm_start");
extern const uint8_t chime_1_pcm_end[] asm("_binary_chime_1_pcm_end");
extern const uint8_t chime_2_pcm_start[] asm("_binary_chime_2_pcm_start");
extern const uint8_t chime_2_pcm_end[] asm("_binary_chime_2_pcm_end");
extern const uint8_t chime_3_pcm_start[] asm("_binary_chime_3_pcm_start");
extern const uint8_t chime_3_pcm_end[] asm("_binary_chime_3_pcm_end");
extern const uint8_t chime_4_pcm_start[] asm("_binary_chime_4_pcm_start");
extern const uint8_t chime_4_pcm_end[] asm("_binary_chime_4_pcm_end");
extern const uint8_t wifi_prompt_pcm_start[] asm("_binary_wifi_prompt_pcm_start");
extern const uint8_t wifi_prompt_pcm_end[] asm("_binary_wifi_prompt_pcm_end");

void CodecPort::CodecPort_MusicTask(void *arg) {
	CodecPort *codec = (CodecPort *)arg;
	codec->CodecPort_SetSpeakerVol(60);
	for(;;) {
		size_t bytes_write = 0;
  	  	size_t bytes_sizt = hourly_chime_pcm_end - hourly_chime_pcm_start;
  	  	uint8_t *data_ptr = (uint8_t *)hourly_chime_pcm_start;
		codec->CodecPort_SetInfo("es8311",1,24000,4,16);
		do
		{
			codec->CodecPort_PlayWrite(data_ptr, 256);
  	  	  	data_ptr += 256;
  	  	  	bytes_write += 256;
		} while (bytes_write < bytes_sizt);
	}
}

void CodecPort::CodecPort_EchoTask(void *arg) {
	CodecPort *codec = (CodecPort *)arg;
	codec->CodecPort_SetSpeakerVol(60);
	codec->CodecPort_SetMicGain(25);
	uint8_t *data_ptr = (uint8_t *)heap_caps_malloc(1024 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
	codec->CodecPort_SetInfo("es8311 & es7210",1,44100,2,16);
	for(;;)
  	{
  	  	if(ESP_CODEC_DEV_OK == codec->CodecPort_EchoRead(data_ptr, 1024))
  	  	{
  	  	  	codec->CodecPort_PlayWrite(data_ptr, 1024);
  	  	}
  	}
}


CodecPort::CodecPort(I2cMasterBus& i2cbus,const char *strName) :
i2cbus_(i2cbus) 
{
    set_codec_board_type(strName);
    codec_init_cfg_t codec_cfg = {};
    codec_cfg.in_mode          = CODEC_I2S_MODE_NONE;
    codec_cfg.out_mode         = CODEC_I2S_MODE_TDM;
    codec_cfg.in_use_tdm       = false;
    codec_cfg.reuse_dev        = false;
    esp_err_t err = init_codec(&codec_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec init failed: %s", esp_err_to_name(err));
        return;
    }
    playback = get_playback_handle();
    record   = get_record_handle();
    initialized = playback != NULL;

    i2c_master_bus_handle_t I2cMasterBus = i2cbus_.Get_I2cBusHandle();
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = Es8311Address;
    dev_cfg.scl_speed_hz    = 400000;
    err = i2c_master_bus_add_device(I2cMasterBus, &dev_cfg, &I2c_DevEs8311);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "es8311 i2c add failed: %s", esp_err_to_name(err));
    }

    dev_cfg.device_address  = Es7210Address;
    err = i2c_master_bus_add_device(I2cMasterBus, &dev_cfg, &I2c_DevEs7210);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "es7210 i2c add failed: %s", esp_err_to_name(err));
    }
}

CodecPort::~CodecPort() {
    CodecPort_CloseSpeaker();
    CodecPort_CloseMic();
    deinit_codec();
    if (I2c_DevEs8311) {
        esp_err_t err = i2c_master_bus_rm_device(I2c_DevEs8311);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "es8311 i2c remove failed: %s", esp_err_to_name(err));
        }
        I2c_DevEs8311 = nullptr;
    }
    if (I2c_DevEs7210) {
        esp_err_t err = i2c_master_bus_rm_device(I2c_DevEs7210);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "es7210 i2c remove failed: %s", esp_err_to_name(err));
        }
        I2c_DevEs7210 = nullptr;
    }
    initialized = false;
    playback = nullptr;
    record = nullptr;
}

void CodecPort::Codec_SetCodecReg(const char *str, uint8_t reg, uint8_t data) {
    if (!strcmp(str, "es8311"))
        i2cbus_.i2c_write_buff(I2c_DevEs8311, reg, &data, 1);
    if (!strcmp(str, "es7210"))
        i2cbus_.i2c_write_buff(I2c_DevEs7210, reg, &data, 1);
}

uint8_t CodecPort::Codec_GetCodecReg(const char *str, uint8_t reg) {
    uint8_t data = 0x00;
    if (!strcmp(str, "es8311"))
        i2cbus_.i2c_read_buff(I2c_DevEs8311, reg, &data, 1);
    if (!strcmp(str, "es7210"))
        i2cbus_.i2c_read_buff(I2c_DevEs7210, reg, &data, 1);
    return data;
}

void CodecPort::CodecPort_SetSpeakerVol(int vol) {
    if (!initialized || !playback) return;
	esp_codec_dev_set_out_vol(playback, vol);
}

void CodecPort::CodecPort_SetMicGain(float db_value) {
    if (!initialized || !record) return;
	esp_codec_dev_set_in_gain(record, db_value);
}

void CodecPort::CodecPort_CloseSpeaker(void) {
    if (!initialized || !playback || !speaker_open) return;
	int ret = esp_codec_dev_close(playback);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "speaker close failed: %d", ret);
    }
    speaker_open = false;
}

void CodecPort::CodecPort_CloseMic(void) {
    if (!initialized || !record || !mic_open) return;
	int ret = esp_codec_dev_close(record);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "mic close failed: %d", ret);
    }
    mic_open = false;
}

int CodecPort::CodecPort_PlayWrite(void *ptr,int ptr_len) {
    if (!initialized || !playback) return ESP_FAIL;
	return esp_codec_dev_write(playback, ptr, ptr_len);
}

int CodecPort::CodecPort_EchoRead(void *ptr,int ptr_len) {
    if (!initialized || !record) return ESP_FAIL;
	return esp_codec_dev_read(record, ptr, ptr_len);
}

bool CodecPort::CodecPort_SetInfo(const char *strName,int open_en,int sample_rate,int channel,int bits_per_sample) {
    esp_codec_dev_sample_info_t fs = {};
    	fs.sample_rate = sample_rate;
    	fs.channel = channel;
    	fs.bits_per_sample = bits_per_sample;
        if (channel == 4) {
            fs.channel_mask = 0x0F;
            fs.mclk_multiple = 384;
        }
	if(open_en) {
        if (!initialized) return false;
        int ret = ESP_CODEC_DEV_OK;
		if(!strcmp(strName,"es8311")) {
			ret = esp_codec_dev_open(playback, &fs);
            speaker_open = ret == ESP_CODEC_DEV_OK;
		} else if(!strcmp(strName,"es7210")) {
			ret = esp_codec_dev_open(record, &fs);
            mic_open = ret == ESP_CODEC_DEV_OK;
		} else {
			ret = esp_codec_dev_open(playback, &fs);
            speaker_open = ret == ESP_CODEC_DEV_OK;
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

static bool play_pcm_to_slot0(CodecPort *codec, const uint8_t *pcm_start, const uint8_t *pcm_end, int source_slot, int volume)
{
    static uint8_t slot_buffer[4096];
    constexpr int kSampleRate = 24000;
    constexpr int kChannels = 4;
    constexpr int kBytesPerSample = sizeof(int16_t);
    constexpr int kFrameBytes = kChannels * kBytesPerSample;
    constexpr int kWarmupMs = 90;
    constexpr int kFadeInMs = 80;
    constexpr int kFadeOutMs = 36;
    constexpr int kTailSilenceMs = 40;
    const size_t warmup_bytes = (kSampleRate * kWarmupMs / 1000) * kFrameBytes;
    const size_t fade_frames = kSampleRate * kFadeInMs / 1000;
    const size_t fade_out_frames = kSampleRate * kFadeOutMs / 1000;
    const size_t tail_bytes = (kSampleRate * kTailSilenceMs / 1000) * kFrameBytes;

    if (!codec || !codec->CodecPort_IsReady()) {
        ESP_LOGW(TAG, "codec is not ready");
        return false;
    }
    if (source_slot < 0 || source_slot > 3) {
        ESP_LOGW(TAG, "invalid pcm source slot: %d", source_slot);
        return false;
    }
    if (!codec->CodecPort_SetInfo("es8311", 1, 24000, 4, 16)) {
        return false;
    }
    codec->CodecPort_SetSpeakerVol(0);
    memset(slot_buffer, 0, sizeof(slot_buffer));
    size_t warmup_written = 0;
    while (warmup_written < warmup_bytes) {
        size_t chunk = warmup_bytes - warmup_written;
        if (chunk > sizeof(slot_buffer)) {
            chunk = sizeof(slot_buffer);
        }
        if (codec->CodecPort_PlayWrite((void *)slot_buffer, (int)chunk) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "audio warmup write failed");
            codec->CodecPort_CloseSpeaker();
            return false;
        }
        warmup_written += chunk;
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    codec->CodecPort_SetSpeakerVol(volume);
    const size_t bytes_size = pcm_end - pcm_start;
    const size_t total_frames = bytes_size / kFrameBytes;
    const uint8_t *data_ptr = pcm_start;
    size_t bytes_written = 0;
    while (bytes_written < bytes_size) {
        size_t chunk = bytes_size - bytes_written;
        if (chunk > sizeof(slot_buffer)) {
            chunk = sizeof(slot_buffer);
        }
        memcpy(slot_buffer, data_ptr, chunk);
        int16_t *samples = reinterpret_cast<int16_t *>(slot_buffer);
        size_t frames = chunk / (4 * sizeof(int16_t));
        for (size_t frame = 0; frame < frames; ++frame) {
            size_t global_frame = (bytes_written / kFrameBytes) + frame;
            int16_t selected_sample = samples[frame * 4 + source_slot];
            if (global_frame < fade_frames) {
                selected_sample = (int16_t)(((int32_t)selected_sample * (int32_t)global_frame) / (int32_t)fade_frames);
            }
            if (fade_out_frames > 0 && global_frame < total_frames) {
                size_t frames_left = total_frames - global_frame;
                if (frames_left < fade_out_frames) {
                    selected_sample = (int16_t)(((int32_t)selected_sample * (int32_t)frames_left) / (int32_t)fade_out_frames);
                }
            }
            for (int slot = 0; slot < 4; ++slot) {
                samples[frame * 4 + slot] = slot == 0 ? selected_sample : 0;
            }
        }
        if (codec->CodecPort_PlayWrite((void *)slot_buffer, (int)chunk) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "pcm write failed");
            codec->CodecPort_CloseSpeaker();
            return false;
        }
        data_ptr += chunk;
        bytes_written += chunk;
    }
    size_t tail_written = 0;
    memset(slot_buffer, 0, sizeof(slot_buffer));
    while (tail_written < tail_bytes) {
        size_t chunk = tail_bytes - tail_written;
        if (chunk > sizeof(slot_buffer)) {
            chunk = sizeof(slot_buffer);
        }
        if (codec->CodecPort_PlayWrite((void *)slot_buffer, (int)chunk) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "audio tail silence write failed");
            codec->CodecPort_CloseSpeaker();
            return false;
        }
        tail_written += chunk;
    }
    codec->CodecPort_CloseSpeaker();
    return true;
}

bool CodecPort::CodecPort_PlayHourlyChime(void) {
    return CodecPort_PlayHourlyChimeSlot(3);
}

bool CodecPort::CodecPort_PlayHourlyChimeSlot(int source_slot) {
    return play_pcm_to_slot0(this, hourly_chime_pcm_start, hourly_chime_pcm_end, source_slot, 90);
}

bool CodecPort::CodecPort_PlayChimeSound(int sound_index, int volume_percent) {
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
    case 4:
        start = chime_4_pcm_start;
        end = chime_4_pcm_end;
        source_slot = 0;
        break;
    default:
        break;
    }
    return play_pcm_to_slot0(this, start, end, source_slot, volume_percent);
}

bool CodecPort::CodecPort_PlayWifiPrompt(void) {
    return play_pcm_to_slot0(this, wifi_prompt_pcm_start, wifi_prompt_pcm_end, 0, 90);
}

void CodecPort::CodecPort_CreateMusicTask(void) {
	xTaskCreate(CodecPort_MusicTask, "CodecPort_MusicTask", 4 * 1024, (void *)this, 2, NULL);
}

void CodecPort::CodecPort_CreateEchoTask(void) {
	xTaskCreate(CodecPort_EchoTask, "CodecPort_EchoTask", 4 * 1024, (void *)this, 2, NULL);
}

uint8_t *CodecPort::CodecPort_GetPcmData(uint32_t *len) {
	*len = (hourly_chime_pcm_end - hourly_chime_pcm_start);
	return (uint8_t *)hourly_chime_pcm_start;
}
