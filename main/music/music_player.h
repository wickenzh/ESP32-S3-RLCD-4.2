// SD卡本地音乐播放服务：递归扫描MP3/WAV、列表存SD卡、Helix MP3/WAV解码播放。
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 音乐播放器状态
typedef enum {
    kMusicIdle = 0,
    kMusicScanning,     // 正在扫描SD卡
    kMusicLoading,      // 正在加载歌曲
    kMusicPlaying,      // 正在播放
    kMusicPaused,       // 暂停
    kMusicError,        // 出错
} MusicState;

// 音乐播放器快照（UI读取用）
typedef struct {
    MusicState state;
    char current_file[64];   // 当前播放文件名
    char status_text[48];    // 状态文本
    int song_index;          // 当前歌曲索引
    int song_count;          // 歌曲总数
    char current_dir[64];    // 当前目录
    char upcoming_files[3][64]; // 下3首文件名
    bool shuffle_mode;       // 是否随机播放
} MusicSnapshot;

// 初始化音乐播放服务
void music_init();

// 页面激活/停用
void music_set_page_active(bool active);
bool music_page_active();

// 获取快照
void music_get_snapshot(MusicSnapshot *out);

// KEY键操作：短按下一首
void music_next_song();

// BOOT键操作：上一首
void music_prev_song();

// 播放/暂停切换
void music_toggle_pause();

// 音乐播放时持有音频资源
bool music_is_playing();

// 随机播放切换
void music_toggle_shuffle();
bool music_shuffle_enabled();

#ifdef __cplusplus
}
#endif
