// 复用本项目网络与电源服务对接小智官方激活和 WebSocket 会话。
#include "xiaozhi_ai.h"

#include "app_tick_time.h"
#include "app_state.h"
#include "alarm_services.h"
#include "audio_services.h"
#include "network_services.h"
#include "network_credentials_state.h"
#include "network_https_resources.h"
#include "scoped_heap_buffer.h"
#include "ui_views.h"
#include "xiaozhi_activation_flow.h"
#include "xiaozhi_activation_retry_policy.h"
#include "xiaozhi_activation_storage.h"
#include "xiaozhi_audio_frames.h"
#include "xiaozhi_conversation_events.h"
#include "xiaozhi_conversation_io.h"
#include "xiaozhi_idle_wait_policy.h"
#include "xiaozhi_mcp.h"
#include "xiaozhi_conversation_policy.h"
#include "xiaozhi_power_session.h"
#include "xiaozhi_runtime_diagnostics.h"
#include "xiaozhi_server_hello_parser.h"
#include "xiaozhi_snapshot_state.h"
#include "xiaozhi_task_start_retry.h"
#include "xiaozhi_tts_playback.h"
#include "xiaozhi_voice.h"
#include "xiaozhi_voice_codec.h"
#include "xiaozhi_websocket_session.h"
#include "weather_city_mcp.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_transport.h>
#include <esp_transport_ws.h>
#include <esp_opus_dec.h>
#include <atomic>
#include <string.h>

namespace {
using xiaozhi_websocket::WebsocketSession;
using xiaozhi_websocket::close_websocket;
using xiaozhi_websocket::open_websocket;
using xiaozhi_websocket::websocket_send_listen_start;
using xiaozhi_websocket::websocket_send_wake_abort;
using xiaozhi_conversation_events::clear_tts_timing_state;
using xiaozhi_conversation_events::publish_pending_assistant_text;
using xiaozhi_conversation_events::tts_final_frames_settled;
using xiaozhi_conversation_events::user_subtitle_hold_active;

constexpr uint32_t kActivationRetryMs = 15000;
constexpr uint32_t kTaskStartRetryMs = 5000;
constexpr uint32_t kLoopIdleMs = 500;
constexpr TickType_t kPomodoroAudioPausedWaitTicks = portMAX_DELAY;
constexpr uint32_t kWakeAudioPerformanceSettleMs = 40;
constexpr uint32_t kConversationIdleTimeoutMs = 30000;
constexpr uint32_t kMcpWeatherRefreshPollMs = 500;
constexpr uint32_t kMcpWeatherRefreshTimeoutMs = 150000;
constexpr int kIncomingAudioBufferSize = 4096;
// 官方实现为 Opus 编解码任务预留 24 KiB。这里的任务还负责 WebSocket
// 协议，因此至少保持相同栈空间，避免进入 SILK 编码器后破坏任务栈。
constexpr uint32_t kXiaozhiTaskStackSize = 24 * 1024;
constexpr const char *kWifiStatus = "正在连接Wi-Fi";
constexpr const char *kReadyStatus = "等待唤醒词";
constexpr const char *kErrorStatus = "小智服务不可用";
constexpr const char *kNoWifiDetail = "请先在系统设置中配置 Wi-Fi";
constexpr const char *kOfflineDetail = "离线模式下无法使用小智 AI";
constexpr const char *kBoundDetail = "说出唤醒词即可开始对话";
constexpr const char *kWakeWordFailureDetail = "语音监听初始化失败，请稍后重试";
constexpr EventBits_t kAiPageActiveBit = BIT0;
constexpr EventBits_t kAiWakeBit = BIT1;
#define XIAOZHI_STATE_INIT_FAILED_LOG "Xiaozhi AI state initialization failed"

struct VoiceIoBuffers {
    char incoming[kIncomingAudioBufferSize] = {};
    XiaozhiAudioDecodeBuffers audio;
};

EventGroupHandle_t s_events = nullptr;
TaskHandle_t s_task_handle = nullptr;
std::atomic<bool> s_task_exited{true};
std::atomic<bool> s_alarm_suspended{false};
std::atomic<bool> s_pomodoro_audio_suspended{false};
static_assert(kXiaozhiTaskStackSize % sizeof(StackType_t) == 0,
              "Xiaozhi task stack must align to StackType_t");
static_assert(kWakeAudioPerformanceSettleMs > 0,
              "Xiaozhi wake audio performance settle time must be positive");
static_assert(pdMS_TO_TICKS(kTaskStartRetryMs) > 0,
              "Xiaozhi task start retry delay must convert to ticks");
static_assert(pdMS_TO_TICKS(kMcpWeatherRefreshPollMs) > 0,
              "MCP weather refresh poll delay must convert to ticks");
static_assert(kMcpWeatherRefreshTimeoutMs > kMcpWeatherRefreshPollMs,
              "MCP weather refresh timeout must exceed its poll delay");
// The main AI task reads NVS. Flash/NVS operations temporarily disable the
// external-memory cache, so its stack must stay in internal DRAM. Reserving it
// statically avoids the late 24 KiB contiguous-heap allocation failure.
StackType_t s_task_stack[kXiaozhiTaskStackSize / sizeof(StackType_t)];
StaticTask_t s_task_buffer;
bool s_voice_started = false;
XiaozhiTaskStartRetryState<TickType_t> s_task_start_retry;

void notify_ai_voice_event()
{
    if (s_events) {
        xEventGroupSetBits(s_events, kAiWakeBit);
    }
}

void reclaim_ai_task_if_exited()
{
    if (!s_task_handle || !s_task_exited.load() ||
        eTaskGetState(s_task_handle) != eSuspended) {
        return;
    }
    vTaskDelete(s_task_handle);
    s_task_handle = nullptr;
}

void log_voice_resources(const char *stage)
{
    ESP_LOGI(TAG,
             "xiaozhi resources %s: stack_free=%u dma_free=%u dma_largest=%u "
             "internal_free=%u internal_largest=%u psram_free=%u",
             stage ? stage : "unknown",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void snapshot_set(XiaozhiAiState state,
                  const char *status,
                  const char *detail,
                  const char *binding_code = nullptr)
{
    xiaozhi_snapshot_set(state, status, detail, binding_code);
}

void snapshot_set_status_preserving_detail(XiaozhiAiState state, const char *status)
{
    xiaozhi_snapshot_set_status_preserving_detail(state, status);
}

void snapshot_mark_user_activity()
{
    xiaozhi_snapshot_mark_user_activity();
}

void return_from_xiaozhi_to_home()
{
    active_work_page_store(first_enabled_work_page());
    if (s_events) {
        xEventGroupClearBits(s_events, kAiPageActiveBit);
        xEventGroupSetBits(s_events, kAiWakeBit);
    }
    notify_ui_task();
}

bool handle_wake_interrupt(WebsocketSession &session, VoiceCodecRuntime &codec_runtime)
{
    bool abort_sent = websocket_send_wake_abort(&session);
    xiaozhi_tts_playback_stop();
    abort_xiaozhi_speaker_playback();
    (void)esp_opus_dec_reset(codec_runtime.decoder);
    session.server_speaking = false;
    session.resume_listening_pending = false;
    session.discard_tts_audio = true;
    clear_tts_timing_state(session);
    bool listen_sent = websocket_send_listen_start(&session);
    bool wake_feedback_played = play_xiaozhi_wake_feedback();
    bool playback_restarted = xiaozhi_tts_playback_start();
    xiaozhi_voice_set_streaming(true);
    session.user_text_hold_until = 0;
    session.user_text_hold_set = false;
    session.pending_assistant_text[0] = '\0';
    snapshot_set(kXiaozhiAiListening, "已打断", "请继续说话");
    ESP_LOGI(TAG,
             "Xiaozhi wake interrupt: abort=%d listen=%d feedback=%d playback=%d",
             abort_sent,
             listen_sent,
             wake_feedback_played,
             playback_restarted);
    return wake_feedback_played && playback_restarted;
}

bool run_voice_conversation()
{
    // TLS、Opus 与状态刷新都会短时占用内部 DMA 内存。复用现有网络守卫，
    // 在小智会话期间让 RLCD 使用 512 字节分块，避免 SPI 临时缓冲分配失败。
    NetworkDisplayDmaGuard display_guard(true);
    log_voice_resources("before websocket");
    char url[256] = {};
    char token[256] = {};
    int32_t version = 1;
    if (!xiaozhi_load_websocket_config(url, sizeof(url), token, sizeof(token), &version)) {
        return false;
    }
    WebsocketSession session = {};
    if (!open_websocket(&session, url, token, version)) {
        log_voice_resources("websocket failed");
        return false;
    }
    log_voice_resources("websocket connected");
    ScopedHeapBuffer<uint8_t> buffers_storage(
        static_cast<uint8_t *>(heap_caps_calloc(
            1, sizeof(VoiceIoBuffers), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        sizeof(VoiceIoBuffers));
    if (!buffers_storage) {
        close_websocket(&session);
        return false;
    }
    VoiceIoBuffers *buffers = reinterpret_cast<VoiceIoBuffers *>(buffers_storage.data());
    if (!xiaozhi_start_voice_protocol_session(&session,
                                               buffers->incoming,
                                               sizeof(buffers->incoming))) {
        buffers_storage.reset();
        close_websocket(&session);
        return false;
    }
    VoiceCodecRuntime codec_runtime;
    bool ready = codec_runtime.initialize(session.output_sample_rate);
    if (ready) {
        ready = xiaozhi_tts_playback_start();
    }
    ESP_LOGI(TAG,
             "Opus frame buffers: input=%d output=%d psram=%s bytes=%u",
             codec_runtime.encoder_input_size,
             codec_runtime.encoder_output_size,
             codec_runtime.encode_buffers ? "ready" : "unavailable",
             static_cast<unsigned>(sizeof(VoiceEncodeBuffers)));
    log_voice_resources(ready ? "opus ready" : "opus failed");
    if (ready) {
        xiaozhi_voice_set_streaming(true);
        snapshot_set(kXiaozhiAiListening, "正在聆听", "请开始说话");
    }
    TickType_t last_activity = xTaskGetTickCount();
    while (ready && (xEventGroupGetBits(s_events) & kAiPageActiveBit) != 0 &&
           !s_pomodoro_audio_suspended.load(std::memory_order_acquire) &&
           (xTaskGetTickCount() - last_activity) < pdMS_TO_TICKS(kConversationIdleTimeoutMs)) {
        if (xiaozhi_tts_playback_failed()) {
            ESP_LOGW(TAG, "Xiaozhi TTS playback failed; rebuilding voice session");
            ready = false;
            break;
        }
        if (session.empty_reply_continuation_pending &&
            app_tick_deadline_reached(xTaskGetTickCount(),
                                      session.empty_reply_continuation_deadline)) {
            ESP_LOGI(TAG, "Xiaozhi empty reply continuation timeout; returning to wake word");
            break;
        }
        publish_pending_assistant_text(&session);
        bool wake_detected = !session.peer_disconnected && xiaozhi_voice_take_wake_word();
        TickType_t wake_tick = xTaskGetTickCount();
        uint32_t speaking_elapsed_ms = session.tts_started_tick_set
                                           ? static_cast<uint32_t>(
                                                 (static_cast<uint64_t>(wake_tick - session.tts_started_tick) *
                                                  1000U) /
                                                 configTICK_RATE_HZ)
                                           : 0U;
        bool wake_interrupt = wake_detected &&
                              xiaozhi_wake_interrupt_allowed(
                                  session.server_speaking,
                                  session.resume_listening_pending,
                                  session.tts_started_tick_set,
                                  speaking_elapsed_ms);
        if (wake_detected && session.server_speaking && !wake_interrupt) {
            ESP_LOGI(TAG,
                     "Xiaozhi wake ignored during TTS guard: elapsed=%u stop_pending=%d",
                     static_cast<unsigned>(speaking_elapsed_ms),
                     session.resume_listening_pending ? 1 : 0);
        }
        if (wake_interrupt) {
            if (!handle_wake_interrupt(session, codec_runtime)) {
                ready = false;
                break;
            }
        }
        // AFE fills its stream asynchronously. Never block the WebSocket
        // receive path waiting for a 60 ms uplink frame; otherwise TTS packets
        // arrive slower than the speaker consumes them and cause underruns.
        if (!session.peer_disconnected &&
            xiaozhi_voice_processed_bytes_available() >=
            static_cast<size_t>(codec_runtime.encoder_input_size)) {
            if (!xiaozhi_send_encoded_microphone(&session, &codec_runtime)) {
                ready = false;
                break;
            }
        }
        int received = 0;
        if (!session.peer_disconnected) {
            constexpr int kReadTimeoutMs = 10;
            received = esp_transport_read(session.socket,
                                          buffers->incoming,
                                          sizeof(buffers->incoming),
                                          kReadTimeoutMs);
        }
        if (received > 0) {
            last_activity = xTaskGetTickCount();
            if (esp_transport_ws_get_read_opcode(session.socket) == WS_TRANSPORT_OPCODES_BINARY) {
                if (!xiaozhi_decode_incoming_audio(
                        &session,
                        reinterpret_cast<uint8_t *>(buffers->incoming),
                        static_cast<size_t>(received),
                        &codec_runtime,
                        &buffers->audio)) {
                    ready = false;
                    break;
                }
            } else if (esp_transport_ws_get_read_opcode(session.socket) == WS_TRANSPORT_OPCODES_TEXT) {
                if (!xiaozhi_handle_incoming_text_frame(
                        &session,
                        buffers->incoming,
                        sizeof(buffers->incoming),
                        static_cast<size_t>(received))) {
                    ready = false;
                    break;
                }
            }
        } else if (received < 0 && session.exit_after_reply_requested) {
            session.peer_disconnected = true;
            ESP_LOGI(TAG, "Xiaozhi peer closed after farewell; draining local audio");
        } else if (received < 0) {
            ready = false;
            break;
        }
        if (session.exit_after_reply_requested &&
                   session.exit_reply_started &&
                   xiaozhi_tts_playback_drained() &&
                   tts_final_frames_settled(session) &&
                   (session.resume_listening_pending || session.peer_disconnected)) {
            ESP_LOGI(TAG, "Xiaozhi farewell played, returning home");
            return_from_xiaozhi_to_home();
            break;
        } else if (!session.peer_disconnected &&
                   session.resume_listening_pending &&
                   xiaozhi_tts_playback_drained() &&
                   tts_final_frames_settled(session)) {
            if (weather_city_mcp_save_pending()) {
                ESP_LOGI(TAG, "Xiaozhi weather city reply finished; closing voice session for safe refresh");
                snapshot_set(kXiaozhiAiActivating, "天气城市已设置", "正在后台更新全部天气");
                break;
            }
            if (!resume_xiaozhi_microphone_after_playback() ||
                !websocket_send_listen_start(&session)) {
                ready = false;
                break;
            }
            bool empty_reply = xiaozhi_turn_reply_is_empty(
                session.turn_user_text_received,
                session.turn_assistant_text_received,
                session.turn_assistant_audio_received);
            session.resume_listening_pending = false;
            session.server_speaking = false;
            session.discard_tts_audio = false;
            clear_tts_timing_state(session);
            last_activity = xTaskGetTickCount();
            if (empty_reply) {
                session.empty_reply_continuation_pending = true;
                session.empty_reply_continuation_deadline =
                    last_activity + pdMS_TO_TICKS(kXiaozhiEmptyReplyContinuationMs);
                snapshot_set(kXiaozhiAiListening, "没有听完整", "请继续说，或重新说一遍");
                ESP_LOGI(TAG,
                         "Xiaozhi empty reply; continuation window=%u ms",
                         static_cast<unsigned>(kXiaozhiEmptyReplyContinuationMs));
            } else if (user_subtitle_hold_active(&session)) {
                snapshot_set_status_preserving_detail(kXiaozhiAiListening, "正在聆听");
            } else {
                bool had_pending_subtitle = session.pending_assistant_text[0] != '\0';
                publish_pending_assistant_text(&session);
                if (!had_pending_subtitle) {
                    snapshot_set(kXiaozhiAiListening, "正在聆听", "请继续说话");
                }
            }
            session.turn_user_text_received = false;
            session.turn_assistant_text_received = false;
            session.turn_assistant_audio_received = false;
            ESP_LOGI(TAG, "Xiaozhi listening resumed for next turn");
        }
        if (session.peer_disconnected) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (session.exit_after_reply_requested &&
            session.exit_reply_deadline_set &&
            app_tick_deadline_reached(xTaskGetTickCount(), session.exit_reply_deadline) &&
            (!session.exit_reply_started || xiaozhi_tts_playback_drained())) {
            ESP_LOGW(TAG, "Xiaozhi farewell timeout, returning home");
            return_from_xiaozhi_to_home();
            break;
        }
    }
    xiaozhi_tts_playback_stop();
    xiaozhi_voice_set_streaming(false);
    codec_runtime.release();
    buffers_storage.reset();
    close_websocket(&session);
    log_voice_resources("conversation closed");
    return ready;
}

void stop_voice_session()
{
    xiaozhi_voice_stop();
    s_voice_started = false;
}

void release_realtime_network()
{
    const XiaozhiPowerSessionSnapshot power = xiaozhi_power_session_snapshot();
    XiaozhiRuntimeOwnershipSnapshot ownership = {
        s_voice_started,
        power.network_keepalive,
        power.network_lock_held,
        power.idle_low_power,
    };
    bool had_xiaozhi_resources = xiaozhi_runtime_resources_active(ownership);

    // Error paths can clear a high-level state flag before every worker and
    // peripheral has stopped. Both stop functions are idempotent, so page exit
    // always performs the complete cleanup instead of trusting cached flags.
    xiaozhi_tts_playback_stop();
    stop_voice_session();
    xiaozhi_power_session_release();
    if (had_xiaozhi_resources) {
        xiaozhi_log_shutdown_snapshot();
    }
}

void ensure_wake_word_listening()
{
    if (xiaozhi_voice_is_listening()) {
        s_voice_started = true;
        return;
    }
    // A fatal feed-task exit can leave the high-level flag set until the AI
    // coordinator observes the voice event. Reclaim that stopped runtime
    // before rebuilding the listener.
    if (s_voice_started) {
        stop_voice_session();
    }
    if (!xiaozhi_voice_start()) {
        snapshot_set(kXiaozhiAiError, kErrorStatus, kWakeWordFailureDetail);
        return;
    }
    s_voice_started = true;
    snapshot_set(kXiaozhiAiReady, kReadyStatus, kBoundDetail);
    xiaozhi_power_session_set_idle(true);
}

bool wait_for_mcp_weather_refresh()
{
    if (!g_app_events || !s_events) {
        ESP_LOGW(TAG, "Xiaozhi weather refresh wait skipped: event group unavailable");
        return false;
    }
    const TickType_t deadline = xTaskGetTickCount() +
                                pdMS_TO_TICKS(kMcpWeatherRefreshTimeoutMs);
    while ((xEventGroupGetBits(g_app_events) & kManualWeatherSyncBit) != 0 &&
           (xEventGroupGetBits(s_events) & kAiPageActiveBit) != 0) {
        if (app_tick_deadline_reached(xTaskGetTickCount(), deadline)) {
            ESP_LOGW(TAG,
                     "Xiaozhi weather refresh wait timed out after %u ms; continuing in background",
                     static_cast<unsigned>(kMcpWeatherRefreshTimeoutMs));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(kMcpWeatherRefreshPollMs));
    }
    return (xEventGroupGetBits(g_app_events) & kManualWeatherSyncBit) == 0;
}

void xiaozhi_ai_task(void *)
{
    TickType_t next_activation_attempt = 0;
    bool activation_attempt_scheduled = false;
    bool pomodoro_audio_paused = false;
    for (;;) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        bool active = (bits & kAiPageActiveBit) != 0;
        if (!active) {
            release_realtime_network();
            snapshot_set(kXiaozhiAiInactive, kXiaozhiDefaultStatus, "");
            s_task_exited.store(true);
            vTaskSuspend(nullptr);
            return;
        }
        // 番茄钟只借用 Codec：保留页面任务和网络上下文，避免提示结束后
        // 被 UI 当成重新进入小智页面而重建完整服务。
        if (s_pomodoro_audio_suspended.load(std::memory_order_acquire)) {
            if (!pomodoro_audio_paused) {
                xiaozhi_tts_playback_stop();
                stop_voice_session();
                snapshot_set(kXiaozhiAiReady, kReadyStatus, kBoundDetail);
                xiaozhi_power_session_set_idle(true);
                pomodoro_audio_paused = true;
                ESP_LOGI(TAG, "Xiaozhi audio paused for pomodoro completion");
            }
            xEventGroupWaitBits(s_events,
                                kAiWakeBit,
                                pdTRUE,
                                pdFALSE,
                                kPomodoroAudioPausedWaitTicks);
            continue;
        }
        if (pomodoro_audio_paused) {
            pomodoro_audio_paused = false;
            snapshot_set(kXiaozhiAiReady, kReadyStatus, kBoundDetail);
            ESP_LOGI(TAG, "Xiaozhi audio resuming after pomodoro completion");
        }
        if (xiaozhi_ai_configuration_blocked(g_offline_mode_ui_enabled,
                                             network_wifi_credentials_configured())) {
            release_realtime_network();
            if (g_offline_mode_ui_enabled) {
                snapshot_set(kXiaozhiAiError, kErrorStatus, kOfflineDetail);
            } else {
                snapshot_set(kXiaozhiAiWaitingForWifi, kWifiStatus, kNoWifiDetail);
            }
            xEventGroupWaitBits(s_events,
                                kAiWakeBit,
                                pdTRUE,
                                pdFALSE,
                                portMAX_DELAY);
            continue;
        }
        if (!xiaozhi_power_session_acquire_realtime()) {
            // Connection setup has already exhausted its bounded wait. Release
            // the page-owned radio, PM locks and voice resources before the
            // 15-second retry backoff instead of keeping the failed session at
            // realtime power for the whole delay.
            release_realtime_network();
            snapshot_set(kXiaozhiAiWaitingForWifi, kWifiStatus, "连接失败，正在重试");
            xEventGroupWaitBits(s_events, kAiWakeBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(kActivationRetryMs));
            continue;
        }
        TickType_t now = xTaskGetTickCount();
        XiaozhiAiSnapshot current_snapshot = {};
        xiaozhi_snapshot_get(&current_snapshot);
        if (xiaozhi_activation_attempt_due(current_snapshot.state,
                                            activation_attempt_scheduled,
                                            now,
                                            next_activation_attempt)) {
            xiaozhi_activate_or_restore_session();
            next_activation_attempt = now + pdMS_TO_TICKS(kActivationRetryMs);
            activation_attempt_scheduled = true;
            xiaozhi_snapshot_get(&current_snapshot);
        }
        if (current_snapshot.state == kXiaozhiAiReady) {
            ensure_wake_word_listening();
        }
        if (xiaozhi_voice_take_wake_word()) {
            // The audio session stays owned by the existing audio service;
            // protocol I/O cannot create a competing I2S or Wi-Fi stack.
            if (!xiaozhi_power_session_set_idle(false)) {
                stop_voice_session();
                snapshot_set(kXiaozhiAiError, kErrorStatus, "系统繁忙，稍后重试");
                continue;
            }
            // 待唤醒阶段会释放 CPU MAX 锁。恢复实时模式后给 APB/I2S/PA
            // 一个短稳定窗口，再打开扬声器，避免首段提示音偶发失真。
            vTaskDelay(pdMS_TO_TICKS(kWakeAudioPerformanceSettleMs));
            snapshot_mark_user_activity();
            if (!play_xiaozhi_wake_feedback()) {
                ESP_LOGW(TAG, "Xiaozhi wake feedback failed; rebuilding voice session");
                stop_voice_session();
                snapshot_set(kXiaozhiAiError, kErrorStatus, "音频状态异常，正在重试");
                continue;
            }
            snapshot_set(kXiaozhiAiListening, "已唤醒", "正在连接语音会话");
            bool conversation_ok = run_voice_conversation();
            bool weather_city_pending = weather_city_mcp_save_pending();
            if (xiaozhi_mcp_volume_save_pending() ||
                alarm_save_pending() ||
                weather_city_pending) {
                stop_voice_session();
                if (!xiaozhi_mcp_flush_pending_settings()) {
                    ESP_LOGW(TAG, "xiaozhi MCP volume save failed");
                }
                if (!alarm_flush_pending_save()) {
                    ESP_LOGW(TAG, "xiaozhi MCP alarm save failed");
                }
                bool weather_city_saved = weather_city_mcp_flush_pending_save();
                if (!weather_city_saved) {
                    ESP_LOGW(TAG, "xiaozhi MCP weather city save failed");
                } else if (weather_city_pending) {
                    // Full weather refresh includes current weather, warning,
                    // forecast and air quality. Run it only after WebSocket,
                    // Opus, AEC and Codec resources have been released.
                    snapshot_set(kXiaozhiAiActivating,
                                 "天气城市已保存",
                                 "正在后台更新全部天气");
                    release_realtime_network();
                    (void)wait_for_mcp_weather_refresh();
                }
            }
            if (!conversation_ok) {
                stop_voice_session();
                snapshot_set(kXiaozhiAiError, kErrorStatus, "语音会话中断，稍后重试");
            } else if ((xEventGroupGetBits(s_events) & kAiPageActiveBit) != 0 &&
                       !s_pomodoro_audio_suspended.load(std::memory_order_acquire)) {
                snapshot_set(kXiaozhiAiReady, kReadyStatus, kBoundDetail);
                xiaozhi_power_session_set_idle(true);
            }
        }
        xiaozhi_snapshot_get(&current_snapshot);
        TickType_t wait_now = xTaskGetTickCount();
        TickType_t idle_wait = portMAX_DELAY;
        if (!xiaozhi_ai_idle_wait_until_event(current_snapshot.state,
                                              xiaozhi_voice_is_listening())) {
            idle_wait = xiaozhi_activation_retry_wait_ticks(
                current_snapshot.state,
                activation_attempt_scheduled,
                wait_now,
                next_activation_attempt,
                pdMS_TO_TICKS(kLoopIdleMs));
        }
        xEventGroupWaitBits(s_events, kAiWakeBit, pdTRUE, pdFALSE, idle_wait);
    }
}
} // namespace

void xiaozhi_ai_init()
{
    if (s_events) {
        return;
    }
    s_events = xEventGroupCreate();
    bool snapshot_ready = xiaozhi_snapshot_state_init();
    if (!s_events || !snapshot_ready) {
        ESP_LOGW(TAG, "%s", XIAOZHI_STATE_INIT_FAILED_LOG);
        xiaozhi_snapshot_state_deinit();
        if (s_events) {
            vEventGroupDelete(s_events);
            s_events = nullptr;
        }
        return;
    }
    xiaozhi_voice_set_event_callback(notify_ai_voice_event);
}

void xiaozhi_ai_set_page_active(bool active)
{
    if (!s_events) {
        return;
    }
    active = active && !s_alarm_suspended.load();
    reclaim_ai_task_if_exited();
    const bool already_active = (xEventGroupGetBits(s_events) & kAiPageActiveBit) != 0;
    // ui_task evaluates the visible-page state every loop.  Do not turn that
    // polling into an event storm: while inactive, repeated wake events kept
    // this task runnable on CPU1 and could starve the UI idle task.
    if (!active) {
        s_task_start_retry.reset();
        if (!already_active) {
            return;
        }
        xEventGroupClearBits(s_events, kAiPageActiveBit);
        xEventGroupSetBits(s_events, kAiWakeBit);
        return;
    }
    if (!already_active) {
        xEventGroupSetBits(s_events, kAiPageActiveBit | kAiWakeBit);
    }
    const TickType_t task_start_now = xTaskGetTickCount();
    if (s_task_handle ||
        xiaozhi_power_session_task_start_blocked() ||
        !s_task_start_retry.attempt_due(task_start_now,
                                        pdMS_TO_TICKS(kTaskStartRetryMs))) {
        return;
    }
    ESP_LOGI(TAG,
             "Xiaozhi task heap: internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    s_task_exited.store(false);
    s_task_handle = xTaskCreateStaticPinnedToCore(xiaozhi_ai_task,
                                                  "xiaozhi_ai",
                                                  kXiaozhiTaskStackSize,
                                                  nullptr,
                                                  4,
                                                  s_task_stack,
                                                  &s_task_buffer,
                                                  tskNO_AFFINITY);
    if (!s_task_handle) {
        ESP_LOGW(TAG, "Xiaozhi AI task creation failed");
        s_task_start_retry.record_failure(task_start_now);
        s_task_exited.store(true);
        s_task_handle = nullptr;
        xEventGroupClearBits(s_events, kAiWakeBit);
        snapshot_set(kXiaozhiAiError, kErrorStatus, "小智任务启动失败");
        return;
    }
    s_task_start_retry.reset();
    ESP_LOGI(TAG,
             "Xiaozhi AI task ready: stack_internal_static=%u tcb_internal=%u",
             static_cast<unsigned>(kXiaozhiTaskStackSize),
             static_cast<unsigned>(sizeof(StaticTask_t)));
}

void xiaozhi_ai_notify_network_configuration_changed()
{
    if (s_events) {
        xEventGroupSetBits(s_events, kAiWakeBit);
    }
}

bool xiaozhi_ai_page_active()
{
    return s_events && (xEventGroupGetBits(s_events) & kAiPageActiveBit) != 0;
}

bool xiaozhi_ai_network_keepalive_active()
{
    return xiaozhi_power_session_keepalive_active();
}

void xiaozhi_ai_set_alarm_suspended(bool suspended)
{
    if (s_alarm_suspended.exchange(suspended) == suspended) {
        return;
    }
    if (suspended && s_events) {
        xEventGroupClearBits(s_events, kAiPageActiveBit);
        xEventGroupSetBits(s_events, kAiWakeBit);
    }
    // 解除后由 UI 可见页判断恢复，避免闹钟线程替页面管理器决定是否重启小智。
}

void xiaozhi_ai_set_pomodoro_audio_suspended(bool suspended)
{
    if (s_pomodoro_audio_suspended.exchange(suspended, std::memory_order_acq_rel) == suspended) {
        return;
    }
    if (s_events) {
        xEventGroupSetBits(s_events, kAiWakeBit);
    }
}

void xiaozhi_ai_get_snapshot(XiaozhiAiSnapshot *out)
{
    xiaozhi_snapshot_get(out);
}

void xiaozhi_ai_clear_activation()
{
    release_realtime_network();
    (void)xiaozhi_clear_activation_storage();
}
