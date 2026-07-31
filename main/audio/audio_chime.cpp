// 编排整点提醒、设置试听和配网提示音任务。
#include "audio_services.h"

#include "audio_chime_policy.h"
#include "audio_services_internal.h"
#include "ota_runtime_state.h"
#include "single_pending_task_gate.h"
#include "startup_state.h"
#include "wifi_portal_state.h"
#include "wifi_radio_state.h"

#include <cstdint>

#define AUDIO_TASK_FUNCTION_UNAVAILABLE_LOG_FORMAT "failed to create %s task: task function unavailable"
#define AUDIO_TASK_CREATE_FAILED_LOG_FORMAT "failed to create %s task rtos=%s stack=%u priority=%u core=%d rc=%d"
#define HOURLY_CHIME_PLAYED_LOG_FORMAT "hourly chime played sound=%d volume=%d"
#define HOURLY_CHIME_SKIPPED_LOG_FORMAT "hourly chime skipped sound=%d"

namespace {
std::atomic<bool> s_setup_prompt_pending{false};
SinglePendingTaskGate s_settings_chime_retry_gate;
constexpr uint32_t kAudioPlaybackTaskStack = 6144;
constexpr uint32_t kSettingsChimeRetryTaskStack = 3072;
constexpr UBaseType_t kAudioPlaybackTaskPriority = 4;
constexpr UBaseType_t kSettingsChimeRetryTaskPriority = 3;
constexpr BaseType_t kAudioTaskCore = 1;
constexpr int kSettingsChimeRetryAttempts = 8;
constexpr uint32_t kSettingsChimeRetryDelayMs = 180;
constexpr uint32_t kSetupPromptChainDelayMs = 120;
constexpr TickType_t kSettingsChimeRetryDelay = pdMS_TO_TICKS(kSettingsChimeRetryDelayMs);
constexpr TickType_t kSetupPromptChainDelay = pdMS_TO_TICKS(kSetupPromptChainDelayMs);
constexpr const char *kDefaultAudioTaskName = "audio_play";
constexpr const char *kHourlyChimeTaskName = "hourly_chime";
constexpr const char *kSetupPromptTaskName = "setup_prompt";
constexpr const char *kSettingsChimeRetryTaskName = "settings_chime";
constexpr const char *kDefaultAudioLogName = "audio playback";
constexpr const char *kHourlyChimeLogName = "hourly chime";
constexpr const char *kSetupPromptLogName = "setup prompt";
constexpr const char *kSettingsChimeBusyLog = "settings confirmation chime skipped: audio busy";
constexpr const char *kSetupPromptPlayedLog = "setup prompt played";
constexpr const char *kSetupPromptSkippedLog = "setup prompt skipped";
constexpr const char *kSetupPromptPendingLog = "setup prompt pending";
constexpr const char *kSettingsChimeRetryTaskCreateFailedLog = "failed to create settings chime retry task";
constexpr const char *kHourlyChimeRadioSetupSkippedLog = "hourly chime skipped while radio or setup is active";

static_assert(kAudioPlaybackTaskPriority > tskIDLE_PRIORITY,
              "audio playback task priority must exceed idle");
static_assert(kSettingsChimeRetryTaskPriority > tskIDLE_PRIORITY,
              "settings chime retry priority must exceed idle");
static_assert(kAudioTaskCore >= 0 && kAudioTaskCore < portNUM_PROCESSORS,
              "audio task core must exist on the target");
static_assert(kSettingsChimeRetryAttempts > 0,
              "settings chime retry attempts must be positive");
static_assert(kSettingsChimeRetryDelay > 0,
              "settings chime retry delay must be positive");
static_assert(kSetupPromptChainDelay > 0,
              "setup prompt chain delay must be positive");

const char *audio_text_or_default(const char *text, const char *fallback)
{
    return text ? text : fallback;
}

void settings_confirmation_chime_task(void *);

bool create_audio_task(TaskFunction_t task_fn,
                       const char *task_name,
                       uint32_t task_stack,
                       UBaseType_t task_priority,
                       void *task_arg,
                       const char *log_name)
{
    const char *display_name = audio_text_or_default(log_name, kDefaultAudioLogName);
    const char *rtos_name = audio_text_or_default(task_name, kDefaultAudioTaskName);
    if (!task_fn) {
        ESP_LOGW(TAG, AUDIO_TASK_FUNCTION_UNAVAILABLE_LOG_FORMAT, display_name);
        return false;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(task_fn,
                                            rtos_name,
                                            task_stack,
                                            task_arg,
                                            task_priority,
                                            nullptr,
                                            kAudioTaskCore);
    if (ok != pdPASS) {
        ESP_LOGW(TAG,
                 AUDIO_TASK_CREATE_FAILED_LOG_FORMAT,
                 display_name,
                 rtos_name,
                 (unsigned)task_stack,
                 (unsigned)task_priority,
                 (int)kAudioTaskCore,
                 (int)ok);
        return false;
    }
    return true;
}

void create_settings_chime_retry_task()
{
    if (!s_settings_chime_retry_gate.try_acquire()) {
        return;
    }
    if (!create_audio_task(settings_confirmation_chime_task,
                           kSettingsChimeRetryTaskName,
                           kSettingsChimeRetryTaskStack,
                           kSettingsChimeRetryTaskPriority,
                           nullptr,
                           kSettingsChimeRetryTaskCreateFailedLog)) {
        s_settings_chime_retry_gate.release();
    }
}

void run_hourly_chime(int sound_index)
{
    const int volume_percent = g_chime_volume_percent.load(std::memory_order_acquire);
    CodecPort *codec = audio_prepare_codec_for_playback();
    if (codec && codec->CodecPort_PlayChimeSound(sound_index, volume_percent)) {
        ESP_LOGI(TAG, HOURLY_CHIME_PLAYED_LOG_FORMAT, sound_index, volume_percent);
    } else {
        ESP_LOGW(TAG, HOURLY_CHIME_SKIPPED_LOG_FORMAT, sound_index);
    }
    audio_finish_playback();
    if (s_setup_prompt_pending.load(std::memory_order_acquire) &&
        !startup_screen_active()) {
        vTaskDelay(kSetupPromptChainDelay);
        (void)start_setup_prompt_playback();
    }
}

void run_setup_prompt()
{
    CodecPort *codec = audio_prepare_codec_for_playback();
    if (codec && codec->CodecPort_PlayWifiPrompt()) {
        ESP_LOGI(TAG, "%s", kSetupPromptPlayedLog);
    } else {
        ESP_LOGW(TAG, "%s", kSetupPromptSkippedLog);
    }
    audio_finish_playback();
}
} // namespace

void hourly_chime_task(void *arg)
{
    const int sound_index = (int)(intptr_t)arg;
    run_hourly_chime(sound_index);
    vTaskDelete(nullptr);
}

bool play_chime_sound_blocking(int source_slot, bool (*stop_requested)())
{
    return play_chime_sound_repeated_blocking(source_slot, 1, stop_requested);
}

bool play_chime_sound_repeated_blocking(int source_slot,
                                        int repeat_count,
                                        bool (*stop_requested)())
{
    if (repeat_count <= 0 || !audio_try_mark_playing()) {
        return false;
    }
    CodecPort *codec = audio_prepare_codec_for_playback();
    const int volume_percent = g_chime_volume_percent.load(std::memory_order_acquire);
    bool played = codec != nullptr;
    for (int repeat = 0; played && repeat < repeat_count; ++repeat) {
        if (stop_requested && stop_requested()) {
            played = false;
            break;
        }
        played = codec->CodecPort_PlayChimeSound(source_slot,
                                                 volume_percent,
                                                 stop_requested);
    }
    audio_finish_playback();
    return played;
}

void setup_prompt_task(void *)
{
    run_setup_prompt();
    vTaskDelete(nullptr);
}

namespace {
void run_settings_confirmation_chime()
{
    for (int attempt = 0; attempt < kSettingsChimeRetryAttempts; ++attempt) {
        if (start_chime_playback(g_chime_sound_index)) {
            return;
        }
        vTaskDelay(kSettingsChimeRetryDelay);
    }
    ESP_LOGW(TAG, "%s", kSettingsChimeBusyLog);
}

void settings_confirmation_chime_task(void *)
{
    run_settings_confirmation_chime();
    s_settings_chime_retry_gate.release();
    vTaskDelete(nullptr);
}
} // namespace

bool start_chime_playback(int source_slot)
{
    if (!audio_try_mark_playing()) {
        return false;
    }
    if (!create_audio_task(hourly_chime_task,
                           kHourlyChimeTaskName,
                           kAudioPlaybackTaskStack,
                           kAudioPlaybackTaskPriority,
                           (void *)(intptr_t)source_slot,
                           kHourlyChimeLogName)) {
        audio_clear_playing();
        return false;
    }
    return true;
}

bool start_setup_prompt_playback()
{
    if (!audio_try_mark_playing()) {
        return false;
    }
    s_setup_prompt_pending.store(false, std::memory_order_release);
    if (!create_audio_task(setup_prompt_task,
                           kSetupPromptTaskName,
                           kAudioPlaybackTaskStack,
                           kAudioPlaybackTaskPriority,
                           nullptr,
                           kSetupPromptLogName)) {
        audio_clear_playing();
        s_setup_prompt_pending.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

bool setup_prompt_playback_pending()
{
    return s_setup_prompt_pending.load(std::memory_order_acquire);
}

void request_setup_prompt_once()
{
    if (startup_screen_active() ||
        is_audio_playing()) {
        s_setup_prompt_pending.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "%s", kSetupPromptPendingLog);
        return;
    }
    if (!start_setup_prompt_playback()) {
        s_setup_prompt_pending.store(true, std::memory_order_release);
    }
}

void request_settings_confirmation_chime()
{
    if (audio_playback_blocked_by_system(battery_low_mode_load(),
                                         ota_runtime_state_load() == kOtaUpdating)) {
        return;
    }
    if (start_chime_playback(g_chime_sound_index)) {
        return;
    }
    create_settings_chime_retry_task();
}

void play_hourly_chime(int hour, bool enforce_quiet_hours)
{
    int ota_state = ota_runtime_state_load();
    const AudioChimeDecision decision = audio_hourly_chime_decision({
        battery_low_mode_load(),
        ota_state == kOtaUpdating,
        wifi_radio_on_load(),
        setup_portal_active_load(),
        ota_state == kOtaChecking,
        enforce_quiet_hours,
        g_hourly_chime_all_day,
        hour,
    });
    if (decision == kAudioChimeBlockedByNetwork) {
        ESP_LOGI(TAG, "%s", kHourlyChimeRadioSetupSkippedLog);
        return;
    }
    if (decision != kAudioChimePlay) {
        return;
    }
    (void)start_chime_playback(g_chime_sound_index);
}
