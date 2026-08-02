// 管理SD卡本地音乐的递归扫描、列表存储、Helix MP3/WAV解码和I2S播放生命周期。
#include "music_player.h"

#include "app_state.h"
#include "audio_services_internal.h"
#include "display_bsp.h"

#include <atomic>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

#include "mp3dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#define MUSIC_TAG "MusicPlayer"

namespace {

// ====== 常量 ======
// 歌曲列表存储（PSRAM动态分配，避免占用DRAM）
static constexpr int kMaxSongCount = 500;
static constexpr int kMaxPathLen = 255;
static constexpr int kMaxScanDepth = 8;
static char **s_song_paths = nullptr; // PSRAM分配：s_song_paths[i]指向第i首歌路径
static int s_scan_count = 0;
static bool s_scan_done = false;  // 是否已完成过扫描
static bool s_scan_failed = false; // 上次扫描是否失败
// MP3解码输入缓冲区
static constexpr size_t kMp3InputBufSize = 32 * 1024;
// PCM输出缓冲区大小（PSRAM分配，2MB预缓冲）
static constexpr size_t kPcmOutputBufferSize = 2 * 1024 * 1024;
// PCM预缓冲阈值（攒够此字节数后开始写I2S，约1MB≈5秒@44.1kHz立体声16bit）
static constexpr size_t kPcmPrebufferBytes = 1 * 1024 * 1024;
// 播放任务栈大小
static constexpr uint32_t kPlayTaskStackSize = 16384;
// 播放任务TCB和栈（PSRAM分配）
static StaticTask_t s_play_task_tcb = {};
static StackType_t *s_play_task_stack = nullptr;
// 播放采样率默认值
static constexpr int kDefaultSampleRate = 44100;
static constexpr int kPlaybackBitsPerSample = 16;
static constexpr int kPlaybackChannelCount = 2;
static constexpr int kDefaultSpeakerVolume = 80;
// FATFS文件读取块大小
static constexpr size_t kFileReadBufSize = 32 * 1024;
// WAV双缓冲每个缓冲区大小（256KB，两个共512KB，约3秒@44.1kHz立体声16bit）
static constexpr size_t kWavBufSize = 1024 * 1024;
// WAV头最大扫描范围（支持JUNK/LIST等额外块）
static constexpr size_t kWavHeaderMaxCheck = 4096;

// ====== 状态 ======
static std::atomic<MusicState> s_state{kMusicIdle};
static std::atomic<bool> s_page_active{false};
static std::atomic<int> s_song_index{0};
static std::atomic<int> s_song_count{0};
static char s_current_file[128] = {};
static char s_status_text[64] = {};
static char s_current_dir[128] = {};
static TaskHandle_t s_play_task = nullptr;
static volatile bool s_stop_requested = false;
static std::atomic<bool> s_skip_to_next{false};
static std::atomic<bool> s_shuffle_mode{true}; // 默认随机播放

// 音频资源
static bool s_audio_owned = false;
static CodecPort *s_codec = nullptr;
static bool s_speaker_open = false;

// ====== 辅助函数 ======
void update_state(MusicState state, const char *status)
{
    s_state.store(state, std::memory_order_release);
    if (status) {
        strlcpy(s_status_text, status, sizeof(s_status_text));
    }
}

// 从完整路径提取文件名（不含目录）
const char *basename_of(const char *path)
{
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// 从完整路径提取目录部分
void extract_dir(const char *path, char *dir_out, size_t dir_size)
{
    if (!path || !dir_out || dir_size == 0) return;
    strlcpy(dir_out, path, dir_size);
    char *slash = strrchr(dir_out, '/');
    if (slash && slash != dir_out) {
        *slash = '\0';
    } else {
        strlcpy(dir_out, "/", dir_size);
    }
}

// Fisher-Yates洗牌（对s_song_paths[0..count-1]随机排列）
void shuffle_song_list(int count)
{
    if (count <= 1 || !s_song_paths) return;
    // 使用esp_timer_get_ticks作为随机种子
    srand((unsigned)(esp_timer_get_time() & 0xFFFFFFFF));
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        char *tmp = s_song_paths[i];
        s_song_paths[i] = s_song_paths[j];
        s_song_paths[j] = tmp;
    }
    ESP_LOGI(MUSIC_TAG, "Song list shuffled (%d songs)", count);
}

// 判断文件扩展名是否为MP3
bool is_mp3_file(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) return false;
    return (name[len-4] == '.' &&
            (name[len-3] == 'm' || name[len-3] == 'M') &&
            (name[len-2] == 'p' || name[len-2] == 'P') &&
            (name[len-1] == '3' || name[len-1] == '3'));
}

// 判断文件扩展名是否为WAV
bool is_wav_file(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) return false;
    return (name[len-4] == '.' &&
            (name[len-3] == 'w' || name[len-3] == 'W') &&
            (name[len-2] == 'a' || name[len-2] == 'A') &&
            (name[len-1] == 'v' || name[len-1] == 'V'));
}

// 判断是否为临时文件（电台缓存等）
bool is_temp_file(const char *name)
{
    if (!name) return true;
    // 过滤 radio_chunk*.mp3
    if (strncmp(name, "radio_chunk", 11) == 0) return true;
    return false;
}

// 判断是否为隐藏文件/目录（以.开头，或macOS ._文件）
bool is_hidden_entry(const char *name)
{
    if (!name || name[0] == '.') return true;
    return false;
}

void scan_directory_recursive(const char *dir_path, int depth)
{
    if (depth > kMaxScanDepth || s_scan_count >= kMaxSongCount) return;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(MUSIC_TAG, "Cannot open dir: %s", dir_path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (is_hidden_entry(entry->d_name)) continue;

        char full_path[512];
        int n;
        if (dir_path[0] == '/' && dir_path[1] == '\0') {
            n = snprintf(full_path, sizeof(full_path), "/%s", entry->d_name);
        } else {
            n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        }
        if (n < 0 || (size_t)n >= sizeof(full_path)) continue; // 路径过长，跳过

        if (entry->d_type == DT_DIR) {
            scan_directory_recursive(full_path, depth + 1);
        } else if (entry->d_type == DT_REG) {
            if ((is_mp3_file(entry->d_name) || is_wav_file(entry->d_name)) && !is_temp_file(entry->d_name)) {
                if (s_scan_count < kMaxSongCount && s_song_paths) {
                    s_song_paths[s_scan_count] = (char *)heap_caps_malloc(kMaxPathLen + 1, MALLOC_CAP_SPIRAM);
                    if (s_song_paths[s_scan_count]) {
                        strlcpy(s_song_paths[s_scan_count], full_path, kMaxPathLen + 1);
                        if (s_scan_count < 5) {
                            ESP_LOGI(MUSIC_TAG, "  [%d] %s", s_scan_count, full_path);
                        }
                        s_scan_count++;
                    }
                }
                if (s_scan_count >= kMaxSongCount) break;
            }
        }
    }
    closedir(dir);
}

// SD卡挂载句柄
static sdmmc_card_t *s_sdcard = nullptr;
static bool s_sdcard_mounted = false;

// 挂载SD卡（使用与CustomSDPort相同的引脚配置）
static bool mount_sd_card()
{
    if (s_sdcard_mounted) return true;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; // 1-bit模式
    slot_config.clk = GPIO_NUM_38;
    slot_config.cmd = GPIO_NUM_21;
    slot_config.d0  = GPIO_NUM_39;
    slot_config.d1  = (gpio_num_t)-1;
    slot_config.d2  = (gpio_num_t)-1;
    slot_config.d3  = (gpio_num_t)-1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &s_sdcard);
    if (ret != ESP_OK) {
        ESP_LOGW(MUSIC_TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    s_sdcard_mounted = true;
    ESP_LOGI(MUSIC_TAG, "SD card mounted OK");
    return true;
}

// 执行扫描（仅首次进入页面时）
bool perform_scan_if_needed()
{
    if (s_scan_done) return !s_scan_failed;

    // 挂载SD卡
    if (!mount_sd_card()) {
        s_scan_done = true;
        s_scan_failed = true;
        update_state(kMusicError, "SD卡挂载失败");
        return false;
    }

    ESP_LOGI(MUSIC_TAG, "Scanning SD card for MP3/WAV files...");
    update_state(kMusicScanning, "扫描SD卡...");

    s_scan_count = 0;
    scan_directory_recursive("/sdcard", 0);
    s_scan_done = true;
    s_scan_failed = (s_scan_count == 0);

    if (s_scan_failed) {
        ESP_LOGW(MUSIC_TAG, "No music files found on SD card");
    } else {
        ESP_LOGI(MUSIC_TAG, "Scan complete: %d songs found", s_scan_count);
        // 随机播放模式下洗牌
        if (s_shuffle_mode.load()) {
            shuffle_song_list(s_scan_count);
        }
    }
    return !s_scan_failed;
}

// 从内存列表获取第N首歌路径
bool get_song_path(int index, char *path_out, size_t path_size)
{
    if (index < 0 || index >= s_scan_count || !s_song_paths || !s_song_paths[index]) return false;
    strlcpy(path_out, s_song_paths[index], path_size);
    return true;
}

// ====== 音频资源管理 ======
void release_audio();  // 前向声明

bool acquire_audio()
{
    if (s_audio_owned) return true;

    // 获取播放独占权（防止与其他音频模块冲突）
    if (!audio_try_mark_playing()) {
        ESP_LOGW(MUSIC_TAG, "audio already playing, cannot start music");
        return false;
    }
    s_audio_owned = true;

    s_codec = audio_prepare_codec_for_playback();
    if (!s_codec) {
        ESP_LOGW(MUSIC_TAG, "Failed to acquire audio codec");
        audio_clear_playing();
        s_audio_owned = false;
        return false;
    }
    Display_AcquireDmaConservativeMode();
    ESP_LOGI(MUSIC_TAG, "Audio acquired OK (codec=%p)", (void *)s_codec);
    return true;
}

void release_audio()
{
    if (s_speaker_open && s_codec) {
        s_codec->CodecPort_CloseSpeaker();
        s_speaker_open = false;
        ESP_LOGI(MUSIC_TAG, "Speaker closed");
    }
    if (s_audio_owned) {
        audio_finish_playback();
        audio_clear_playing();
        s_codec = nullptr;
        s_audio_owned = false;
        Display_ReleaseDmaConservativeMode();
    }
}

bool open_speaker(int sample_rate)
{
    if (!s_codec) {
        ESP_LOGW(MUSIC_TAG, "open_speaker: no codec");
        return false;
    }
    if (s_speaker_open) return true;
    bool ok = s_codec->CodecPort_SetInfo("es8311", 1, sample_rate,
                                          kPlaybackChannelCount, kPlaybackBitsPerSample);
    if (ok) {
        s_speaker_open = true;
        s_codec->CodecPort_SetSpeakerVol(kDefaultSpeakerVolume);
        ESP_LOGI(MUSIC_TAG, "speaker opened: %d Hz", sample_rate);
    } else {
        ESP_LOGW(MUSIC_TAG, "CodecPort_SetInfo failed: %d Hz", sample_rate);
    }
    return ok;
}

// ====== WAV文件头解析 ======
struct WavInfo {
    int sample_rate;
    int channels;
    int bits_per_sample;
    size_t data_offset;   // PCM数据在文件中的偏移
    size_t data_size;     // PCM数据大小
};

bool parse_wav_header(FILE *f, WavInfo *info)
{
    if (!f || !info) return false;

    // 读取WAV文件头（前4KB，足够覆盖大多数含额外块的WAV）
    uint8_t *header = (uint8_t *)heap_caps_malloc(kWavHeaderMaxCheck, MALLOC_CAP_SPIRAM);
    if (!header) {
        ESP_LOGW(MUSIC_TAG, "WAV header buffer alloc failed");
        return false;
    }
    size_t read_len = fread(header, 1, kWavHeaderMaxCheck, f);
    if (read_len < 44) {
        free(header);
        return false;
    }

    // 检查RIFF标记
    if (memcmp(header, "RIFF", 4) != 0) { free(header); return false; }
    // 检查WAVE标记
    if (memcmp(header + 8, "WAVE", 4) != 0) { free(header); return false; }

    // 查找fmt 和 data 子块（顺序扫描，跳过JUNK/LIST等额外块）
    bool fmt_found = false;
    bool data_found = false;
    size_t offset = 12;
    while (offset + 8 <= read_len) {
        uint32_t chunk_size = header[offset + 4] |
                             (header[offset + 5] << 8) |
                             (header[offset + 6] << 16) |
                             (header[offset + 7] << 24);
        if (memcmp(header + offset, "fmt ", 4) == 0) {
            if (offset + 8 + 16 <= read_len) {  // fmt块至少16字节数据
                uint16_t audio_format = header[offset + 8] | (header[offset + 9] << 8);
                if (audio_format != 1) {
                    ESP_LOGW(MUSIC_TAG, "WAV: unsupported format %d (only PCM)", audio_format);
                    free(header);
                    return false;
                }
                // fmt块结构（块内偏移从offset+8开始）:
                //   +0:  audio_format(2B)
                //   +2:  num_channels(2B)
                //   +4:  sample_rate(4B)
                //   +8:  byte_rate(4B)
                //   +12: block_align(2B)
                //   +14: bits_per_sample(2B)
                info->channels = header[offset + 10] | (header[offset + 11] << 8);
                info->sample_rate = header[offset + 12] |
                                   (header[offset + 13] << 8) |
                                   (header[offset + 14] << 16) |
                                   (header[offset + 15] << 24);
                info->bits_per_sample = header[offset + 22] | (header[offset + 23] << 8);
                fmt_found = true;
                ESP_LOGI(MUSIC_TAG, "WAV fmt: %dHz %dch %dbit @ offset %u",
                         info->sample_rate, info->channels, info->bits_per_sample, (unsigned)offset);
            }
        } else if (memcmp(header + offset, "data", 4) == 0) {
            info->data_offset = offset + 8;
            info->data_size = chunk_size;
            data_found = true;
            ESP_LOGI(MUSIC_TAG, "WAV data: %u bytes @ offset %u", (unsigned)chunk_size, (unsigned)(offset + 8));
            break;
        }
        // 跳到下一个块
        size_t skip = 8 + chunk_size;
        if (chunk_size & 1) skip++; // 对齐到2字节边界
        offset += skip;
    }

    free(header);
    return fmt_found && data_found;
}

// ====== MP3播放 ======
void play_mp3_file(const char *path, uint8_t *pcm_output_buf)
{
    ESP_LOGI(MUSIC_TAG, "play_mp3_file: opening %s", path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(MUSIC_TAG, "Cannot open MP3: %s (errno=%d)", path, errno);
        return;
    }
    ESP_LOGI(MUSIC_TAG, "MP3 file opened OK");

    // 分配解码输入缓冲区（PSRAM）
    uint8_t *input_buf = (uint8_t *)heap_caps_malloc(kMp3InputBufSize, MALLOC_CAP_SPIRAM);
    if (!input_buf) {
        ESP_LOGW(MUSIC_TAG, "MP3 input buffer alloc failed");
        fclose(f);
        return;
    }

    HMP3Decoder mp3_dec = MP3InitDecoder();
    if (!mp3_dec) {
        ESP_LOGW(MUSIC_TAG, "MP3 decoder init failed");
        free(input_buf);
        fclose(f);
        return;
    }
    ESP_LOGI(MUSIC_TAG, "MP3 decoder init OK");

    // 跳过ID3v2 tag
    size_t input_stored = 0;
    {
        uint8_t peek[10];
        size_t preread = fread(peek, 1, 10, f);
        if (preread >= 10 && peek[0] == 'I' && peek[1] == 'D' && peek[2] == '3') {
            int id3_size = ((peek[6] & 0x7F) << 21) |
                           ((peek[7] & 0x7F) << 14) |
                           ((peek[8] & 0x7F) << 7) |
                           (peek[9] & 0x7F);
            int skip_total = 10 + id3_size;
            ESP_LOGI(MUSIC_TAG, "ID3v2 tag: %d bytes, skipping", skip_total);
            fseek(f, skip_total, SEEK_SET);
            input_stored = 0;
        } else {
            fseek(f, 0, SEEK_SET);
            input_stored = 0;
        }
    }

    int current_sample_rate = kDefaultSampleRate;
    bool speaker_opened = false;
    int16_t *pcm_buf = (int16_t *)heap_caps_malloc(1152 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        ESP_LOGW(MUSIC_TAG, "PCM buffer alloc failed");
        MP3FreeDecoder(mp3_dec);
        free(input_buf);
        fclose(f);
        return;
    }
    ESP_LOGI(MUSIC_TAG, "MP3 playback starting...");
    int decode_errors = 0;
    int frames_decoded = 0;

    // PCM预缓冲状态
    size_t pcm_stored = 0;           // 当前PCM缓冲区已用字节数
    bool prebuffering = true;        // 是否还在预缓冲阶段
    size_t pcm_write_offset = 0;     // 预缓冲排空时的写偏移

    while (!s_stop_requested && s_page_active.load() && !s_skip_to_next.load()) {
        // 读取文件数据填满输入缓冲区
        if (input_stored < kMp3InputBufSize) {
            size_t to_read = kMp3InputBufSize - input_stored;
            size_t nread = fread(input_buf + input_stored, 1, to_read, f);
            if (nread == 0 && input_stored == 0) break;  // 文件结束
            input_stored += nread;
            vTaskDelay(pdMS_TO_TICKS(1)); // 避免长时间占用CPU触发看门狗
        }

        // 寻找MP3同步字
        int offset = MP3FindSyncWord(input_buf, input_stored);
        if (offset < 0) {
            // 没有找到同步字，丢弃部分数据继续
            if (input_stored > 4) {
                memmove(input_buf, input_buf + input_stored - 4, 4);
                input_stored = 4;
            }
            continue;
        }

        // 解码
        int bytes_left = input_stored - offset;
        unsigned char *in_ptr = input_buf + offset;
        int ret = MP3Decode(mp3_dec, &in_ptr, &bytes_left, pcm_buf, 0);

        if (ret != 0) {
            // 解码错误，跳过这个同步字
            decode_errors++;
            if (decode_errors <= 3) {
                ESP_LOGW(MUSIC_TAG, "MP3Decode error %d at offset %d", ret, offset);
            }
            int consumed = (in_ptr - (input_buf + offset));
            if (consumed <= 0) consumed = 1;
            memmove(input_buf, input_buf + offset + consumed, input_stored - offset - consumed);
            input_stored -= offset + consumed;
            // 连续解码错误过多，放弃
            if (decode_errors > 100) {
                ESP_LOGW(MUSIC_TAG, "Too many MP3 decode errors, aborting");
                break;
            }
            continue;
        }

        decode_errors = 0; // 重置连续错误计数
        frames_decoded++;

        // 解码成功，获取帧信息
        int consumed = (in_ptr - (input_buf + offset));
        memmove(input_buf, input_buf + offset + consumed, input_stored - offset - consumed);
        input_stored -= offset + consumed;

        MP3FrameInfo frame_info;
        MP3GetLastFrameInfo(mp3_dec, &frame_info);

        // 检查采样率变化
        if (frame_info.samprate != current_sample_rate || !speaker_opened) {
            current_sample_rate = frame_info.samprate;
            if (!open_speaker(current_sample_rate)) {
                ESP_LOGW(MUSIC_TAG, "Failed to open speaker at %d Hz", current_sample_rate);
                break;
            }
            speaker_opened = true;
        }

        // PCM数据写入输出缓冲区
        int pcm_bytes = frame_info.outputSamps * sizeof(int16_t);
        if (pcm_bytes > 0 && pcm_output_buf) {
            if (prebuffering) {
                // 预缓冲阶段：攒数据
                if (pcm_stored + pcm_bytes <= kPcmOutputBufferSize) {
                    memcpy(pcm_output_buf + pcm_stored, pcm_buf, pcm_bytes);
                    pcm_stored += pcm_bytes;
                }
                // 攒够预缓冲量，开始输出
                if (pcm_stored >= kPcmPrebufferBytes) {
                    ESP_LOGI(MUSIC_TAG, "PCM prebuffer ready: %d bytes", (int)pcm_stored);
                    prebuffering = false;
                    pcm_write_offset = 0;
                }
            }

            if (!prebuffering) {
                // 排空预缓冲数据
                if (pcm_write_offset < pcm_stored) {
                    size_t remaining = pcm_stored - pcm_write_offset;
                    if (s_codec) {
                        s_codec->CodecPort_PlayWrite(pcm_output_buf + pcm_write_offset, (int)remaining);
                    }
                    pcm_write_offset = pcm_stored;
                }
                // 直接写当前帧
                if (s_codec) {
                    s_codec->CodecPort_PlayWrite(pcm_buf, pcm_bytes);
                }
            }
        }
    }

    // 文件结束：排空剩余PCM数据
    if (!prebuffering && pcm_write_offset < pcm_stored && s_codec) {
        size_t remaining = pcm_stored - pcm_write_offset;
        if (remaining > 0) {
            s_codec->CodecPort_PlayWrite(pcm_output_buf + pcm_write_offset, (int)remaining);
        }
    }

    ESP_LOGI(MUSIC_TAG, "MP3 playback ended: %d frames, stop=%d next=%d",
             frames_decoded, (int)s_stop_requested, s_skip_to_next.load());

    if (pcm_buf) free(pcm_buf);
    MP3FreeDecoder(mp3_dec);
    free(input_buf);
    fclose(f);
}

// ====== WAV播放（双缓冲流水线：预读+播放并行） ======

// WAV双缓冲上下文
struct WavDualBuf {
    int16_t *buf[2];            // 两个PCM缓冲区（PSRAM）
    volatile size_t filled[2];   // 每个缓冲区填充的int16_t样本数
    volatile int write_idx;     // 读取任务当前写哪个buf
    volatile int read_idx;      // 播放任务当前读哪个buf
    volatile bool reader_done;  // 读取任务完成
    FILE *file;                  // 文件句柄（读取任务用）
    WavInfo wav;                 // WAV格式信息
    SemaphoreHandle_t sem_read;  // 读取信号：buf可写
    SemaphoreHandle_t sem_play;  // 播放信号：buf可读
};

// WAV格式转换：raw → 16bit立体声PCM
static int wav_convert(const uint8_t *raw, size_t raw_len,
                       const WavInfo &wav, int16_t *out)
{
    int bytes_per_sample = wav.bits_per_sample / 8;
    int frame_size = bytes_per_sample * wav.channels;
    int num_frames = raw_len / frame_size;

    if (wav.bits_per_sample == 16) {
        if (wav.channels == 1) {
            const int16_t *src = (const int16_t *)raw;
            for (int i = num_frames - 1; i >= 0; i--) {
                out[i * 2] = src[i];
                out[i * 2 + 1] = src[i];
            }
            return num_frames * 2;
        } else {
            memcpy(out, raw, raw_len);
            return raw_len / sizeof(int16_t);
        }
    } else if (wav.bits_per_sample == 24) {
        int16_t *p = out;
        for (int i = 0; i < num_frames; i++) {
            for (int ch = 0; ch < wav.channels; ch++) {
                const uint8_t *s = raw + (i * frame_size) + ch * 3;
                *p++ = (int16_t)((s[1]) | (s[2] << 8));
            }
            if (wav.channels == 1) { int16_t v = p[-1]; *p++ = v; }
        }
        return p - out;
    } else { // 32-bit
        int16_t *p = out;
        for (int i = 0; i < num_frames; i++) {
            for (int ch = 0; ch < wav.channels; ch++) {
                const uint8_t *s = raw + (i * frame_size) + ch * 4;
                *p++ = (int16_t)((s[2]) | (s[3] << 8));
            }
            if (wav.channels == 1) { int16_t v = p[-1]; *p++ = v; }
        }
        return p - out;
    }
}

// 预读任务：循环从文件读raw → 格式转换 → 填入双缓冲区
void wav_prefill_task(void *arg)
{
    WavDualBuf *ctx = (WavDualBuf *)arg;
    uint8_t *raw_buf = (uint8_t *)heap_caps_malloc(kFileReadBufSize, MALLOC_CAP_SPIRAM);
    if (!raw_buf) {
        ctx->reader_done = true;
        xSemaphoreGive(ctx->sem_play);
        vTaskDelete(nullptr);
        return;
    }

    size_t remaining = ctx->wav.data_size;
    int cur = ctx->write_idx; // 初始0

    while (remaining > 0 && !s_stop_requested && s_page_active.load() && !s_skip_to_next.load()) {
        // 等待这个buf可写（播放侧已消费完）
        if (ctx->filled[cur] > 0) {
            xSemaphoreTake(ctx->sem_read, portMAX_DELAY);
        }

        // 循环读raw数据+转换，尽量填满当前buf
        size_t total_samples = 0;
        size_t max_samples = kWavBufSize / sizeof(int16_t);

        while (total_samples < max_samples - 4096 && remaining > 0) {
            size_t max_raw = remaining;
            if (max_raw > kFileReadBufSize) max_raw = kFileReadBufSize;

            size_t nread = fread(raw_buf, 1, max_raw, ctx->file);
            if (nread == 0) { remaining = 0; break; }
            remaining -= nread;

            int out_samples = wav_convert(raw_buf, nread, ctx->wav,
                                          ctx->buf[cur] + total_samples);
            total_samples += out_samples;

            if (nread < max_raw) break;
        }

        ctx->filled[cur] = total_samples;
        if (total_samples == 0) break;

        // 通知播放侧有数据
        xSemaphoreGive(ctx->sem_play);

        // 切到另一个buf
        cur = 1 - cur;
        ctx->write_idx = cur;
    }

    ctx->reader_done = true;
    // 确保播放侧能收到最后的数据
    xSemaphoreGive(ctx->sem_play);

    free(raw_buf);
    vTaskDelete(nullptr);
}

void play_wav_file(const char *path, uint8_t * /*pcm_output_buf*/)
{
    ESP_LOGI(MUSIC_TAG, "play_wav_file: opening %s", path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(MUSIC_TAG, "Cannot open WAV: %s (errno=%d)", path, errno);
        return;
    }

    WavInfo wav = {};
    if (!parse_wav_header(f, &wav)) {
        ESP_LOGW(MUSIC_TAG, "Invalid WAV header: %s", path);
        fclose(f);
        return;
    }

    ESP_LOGI(MUSIC_TAG, "WAV: %d Hz %d ch %d bit, data=%u @ %u",
             wav.sample_rate, wav.channels, wav.bits_per_sample,
             (unsigned)wav.data_size, (unsigned)wav.data_offset);

    if (wav.channels > 2 ||
        (wav.bits_per_sample != 16 && wav.bits_per_sample != 24 && wav.bits_per_sample != 32)) {
        ESP_LOGW(MUSIC_TAG, "WAV: unsupported %d-bit %d-ch", wav.bits_per_sample, wav.channels);
        fclose(f);
        return;
    }

    if (!open_speaker(wav.sample_rate)) {
        ESP_LOGW(MUSIC_TAG, "Failed to open speaker at %d Hz", wav.sample_rate);
        fclose(f);
        return;
    }

    fseek(f, wav.data_offset, SEEK_SET);

    // 初始化双缓冲上下文
    WavDualBuf ctx = {};
    ctx.buf[0] = (int16_t *)heap_caps_malloc(kWavBufSize, MALLOC_CAP_SPIRAM);
    ctx.buf[1] = (int16_t *)heap_caps_malloc(kWavBufSize, MALLOC_CAP_SPIRAM);
    ctx.filled[0] = 0;
    ctx.filled[1] = 0;
    ctx.write_idx = 0;
    ctx.read_idx = 0;
    ctx.reader_done = false;
    ctx.file = f;
    ctx.wav = wav;
    ctx.sem_read = xSemaphoreCreateBinary();
    ctx.sem_play = xSemaphoreCreateBinary();

    if (!ctx.buf[0] || !ctx.buf[1] || !ctx.sem_read || !ctx.sem_play) {
        ESP_LOGW(MUSIC_TAG, "WAV dual buffer alloc failed");
        if (ctx.buf[0]) free(ctx.buf[0]);
        if (ctx.buf[1]) free(ctx.buf[1]);
        if (ctx.sem_read) vSemaphoreDelete(ctx.sem_read);
        if (ctx.sem_play) vSemaphoreDelete(ctx.sem_play);
        fclose(f);
        return;
    }

    // 启动预读任务
    static StaticTask_t prefill_tcb = {};
    static StackType_t *prefill_stack = nullptr;
    if (!prefill_stack) {
        prefill_stack = (StackType_t *)heap_caps_malloc(8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    }

    TaskHandle_t prefill_handle = nullptr;
    if (prefill_stack) {
        prefill_handle = xTaskCreateStatic(wav_prefill_task, "wav_read",
                        8192, &ctx, 4, prefill_stack, &prefill_tcb);
    }

    if (!prefill_handle) {
        ESP_LOGW(MUSIC_TAG, "WAV prefill task create failed");
        free(ctx.buf[0]); free(ctx.buf[1]);
        vSemaphoreDelete(ctx.sem_read); vSemaphoreDelete(ctx.sem_play);
        fclose(f);
        return;
    }

    ESP_LOGI(MUSIC_TAG, "WAV dual-buffer started (%dKB x 2)", (int)(kWavBufSize / 1024));

    // 播放侧：等buf填满 → 写I2S → 通知可重写
    int cur = 0;
    while (!s_stop_requested && s_page_active.load() && !s_skip_to_next.load()) {
        // 等待当前buf有数据
        if (xSemaphoreTake(ctx.sem_play, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (ctx.reader_done) break;
            continue;
        }

        size_t samples = ctx.filled[cur];
        if (samples == 0 && ctx.reader_done) break;
        if (samples == 0) continue;

        // 写I2S
        if (s_codec) {
            s_codec->CodecPort_PlayWrite(ctx.buf[cur], (int)(samples * sizeof(int16_t)));
        }

        ctx.filled[cur] = 0;

        // 通知读取侧这个buf可以重写了
        xSemaphoreGive(ctx.sem_read);

        cur = 1 - cur;
        ctx.read_idx = cur;
    }

    // 等待预读任务退出：给它 sem_read 解锁，然后等 reader_done 标志
    for (int i = 0; i < 100; i++) {
        if (ctx.reader_done) break;
        // 解锁预读任务（它可能在等 sem_read）
        xSemaphoreGive(ctx.sem_read);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    free(ctx.buf[0]);
    free(ctx.buf[1]);
    vSemaphoreDelete(ctx.sem_read);
    vSemaphoreDelete(ctx.sem_play);
    fclose(f);

    ESP_LOGI(MUSIC_TAG, "WAV playback ended");
}

// ====== 播放控制 ======
void play_current_song(uint8_t *pcm_output_buf)
{
    int idx = s_song_index.load();
    char path[256] = {};
    if (!get_song_path(idx, path, sizeof(path))) {
        ESP_LOGW(MUSIC_TAG, "Cannot get song path for index %d", idx);
        update_state(kMusicError, "无法读取歌曲");
        return;
    }

    // 更新当前文件名和目录
    strlcpy(s_current_file, basename_of(path), sizeof(s_current_file));
    extract_dir(path, s_current_dir, sizeof(s_current_dir));

    ESP_LOGI(MUSIC_TAG, "Playing [%d]: %s", idx, path);
    update_state(kMusicPlaying, basename_of(path));

    if (is_mp3_file(path)) {
        play_mp3_file(path, pcm_output_buf);
    } else if (is_wav_file(path)) {
        play_wav_file(path, pcm_output_buf);
    }
    ESP_LOGI(MUSIC_TAG, "play_current_song finished, stop=%d skip=%d",
             (int)s_stop_requested, s_skip_to_next.load());
}

// ====== 播放任务 ======
void play_task(void *)
{
    ESP_LOGI(MUSIC_TAG, "play_task started");

    // 在任务层面分配2MB PCM输出缓冲区（跨歌曲复用，离开页面时释放）
    uint8_t *pcm_output_buf = (uint8_t *)heap_caps_malloc(kPcmOutputBufferSize, MALLOC_CAP_SPIRAM);
    if (!pcm_output_buf) {
        ESP_LOGE(MUSIC_TAG, "PCM output buffer alloc failed (%d bytes)", (int)kPcmOutputBufferSize);
        // 降级：用较小的缓冲区
        size_t fallback_size = 512 * 1024;
        pcm_output_buf = (uint8_t *)heap_caps_malloc(fallback_size, MALLOC_CAP_SPIRAM);
        if (!pcm_output_buf) {
            ESP_LOGE(MUSIC_TAG, "Even fallback PCM buffer alloc failed, exiting");
            s_play_task = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        ESP_LOGW(MUSIC_TAG, "Using fallback PCM buffer: %d bytes", (int)fallback_size);
    } else {
        ESP_LOGI(MUSIC_TAG, "PCM output buffer allocated: %d bytes", (int)kPcmOutputBufferSize);
    }

    int consecutive_failures = 0;
    while (s_page_active.load()) {
        // 等待启动信号
        if (s_song_count.load() <= 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        s_skip_to_next.store(false);
        MusicState prev_state = s_state.load();
        play_current_song(pcm_output_buf);

        // 检测是否播放失败（状态没有进入Playing就结束了）
        bool play_failed = (s_state.load() != kMusicPlaying &&
                            prev_state != kMusicPlaying);

        if (play_failed) {
            consecutive_failures++;
            if (consecutive_failures >= 10) {
                ESP_LOGW(MUSIC_TAG, "Too many consecutive failures, stopping");
                update_state(kMusicError, "连续播放失败");
                break;
            }
            // 短暂延迟避免疯狂循环
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            consecutive_failures = 0;
            // 正常播放结束或手动切歌，短暂延迟
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        // 如果没有被停止，自动播放下一首
        if (!s_stop_requested && s_page_active.load() && !s_skip_to_next.load()) {
            int next = (s_song_index.load() + 1) % s_song_count.load();
            s_song_index.store(next);
        }

        // 让出CPU给IDLE任务（喂看门狗）
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 播放任务自行清理：释放音频资源和PCM缓冲区
    release_audio();
    if (pcm_output_buf) free(pcm_output_buf);
    s_stop_requested = false;
    update_state(kMusicIdle, "");
    s_play_task = nullptr;
    ESP_LOGI(MUSIC_TAG, "play_task exiting (self-cleanup)");
    vTaskDelete(nullptr);
}

} // namespace

// ====== 公共接口 ======
void music_init()
{
    // 从PSRAM分配歌曲路径指针数组
    s_song_paths = (char **)heap_caps_malloc(kMaxSongCount * sizeof(char *), MALLOC_CAP_SPIRAM);
    if (s_song_paths) {
        memset(s_song_paths, 0, kMaxSongCount * sizeof(char *));
    }
    ESP_LOGI(MUSIC_TAG, "Music player initialized (paths=%s)", s_song_paths ? "OK" : "FAIL");
}

void music_set_page_active(bool active)
{
    bool current = s_page_active.load();
    if (active == current) return;  // 状态未变，快速跳过
    s_page_active.store(active);
    if (active) {
        // 防重入：已完成扫描且有播放任务则跳过
        // 注意：扫描失败时 s_play_task==nullptr，此时也需要跳过（用 s_scan_done 判断）
        if (s_scan_done) {
            // 已扫描过（无论成功或失败），如果有播放任务则跳过
            if (s_play_task) return;
            // 扫描失败但没有播放任务，也不应重复扫描
            if (s_scan_failed) {
                update_state(kMusicError, "SD卡无音乐");
                return;
            }
            // 扫描成功但没有播放任务（可能之前离开页面时被清理了），重新启动播放
        }

        // 进入页面：扫描SD卡并开始播放
        if (!perform_scan_if_needed()) {
            update_state(kMusicError, "SD卡无音乐");
            return;
        }
        int count = s_scan_count;
        s_song_count.store(count);
        s_song_index.store(0);

        if (count > 0) {
            update_state(kMusicLoading, "加载中...");
            s_stop_requested = false;

            if (!acquire_audio()) {
                update_state(kMusicError, "音频资源获取失败");
                return;
            }

            if (!s_play_task) {
                // 栈从PSRAM分配，避免DRAM不足
                if (!s_play_task_stack) {
                    s_play_task_stack = (StackType_t *)heap_caps_malloc(
                        kPlayTaskStackSize * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
                }
                if (!s_play_task_stack) {
                    ESP_LOGW(MUSIC_TAG, "play_task stack alloc failed (PSRAM)");
                } else {
                    s_play_task = xTaskCreateStatic(play_task, "music_play",
                                kPlayTaskStackSize, nullptr, 3,
                                s_play_task_stack, &s_play_task_tcb);
                    if (!s_play_task) {
                        ESP_LOGW(MUSIC_TAG, "play_task create static failed");
                    } else {
                        ESP_LOGI(MUSIC_TAG, "play_task created (PSRAM stack)");
                    }
                }
            }
        } else {
            update_state(kMusicIdle, "SD卡无音乐");
        }
    } else {
        // 离开页面：设停止标志，短超时等播放任务退出释放音频资源
        // s_page_active 已在函数开头设为 false
        // play_task 检测到 s_page_active==false 后会自行调用 release_audio()
        s_stop_requested = true;
        update_state(kMusicIdle, "");
        // 等待播放任务退出（最多500ms），确保音频资源释放给电台等模块
        for (int i = 0; i < 25 && s_play_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGI(MUSIC_TAG, "music_set_page_active(false) - play_task=%p", (void *)s_play_task);
    }
}

bool music_page_active()
{
    return s_page_active.load();
}

void music_get_snapshot(MusicSnapshot *out)
{
    if (!out) return;
    out->state = s_state.load();
    out->song_index = s_song_index.load();
    out->song_count = s_song_count.load();
    strlcpy(out->current_file, s_current_file, sizeof(out->current_file));
    strlcpy(out->status_text, s_status_text, sizeof(out->status_text));
    strlcpy(out->current_dir, s_current_dir, sizeof(out->current_dir));
    out->shuffle_mode = s_shuffle_mode.load();

    // 填充下3首文件名
    int count = s_song_count.load();
    int idx = s_song_index.load();
    for (int i = 0; i < 3; i++) {
        memset(out->upcoming_files[i], 0, sizeof(out->upcoming_files[i]));
        if (count > 0) {
            int next_idx = (idx + 1 + i) % count;
            if (s_song_paths && s_song_paths[next_idx]) {
                strlcpy(out->upcoming_files[i], basename_of(s_song_paths[next_idx]),
                        sizeof(out->upcoming_files[i]));
            }
        }
    }
}

void music_next_song()
{
    int count = s_song_count.load();
    if (count <= 0) return;
    int next = (s_song_index.load() + 1) % count;
    s_song_index.store(next);
    s_skip_to_next.store(true);
    ESP_LOGI(MUSIC_TAG, "Next song: %d", next);
}

void music_prev_song()
{
    int count = s_song_count.load();
    if (count <= 0) return;
    int prev = (s_song_index.load() - 1 + count) % count;
    s_song_index.store(prev);
    s_skip_to_next.store(true);
    ESP_LOGI(MUSIC_TAG, "Prev song: %d", prev);
}

void music_toggle_pause()
{
    MusicState current = s_state.load();
    if (current == kMusicPlaying) {
        update_state(kMusicPaused, "暂停");
        ESP_LOGI(MUSIC_TAG, "Paused");
    } else if (current == kMusicPaused) {
        update_state(kMusicPlaying, s_current_file);
        ESP_LOGI(MUSIC_TAG, "Resumed");
    }
}

bool music_is_playing()
{
    MusicState s = s_state.load();
    return s == kMusicPlaying || s == kMusicPaused || s == kMusicLoading;
}

void music_toggle_shuffle()
{
    bool current = s_shuffle_mode.load();
    s_shuffle_mode.store(!current);
    ESP_LOGI(MUSIC_TAG, "Shuffle mode: %s", !current ? "ON" : "OFF");

    // 如果切到shuffle模式且有歌曲列表，立即洗牌
    if (!current && s_scan_count > 1 && s_song_paths) {
        shuffle_song_list(s_scan_count);
        // 重置播放索引到0
        s_song_index.store(0);
        s_skip_to_next.store(true);
    }
}

bool music_shuffle_enabled()
{
    return s_shuffle_mode.load();
}
