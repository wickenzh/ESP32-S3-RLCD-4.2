// 声明音频 codec、I2S 和 PCM 播放相关板级接口。
#pragma once

#include "codec_board.h"
#include "codec_init.h"
#include "i2c_bsp.h"

class CodecPort
{
private:
    esp_codec_dev_handle_t playback = NULL;
    esp_codec_dev_handle_t record = NULL;
    bool initialized = false;
    bool speaker_open = false;
    bool mic_open = false;
    int speaker_sample_rate = 0;

public:
    CodecPort(I2cMasterBus& i2cbus,const char *strName);
    ~CodecPort();

    void CodecPort_SetSpeakerVol(int vol);
    void CodecPort_SetMicGain(float db_value);

    bool CodecPort_CloseSpeaker(void);
    bool CodecPort_CloseMic(void);

    int CodecPort_PlayWrite(void *ptr,int ptr_len);
    int CodecPort_EchoRead(void *ptr,int ptr_len);
    bool CodecPort_OpenXiaozhiMic(void);
    bool CodecPort_OpenXiaozhiSpeaker(int sample_rate);
    bool CodecPort_IsReady(void) const;
    bool CodecPort_IsMicrophoneReady(void) const;
    bool CodecPort_PlayChimeSound(int sound_index,
                                  int volume_percent,
                                  bool (*stop_requested)() = nullptr);
    bool CodecPort_PlayWifiPrompt(void);

    bool CodecPort_SetInfo(const char *strName,int open_en,int sample_rate,int channel,int bits_per_sample);

};
