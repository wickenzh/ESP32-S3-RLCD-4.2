// 管理网络电台的HTTP MP3流下载、MP3解码和I2S播放生命周期。
#include "radio_services.h"

#include "app_state.h"
#include "audio_services_internal.h"
#include "display_bsp.h"
#include "network_services.h"
#include "wifi_radio_state.h"

#include <atomic>
#include <cstring>
#include <unistd.h>
#include <errno.h>

#include "mp3dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "netdb.h"

#define RADIO_TAG "Radio"

namespace {

// 电台列表（蜻蜓FM 64k放第一位作为默认，码率低卡顿少）
struct RadioStation {
    const char *name;
    const char *url;
};

static constexpr RadioStation kStations[] = {
    {"蜻蜓FM-广东",  "http://lhttp.qingting.fm/live/1260/64k.mp3"},
    {"SomaFM-Groove", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"NPR News",     "http://npr-ice.streamguys1.com/live.mp3"},
    {"蜻蜓FM-苏州",  "http://lhttp.qingting.fm/live/2803/64k.mp3"},
};

constexpr int kStationCount = sizeof(kStations) / sizeof(kStations[0]);

// HTTP流读取缓冲区大小（每次从网络读取的字节数，增大减少系统调用次数）
constexpr size_t kHttpReadBufferSize = 8192;
// MP3解码PCM输出缓冲区大小（PSRAM分配，1MB绰绰有余）
constexpr size_t kPcmOutputBufferSize = 2 * 1024 * 1024;
// 解码器输入缓冲区大小
constexpr size_t kDecoderInputBufferSize = 32 * 1024;
// 电台任务栈大小（HTTP+MP3解码+日志格式化需要较大栈）
constexpr uint32_t kRadioTaskStackSize = 16384;
// 连接超时
constexpr int kHttpConnectTimeoutMs = 10000;
// 读取超时
constexpr int kHttpReadTimeoutMs = 15000;
// 页面切入后延迟连接时间
constexpr uint32_t kPageActiveDelayMs = 10000;
// 播放采样率（蜻蜓FM 64k MP3通常为22050或44100）
constexpr int kDefaultPlaybackSampleRate = 44100;
// 播放位深
constexpr int kPlaybackBitsPerSample = 16;
// 播放声道数（ES8311是立体声DAC，设为2声道）
constexpr int kPlaybackChannelCount = 2;
// 扬声器默认音量
constexpr int kDefaultSpeakerVolume = 80;
// PCM预缓冲：先解码攒够数据再开始写I2S，避免刚打开扬声器时I2S underrun
// 48kHz立体声16bit = 19200 B/s，20帧约460ms
constexpr size_t kPcmPrebufferBytes = 20 * 1152 * 2 * sizeof(int16_t);  // 20帧预缓冲
// socket recv最大连续超时重试次数
constexpr int kMaxRecvRetries = 5;
// 自动重连最大次数（0=无限重试）
constexpr int kMaxReconnectAttempts = 0;

// 状态
static std::atomic<int> s_station_index{0};
static std::atomic<RadioState> s_state{kRadioIdle};
static std::atomic<bool> s_page_active{false};
static std::atomic<uint32_t> s_uptime_sec{0};
static char s_station_name[32] = {};
static char s_status_text[64] = {};
static TaskHandle_t s_radio_task_handle = nullptr;
static volatile bool s_stop_requested = false;

// 音频资源（不再独立持有PM锁，由audio_finish_playback统一管理）
static bool s_audio_owned = false;
static CodecPort *s_codec = nullptr;
static bool s_speaker_open = false;

// HTTP客户端

// 事件组
static EventGroupHandle_t s_radio_events = nullptr;
constexpr uint32_t kRadioStartBit = BIT0;
constexpr uint32_t kRadioStopBit = BIT1;

// 辅助函数
void update_state(RadioState state, const char *status)
{
    s_state.store(state, std::memory_order_release);
    if (status) {
        strlcpy(s_status_text, status, sizeof(s_status_text));
    }
}

void update_station_name()
{
    int idx = s_station_index.load();
    if (idx >= 0 && idx < kStationCount) {
        strlcpy(s_station_name, kStations[idx].name, sizeof(s_station_name));
    }
}

bool open_speaker(int sample_rate)
{
    if (!s_codec) {
        return false;
    }
    if (s_speaker_open) {
        return true;
    }
// 打开ES8311扬声器（双声道STD模式）
    bool ok = s_codec->CodecPort_SetInfo("es8311", 1, sample_rate, kPlaybackChannelCount, kPlaybackBitsPerSample);
    if (ok) {
        s_speaker_open = true;
        s_codec->CodecPort_SetSpeakerVol(kDefaultSpeakerVolume);
        ESP_LOGI(RADIO_TAG, "radio speaker opened: %d Hz", sample_rate);
    }
    return ok;
}

void close_speaker()
{
    if (s_codec && s_speaker_open) {
        s_codec->CodecPort_CloseSpeaker();
        s_speaker_open = false;
    }
}

void release_audio()
{
    close_speaker();
    // 使用audio_services的标准释放流程（清理codec+PM锁+播放标记）
    if (s_audio_owned) {
        audio_finish_playback();
        s_audio_owned = false;
    }
    s_codec = nullptr;
}

// 获取音频资源（使用audio_services共享codec管理）
bool acquire_audio()
{
    // 先释放旧资源
    release_audio();

    // 获取播放独占权
    if (!audio_try_mark_playing()) {
        ESP_LOGW(RADIO_TAG, "audio already playing, cannot start radio");
        return false;
    }
    s_audio_owned = true;

    // 获取PM锁+共享CodecPort
    s_codec = audio_prepare_codec_for_playback();
    if (!s_codec) {
        ESP_LOGW(RADIO_TAG, "radio codec unavailable");
        audio_clear_playing();
        s_audio_owned = false;
        return false;
    }

    return true;
}

// ====== socket连接+HTTP请求+流播放（内层循环，可自动重连） ======

// 解析URL到hostname/port/path
void parse_station_url(const char *url, char *hostname, size_t hostname_size, int &port, const char *&path_start)
{
    const char *url_after_scheme = url;
    if (strncmp(url_after_scheme, "http://", 7) == 0) {
        url_after_scheme += 7;
    }
    const char *hp = url_after_scheme;
    while (*hp && *hp != ':' && *hp != '/') { hp++; }
    size_t hlen = (size_t)(hp - url_after_scheme);
    if (hlen > 0 && hlen < hostname_size) {
        memcpy(hostname, url_after_scheme, hlen);
        hostname[hlen] = '\0';
    }
    port = 80;
    path_start = "/";
    if (*hp == ':') {
        port = atoi(hp + 1);
        const char *slash = strchr(hp, '/');
        if (slash) { path_start = slash; }
    } else if (*hp == '/') {
        path_start = hp;
    }
}

// DNS解析（优先gethostbyname，fallback getaddrinfo）
bool resolve_dns(const char *hostname, struct in_addr &out_ip)
{
    struct hostent *he = gethostbyname(hostname);
    if (he && he->h_addr_list[0]) {
        memcpy(&out_ip, he->h_addr_list[0], sizeof(out_ip));
        return true;
    }
    ESP_LOGW(RADIO_TAG, "gethostbyname failed for %s, trying getaddrinfo", hostname);
    struct addrinfo hints = {}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(hostname, nullptr, &hints, &result);
    if (rc == 0 && result) {
        struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
        out_ip = addr->sin_addr;
        freeaddrinfo(result);
        return true;
    }
    if (result) freeaddrinfo(result);
    return false;
}

// 建立HTTP连接，返回socket fd（<0表示失败）
int http_connect(const char *hostname, int port, struct in_addr &server_ip)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(RADIO_TAG, "socket create failed: errno=%d", errno);
        return -1;
    }

    // 设置socket超时
    struct timeval recv_timeout = {15, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
    // 不设置SO_RCVBUF，用LWIP默认值（通常4KB）
    // 过大的SO_RCVBUF会占用DMA内存导致SPI LCD刷新失败

    struct sockaddr_in dest_addr = {};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    dest_addr.sin_addr = server_ip;

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGW(RADIO_TAG, "socket connect failed: errno=%d", errno);
        close(sock);
        return -1;
    }

    return sock;
}

// 发送HTTP GET请求并解析响应头，返回HTTP状态码（<0表示错误）
int http_send_request(int sock, const char *path, const char *hostname, int &out_icy_metaint)
{
    // 使用HTTP/1.0，服务器不会返回chunked编码
    // （HTTP/1.1默认允许chunked，我们的MP3解析器不处理chunked标记）
    char http_req[512] = {};
    snprintf(http_req, sizeof(http_req),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "User-Agent: ESP32-Radio/1.0\r\n"
             "Accept: */*\r\n"
             "Icy-MetaData: 0\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, hostname);

    if (write(sock, http_req, strlen(http_req)) < 0) {
        ESP_LOGW(RADIO_TAG, "socket write failed: errno=%d", errno);
        return -1;
    }

    // 读取HTTP响应头
    char resp_line[256] = {};
    out_icy_metaint = 0;
    int status_code = 0;

    while (true) {
        int line_pos = 0;
        while (line_pos < (int)sizeof(resp_line) - 1) {
            int n = recv(sock, &resp_line[line_pos], 1, 0);
            if (n <= 0) {
                ESP_LOGW(RADIO_TAG, "recv header failed: n=%d errno=%d", n, errno);
                return -1;
            }
            if (resp_line[line_pos] == '\n') {
                resp_line[line_pos + 1] = '\0';
                break;
            }
            if (resp_line[line_pos] != '\r') {
                line_pos++;
            }
        }

        // 空行表示头部结束
        if (line_pos == 0 || resp_line[0] == '\n' || (resp_line[0] == '\r' && resp_line[1] == '\n')) {
            break;
        }

        if (strncmp(resp_line, "HTTP/", 5) == 0) {
            char *sp = strchr(resp_line, ' ');
            if (sp) status_code = atoi(sp + 1);
            ESP_LOGI(RADIO_TAG, "HTTP response: %s", resp_line);
        }

        if (strncasecmp(resp_line, "icy-metaint:", 12) == 0) {
            out_icy_metaint = atoi(resp_line + 12);
            ESP_LOGI(RADIO_TAG, "ICY metaint: %d", out_icy_metaint);
        }

        if (strncasecmp(resp_line, "icy-name:", 9) == 0) {
            char *val = resp_line + 9;
            while (*val == ' ') val++;
            char *eol = val + strlen(val) - 1;
            while (eol > val && (*eol == '\r' || *eol == '\n')) *eol-- = '\0';
            ESP_LOGI(RADIO_TAG, "ICY name: %s", val);
        }
    }

    return status_code;
}

// ====== 电台主任务（双层循环：外层等启停信号，内层连socket+播放+自动重连） ======

void radio_task(void *arg)
{
    ESP_LOGI(RADIO_TAG, "radio task started");

    for (;;) {
        // 外层循环：等待启动/停止信号
        EventBits_t bits = xEventGroupWaitBits(s_radio_events,
                                                kRadioStartBit | kRadioStopBit,
                                                pdTRUE,
                                                pdFALSE,
                                                portMAX_DELAY);
        if (bits & kRadioStopBit) {
            break;
        }
        if (!(bits & kRadioStartBit)) {
            continue;
        }

        s_stop_requested = false;
        int station_idx = s_station_index.load();
        if (station_idx < 0 || station_idx >= kStationCount) {
            station_idx = 0;
            s_station_index.store(0);
        }

        update_state(kRadioConnecting, "连接中...");
        update_station_name();
        ESP_LOGI(RADIO_TAG, "connecting to station: %s", kStations[station_idx].name);

        // 确保WiFi已连接
        if (!wifi_radio_on_load()) {
            ESP_LOGI(RADIO_TAG, "wifi off, starting wifi radio");
            start_wifi_radio(false);
            if (!wait_for_wifi_connected(15000)) {
                ESP_LOGW(RADIO_TAG, "wifi connect timeout, cannot start radio");
                update_state(kRadioError, "WiFi连接超时");
                release_audio();
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }

        // 获取音频资源
        if (!acquire_audio()) {
            update_state(kRadioError, "音频资源获取失败");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // 启用SPI DMA保守模式：I2S音频播放占用DMA内存，
        // 会导致SPI LCD刷新分配不到2KB的DMA块
        // 保守模式将SPI传输块从2KB降到512B，避免刷新失败
        Display_AcquireDmaConservativeMode();

        // 分配持久缓冲区（跨重连复用，避免反复malloc/free）
        uint8_t *http_buffer = (uint8_t *)heap_caps_malloc(kHttpReadBufferSize, MALLOC_CAP_SPIRAM);
        uint8_t *pcm_buffer = (uint8_t *)heap_caps_malloc(kPcmOutputBufferSize, MALLOC_CAP_SPIRAM);
        uint8_t *input_buffer = (uint8_t *)heap_caps_malloc(kDecoderInputBufferSize, MALLOC_CAP_SPIRAM);

        if (!http_buffer || !pcm_buffer || !input_buffer) {
            ESP_LOGW(RADIO_TAG, "buffer allocation failed");
            update_state(kRadioError, "内存不足");
            if (http_buffer) free(http_buffer);
            if (pcm_buffer) free(pcm_buffer);
            if (input_buffer) free(input_buffer);
            release_audio();
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // ====== 内层循环：socket连接+播放+自动重连（保留音频资源） ======
        int reconnect_count = 0;

        while (!s_stop_requested && s_page_active.load()) {
            // 解析URL
            char hostname[128] = {};
            int port = 80;
            const char *path_start = "/";
            parse_station_url(kStations[station_idx].url, hostname, sizeof(hostname), port, path_start);

            // DNS解析
            ESP_LOGI(RADIO_TAG, "DNS resolving: %s", hostname);
            struct in_addr server_ip = {};
            if (!resolve_dns(hostname, server_ip)) {
                ESP_LOGW(RADIO_TAG, "DNS resolve failed for %s", hostname);
                update_state(kRadioError, "DNS解析失败");
                vTaskDelay(pdMS_TO_TICKS(5000));
                if (s_stop_requested || !s_page_active.load()) break;
                continue;
            }

            char ip_str[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &server_ip, ip_str, sizeof(ip_str));
            ESP_LOGI(RADIO_TAG, "DNS resolved: %s -> %s", hostname, ip_str);

            // 建立HTTP连接
            update_state(kRadioConnecting, "连接中...");
            int sock = http_connect(hostname, port, server_ip);
            if (sock < 0) {
                update_state(kRadioError, "TCP连接失败");
                vTaskDelay(pdMS_TO_TICKS(5000));
                if (s_stop_requested || !s_page_active.load()) break;
                continue;
            }

            // 发送HTTP请求
            int icy_metaint = 0;
            int status_code = http_send_request(sock, path_start, hostname, icy_metaint);
            if (status_code != 200) {
                ESP_LOGW(RADIO_TAG, "HTTP status not 200: %d", status_code);
                close(sock);
                update_state(kRadioError, status_code <= 0 ? "HTTP连接失败" : "连接被拒绝");
                vTaskDelay(pdMS_TO_TICKS(5000));
                if (s_stop_requested || !s_page_active.load()) break;
                continue;
            }

            // 初始化Helix MP3解码器
            HMP3Decoder mp3_dec = MP3InitDecoder();
            if (!mp3_dec) {
                ESP_LOGW(RADIO_TAG, "MP3 decoder init failed");
                close(sock);
                update_state(kRadioError, "解码器初始化失败");
                vTaskDelay(pdMS_TO_TICKS(5000));
                if (s_stop_requested || !s_page_active.load()) break;
                continue;
            }

            update_state(kRadioPlaying, "播放中");

            // 预读流开头数据，检查并跳过ID3v2 tag
            size_t input_stored = 0;
            {
                int preread = 0;
                while (preread < 10) {
                    int n = recv(sock, (char *)input_buffer + preread, 10 - preread, 0);
                    if (n <= 0) break;
                    preread += n;
                }
                if (preread >= 10 && input_buffer[0] == 'I' && input_buffer[1] == 'D' && input_buffer[2] == '3') {
                    int id3_size = ((input_buffer[6] & 0x7F) << 21) |
                                   ((input_buffer[7] & 0x7F) << 14) |
                                   ((input_buffer[8] & 0x7F) << 7) |
                                   (input_buffer[9] & 0x7F);
                    int skip_total = 10 + id3_size;
                    ESP_LOGI(RADIO_TAG, "ID3v2 tag found, total=%d bytes, skipping", skip_total);
                    while (preread < skip_total) {
                        int to_read = skip_total - preread;
                        if (to_read > (int)kHttpReadBufferSize) to_read = kHttpReadBufferSize;
                        int n = recv(sock, (char *)http_buffer, to_read, 0);
                        if (n <= 0) break;
                        preread += n;
                    }
                    input_stored = 0;
                    ESP_LOGI(RADIO_TAG, "ID3v2 tag skipped");
                } else {
                    input_stored = preread;
                    if (preread > 0) {
                        char hex[64];
                        int pos = 0;
                        for (int i = 0; i < preread && i < 16; i++) {
                            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", input_buffer[i]);
                        }
                        ESP_LOGI(RADIO_TAG, "stream starts with: %s", hex);
                    }
                }
            }

            bool speaker_opened = false;
            int current_sample_rate = kDefaultPlaybackSampleRate;
            TickType_t start_tick = xTaskGetTickCount();
            bool playback_ok = true;
            int icy_bytes_until_meta = icy_metaint;
            int consecutive_decode_errors = 0;

            // PCM预缓冲：解码数据先累积，攒够后再开始写I2S
            size_t pcm_stored = 0;          // 当前PCM缓冲区中的数据量
            bool pcm_prebuffering = true;    // 是否正在预缓冲阶段
            size_t pcm_write_offset = 0;     // I2S消费偏移

            // ====== 流式播放循环 ======
            int recv_retry_count = 0;

            while (!s_stop_requested && s_page_active.load()) {
                // 1. 从socket读取数据
                int read_len = recv(sock, (char *)http_buffer, kHttpReadBufferSize, 0);
                if (read_len < 0) {
                    int err = errno;
                    if (err == EAGAIN || err == EWOULDBLOCK) {
                        recv_retry_count++;
                        if (recv_retry_count <= kMaxRecvRetries) {
                            ESP_LOGW(RADIO_TAG, "socket recv timeout (retry %d/%d)", recv_retry_count, kMaxRecvRetries);
                            vTaskDelay(pdMS_TO_TICKS(500));
                            continue;
                        }
                        ESP_LOGW(RADIO_TAG, "socket recv timeout after %d retries, reconnecting", kMaxRecvRetries);
                        break;
                    }
                    if (err == EINTR) {
                        continue;
                    }
                    ESP_LOGW(RADIO_TAG, "socket recv error: errno=%d, reconnecting", err);
                    break;
                }
                if (read_len == 0) {
                    ESP_LOGW(RADIO_TAG, "server closed connection, reconnecting");
                    break;
                }
                recv_retry_count = 0;

                // 2. 剥离ICY元数据块，将纯净MP3数据写入输入缓冲区
                size_t src_pos = 0;
                while (src_pos < (size_t)read_len && input_stored < kDecoderInputBufferSize - 1) {
                    if (icy_metaint > 0 && icy_bytes_until_meta == 0) {
                        uint8_t meta_len_byte = http_buffer[src_pos++];
                        int meta_size = meta_len_byte * 16;
                        while (meta_size > 0 && src_pos < (size_t)read_len) {
                            int skip = (meta_size < (int)(read_len - src_pos)) ? meta_size : (int)(read_len - src_pos);
                            src_pos += skip;
                            meta_size -= skip;
                        }
                        while (meta_size > 0) {
                            uint8_t tmp[256];
                            int to_read = (meta_size < 256) ? meta_size : 256;
                            int n = recv(sock, (char *)tmp, to_read, 0);
                            if (n <= 0) break;
                            meta_size -= n;
                        }
                        icy_bytes_until_meta = icy_metaint;
                    } else {
                        size_t mp3_bytes = (size_t)read_len - src_pos;
                        if (icy_metaint > 0 && mp3_bytes > (size_t)icy_bytes_until_meta) {
                            mp3_bytes = (size_t)icy_bytes_until_meta;
                        }
                        size_t space = kDecoderInputBufferSize - input_stored;
                        if (mp3_bytes > space) mp3_bytes = space;
                        if (mp3_bytes > 0) {
                            memcpy(input_buffer + input_stored, http_buffer + src_pos, mp3_bytes);
                            input_stored += mp3_bytes;
                            src_pos += mp3_bytes;
                            if (icy_metaint > 0) icy_bytes_until_meta -= (int)mp3_bytes;
                        } else {
                            break;
                        }
                    }
                }

                // 3. 使用Helix MP3解码器持续解码
                // 解码阈值降低到1024，让低码率流也能及时输出
                while (input_stored > 1024 && !s_stop_requested) {
                    int sync_pos = MP3FindSyncWord(input_buffer, input_stored);
                    if (sync_pos < 0) {
                        if (input_stored > 4) {
                            memmove(input_buffer, input_buffer + input_stored - 4, 4);
                            input_stored = 4;
                        }
                        break;
                    }
                    if (sync_pos > 0) {
                        memmove(input_buffer, input_buffer + sync_pos, input_stored - sync_pos);
                        input_stored -= sync_pos;
                    }

                    unsigned char *inbuf_ptr = input_buffer;
                    int bytes_left = (int)input_stored;
                    int dec_result = MP3Decode(mp3_dec, &inbuf_ptr, &bytes_left, (short *)pcm_buffer, 0);

                    if (dec_result == 0) {
                        consecutive_decode_errors = 0;
                        int consumed = (int)input_stored - bytes_left;
                        if (consumed <= 0) consumed = (int)input_stored;

                        MP3FrameInfo frame_info;
                        MP3GetLastFrameInfo(mp3_dec, &frame_info);
                        if (frame_info.samprate > 0 && frame_info.samprate != current_sample_rate) {
                            current_sample_rate = frame_info.samprate;
                            ESP_LOGI(RADIO_TAG, "MP3: %d Hz, %d ch, %d kbps",
                                     frame_info.samprate, frame_info.nChans, frame_info.bitrate / 1000);
                        }

                        if (!speaker_opened && current_sample_rate > 0) {
                            if (open_speaker(current_sample_rate)) {
                                speaker_opened = true;
                            } else {
                                ESP_LOGW(RADIO_TAG, "speaker open failed");
                                playback_ok = false;
                                break;
                            }
                        }

                        if (speaker_opened && frame_info.outputSamps > 0) {
                            size_t pcm_bytes = frame_info.outputSamps * sizeof(int16_t);

                            if (pcm_prebuffering) {
                                // 预缓冲阶段：Helix输出在pcm_buffer开头，追加到后面累积
                                if (pcm_stored + pcm_bytes <= kPcmOutputBufferSize) {
                                    // 只在源和目标不重叠时拷贝
                                    memmove(pcm_buffer + pcm_stored, pcm_buffer, pcm_bytes);
                                    pcm_stored += pcm_bytes;
                                }
                                // 攒够预缓冲量，开始连续写入I2S
                                if (pcm_stored >= kPcmPrebufferBytes) {
                                    ESP_LOGI(RADIO_TAG, "PCM prebuffer ready: %d bytes", (int)pcm_stored);
                                    pcm_prebuffering = false;
                                    pcm_write_offset = 0;
                                }
                            }

                            if (!pcm_prebuffering) {
                                // 先排空预缓冲数据
                                if (pcm_write_offset < pcm_stored) {
                                    size_t remaining = pcm_stored - pcm_write_offset;
                                    int written = s_codec->CodecPort_PlayWrite(
                                        pcm_buffer + pcm_write_offset, (int)remaining);
                                    if (written != ESP_CODEC_DEV_OK) {
                                        ESP_LOGW(RADIO_TAG, "playback write failed");
                                        playback_ok = false;
                                        break;
                                    }
                                    pcm_write_offset = pcm_stored;
                                }
                                // 直接写入当前帧
                                int written = s_codec->CodecPort_PlayWrite(pcm_buffer, (int)pcm_bytes);
                                if (written != ESP_CODEC_DEV_OK) {
                                    ESP_LOGW(RADIO_TAG, "playback write failed");
                                    playback_ok = false;
                                    break;
                                }
                            }
                        }

                        if ((size_t)consumed < input_stored) {
                            size_t remaining = input_stored - consumed;
                            memmove(input_buffer, input_buffer + consumed, remaining);
                            input_stored = remaining;
                        } else {
                            input_stored = 0;
                        }
                    } else {
                        consecutive_decode_errors++;
                        if (consecutive_decode_errors <= 3) {
                            ESP_LOGW(RADIO_TAG, "MP3 decode error: %d, bytes_left=%d, stored=%d",
                                     dec_result, bytes_left, (int)input_stored);
                        }

                        int consumed = (int)input_stored - bytes_left;
                        if (consumed > 0 && (size_t)consumed < input_stored) {
                            memmove(input_buffer, input_buffer + consumed, input_stored - consumed);
                            input_stored -= consumed;
                        } else {
                            if (input_stored > 1) {
                                memmove(input_buffer, input_buffer + 1, input_stored - 1);
                                input_stored--;
                            }
                        }

                        if (consecutive_decode_errors > 30) {
                            ESP_LOGW(RADIO_TAG, "too many consecutive errors, breaking decode loop");
                            break;
                        }
                        continue;
                    }
                }

                // 更新播放时长
                if (speaker_opened) {
                    TickType_t now = xTaskGetTickCount();
                    s_uptime_sec.store((uint32_t)((now - start_tick) * portTICK_PERIOD_MS / 1000),
                                       std::memory_order_relaxed);
                }

                if (!playback_ok) {
                    break;
                }
            }

            // 清理本次连接的资源（socket+MP3解码器）
            ESP_LOGI(RADIO_TAG, "stream ended, closing socket");
            MP3FreeDecoder(mp3_dec);
            close(sock);

            if (!s_stop_requested && s_page_active.load() && playback_ok) {
                // 自动重连（保留音频资源，避免重新抢夺）
                reconnect_count++;
                ESP_LOGI(RADIO_TAG, "reconnecting in 3s... (attempt %d)", reconnect_count);
                update_state(kRadioError, "连接中断，重连中...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                // 关闭扬声器但不释放codec，重连后重新打开
                close_speaker();
            } else {
                // 主动停止或播放失败，退出内层循环
                break;
            }
        }

        // 释放持久缓冲区+音频资源
        free(http_buffer);
        free(pcm_buffer);
        free(input_buffer);
        // 释放SPI DMA保守模式
        Display_ReleaseDmaConservativeMode();
        release_audio();
        update_state(kRadioIdle, "已停止");
        s_uptime_sec.store(0);
    }

    // 最终清理
    release_audio();
    s_radio_task_handle = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

void radio_init()
{
    if (!s_radio_events) {
        s_radio_events = xEventGroupCreate();
        ESP_LOGI(RADIO_TAG, "radio_init: event group created, free heap=%u", (unsigned)xPortGetFreeHeapSize());
    }
    update_station_name();
    strlcpy(s_status_text, "等待连接", sizeof(s_status_text));
    ESP_LOGI(RADIO_TAG, "radio_init done, station count=%d", kStationCount);
}

void radio_set_page_active(bool active)
{
    bool was_active = s_page_active.load();
    s_page_active.store(active, std::memory_order_release);

    if (active && !was_active) {
        ESP_LOGI(RADIO_TAG, "page activated, starting in %d seconds", kPageActiveDelayMs / 1000);
        // 启动延迟任务
        BaseType_t ret = xTaskCreate([](void *arg) {
            ESP_LOGI(RADIO_TAG, "delay task: waiting %d seconds", kPageActiveDelayMs / 1000);
            vTaskDelay(pdMS_TO_TICKS(kPageActiveDelayMs));

            // 再次检查页面是否仍然激活
            if (!s_page_active.load()) {
                ESP_LOGI(RADIO_TAG, "page deactivated during delay, abort start");
                vTaskDelete(nullptr);
                return;
            }

            // 创建或唤醒电台任务（栈从PSRAM分配）
            if (!s_radio_task_handle) {
                static StaticTask_t s_task_tcb = {};
                static StackType_t *s_task_stack = nullptr;
                if (!s_task_stack) {
                    s_task_stack = (StackType_t *)heap_caps_malloc(kRadioTaskStackSize * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
                }
                if (!s_task_stack) {
                    ESP_LOGE(RADIO_TAG, "radio_task stack alloc failed (PSRAM)!");
                    vTaskDelete(nullptr);
                    return;
                }
                s_radio_task_handle = xTaskCreateStatic(radio_task, "radio_task",
                            kRadioTaskStackSize, nullptr,
                            configMAX_PRIORITIES - 3,
                            s_task_stack, &s_task_tcb);
                if (!s_radio_task_handle) {
                    ESP_LOGE(RADIO_TAG, "radio_task create static failed!");
                    vTaskDelete(nullptr);
                    return;
                }
                ESP_LOGI(RADIO_TAG, "radio_task created (PSRAM stack), handle=%p", s_radio_task_handle);
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            ESP_LOGI(RADIO_TAG, "sending start bit");
            xEventGroupSetBits(s_radio_events, kRadioStartBit);
            vTaskDelete(nullptr);
        }, "radio_start", 4096, nullptr, configMAX_PRIORITIES - 3, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(RADIO_TAG, "radio_start delay task create failed! ret=%d", (int)ret);
        } else {
            ESP_LOGI(RADIO_TAG, "radio_start delay task created");
        }
    } else if (!active && was_active) {
        ESP_LOGI(RADIO_TAG, "page deactivated, stopping radio");
        s_stop_requested = true;
        update_state(kRadioIdle, "已停止");
        s_uptime_sec.store(0);
        // 通知任务停止
        xEventGroupSetBits(s_radio_events, kRadioStopBit);
    }
}

bool radio_page_active()
{
    return s_page_active.load(std::memory_order_acquire);
}

void radio_get_snapshot(RadioSnapshot *out)
{
    if (!out) return;
    out->state = s_state.load(std::memory_order_acquire);
    strlcpy(out->station_name, s_station_name, sizeof(out->station_name));
    strlcpy(out->status_text, s_status_text, sizeof(out->status_text));
    out->uptime_sec = s_uptime_sec.load(std::memory_order_relaxed);
}

void radio_next_station()
{
    int idx = s_station_index.load();
    idx = (idx + 1) % kStationCount;
    s_station_index.store(idx);
    update_station_name();

    // 如果正在播放，停止当前流并重新开始
    bool was_active = s_page_active.load();
    if (was_active) {
        s_stop_requested = true;
        // 等待radio_task退出播放循环（最多2秒）
        for (int i = 0; i < 20 && s_state.load() == kRadioPlaying; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_stop_requested = false;
        update_state(kRadioIdle, "切换电台...");

        // 重新启动
        xEventGroupSetBits(s_radio_events, kRadioStartBit);
    }
}

void radio_prev_station()
{
    int idx = s_station_index.load();
    idx = (idx - 1 + kStationCount) % kStationCount;
    s_station_index.store(idx);
    update_station_name();

    bool was_active = s_page_active.load();
    if (was_active) {
        s_stop_requested = true;
        for (int i = 0; i < 20 && s_state.load() == kRadioPlaying; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_stop_requested = false;
        update_state(kRadioIdle, "切换电台...");

        xEventGroupSetBits(s_radio_events, kRadioStartBit);
    }
}

int radio_current_station_index()
{
    return s_station_index.load();
}

int radio_station_count()
{
    return kStationCount;
}

bool radio_network_keepalive_active()
{
    RadioState state = s_state.load(std::memory_order_acquire);
    return s_page_active.load(std::memory_order_acquire) &&
           (state == kRadioConnecting || state == kRadioPlaying);
}
