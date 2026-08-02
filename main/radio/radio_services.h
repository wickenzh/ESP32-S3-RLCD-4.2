// 声明网络电台服务的连接、解码、播放和页面生命周期接口。
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 电台状态
typedef enum {
    kRadioIdle = 0,       // 空闲
    kRadioConnecting,     // 正在连接
    kRadioPlaying,        // 正在播放
    kRadioError,          // 出错
} RadioState;

// 电台快照（UI读取用）
typedef struct {
    RadioState state;
    char station_name[32];   // 当前电台名
    char status_text[64];    // 状态文本
    uint32_t uptime_sec;     // 播放时长（秒）
} RadioSnapshot;

// 初始化电台服务
void radio_init();

// 页面激活/停用
void radio_set_page_active(bool active);
bool radio_page_active();

// 获取快照
void radio_get_snapshot(RadioSnapshot *out);

// 切换到下一个电台
void radio_next_station();

// 切换到上一个电台
void radio_prev_station();

// 获取当前电台索引
int radio_current_station_index();

// 获取电台总数
int radio_station_count();

// 电台播放时需要WiFi保持连接（阻止stop_wifi_radio）
bool radio_network_keepalive_active();

#ifdef __cplusplus
}
#endif
