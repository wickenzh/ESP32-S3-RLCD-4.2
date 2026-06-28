// 声明 LVGL BSP 初始化、锁和解锁接口。
#pragma once

#include "lvgl.h"

#define LVGL_TICK_PERIOD_MS    1000
#define LVGL_TASK_MAX_DELAY_MS 1000
#define LVGL_TASK_MIN_DELAY_MS 250

typedef void (*DispFlushCb)(struct _lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);

void Lvgl_PortInit(int width, int height,DispFlushCb flush_cb);
bool Lvgl_lock(int timeout_ms);
void Lvgl_unlock(void);
