// 声明启动页动画帧数据和帧尺寸信息。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_ANIM_WIDTH 112
#define BOOT_ANIM_HEIGHT 112
#define BOOT_ANIM_FRAME_COUNT 96
#define BOOT_ANIM_BYTES_PER_FRAME 1568

extern const uint8_t boot_anim_frames[BOOT_ANIM_FRAME_COUNT][BOOT_ANIM_BYTES_PER_FRAME];

#ifdef __cplusplus
}
#endif
