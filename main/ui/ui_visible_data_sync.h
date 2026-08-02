// 声明工作页可见时的天气与每日文字补拉及天气状态刷新接口。
#pragma once

#include "app_state.h"
#include "ui_visible_sync_retry.h"

struct ActiveWorkPageState {
    bool history = false;
    bool gallery = false;
    bool calendar = false;
    bool weather_board = false;
    bool radio = false;
    bool xiaozhi = false;
    bool weather_clock = false;
    bool uses_weather_data = false;
};

bool normal_work_page_active(int page);
ActiveWorkPageState active_work_page_state(int active_page);
void update_visible_weather_sync(const ActiveWorkPageState &state,
                                 time_t now,
                                 TickType_t tick_now,
                                 VisibleSyncRetryState<TickType_t> &retry);
void update_visible_daily_saying_sync(const ActiveWorkPageState &state,
                                      const struct tm &local,
                                      time_t now,
                                      TickType_t tick_now,
                                      VisibleSyncRetryState<TickType_t> &retry);
bool update_weather_clock_network_status(EventBits_t bits,
                                         time_t now,
                                         TickType_t tick_now,
                                         VisibleSyncRetryState<TickType_t> &retry);
