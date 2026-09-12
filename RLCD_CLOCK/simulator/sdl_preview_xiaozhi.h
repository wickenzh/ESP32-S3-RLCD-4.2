// 声明小智及番茄钟 SDL 静态预览的模式和页面主体接口。
#pragma once

#include <ctime>

#include "lvgl.h"

struct XiaozhiPreviewMode {
    int remaining_seconds = -1;
    const char *subtitle = nullptr;
    const char *status = nullptr;
    bool pomodoro_running = false;
    bool pomodoro_final = false;
    bool pomodoro_completed = false;
    bool preparing = false;

    bool pomodoro_visible() const
    {
        return pomodoro_running || pomodoro_final || pomodoro_completed;
    }
};

XiaozhiPreviewMode classify_xiaozhi_preview_mode(const char *preview_mode);
void build_xiaozhi_preview_body(lv_obj_t *screen,
                                const struct tm *local,
                                const XiaozhiPreviewMode &mode);
