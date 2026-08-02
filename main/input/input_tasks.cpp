// 处理 BOOT 和 KEY 按键输入、页面切换和设置页操作请求。
#include "input_tasks.h"

#include "alarm_services.h"
#include "audio_services.h"
#include "input_button_wait_policy.h"
#include "music_player.h"
#include "network_diagnostics_state.h"
#include "ota_services.h"
#include "pomodoro_services.h"
#include "radio_services.h"
#include "task_notification_target.h"
#include "xiaozhi_voice.h"
#include "ui_info_page_state.h"
#include "ui_settings_activity_state.h"
#include "ui_settings_navigation.h"
#include "ui_views.h"
#include "wifi_portal_state.h"
#include "wifi_radio_state.h"

#include "esp_sleep.h"

#define BUTTON_GPIO_CONFIG_FAILED_LOG_FORMAT "button gpio config failed: %s"
#define BUTTON_ISR_SERVICE_FAILED_LOG_FORMAT "button gpio isr service failed: %s; using polling fallback"
#define BUTTON_ISR_HANDLER_FAILED_LOG_FORMAT "button gpio %d isr handler failed: %s; using polling fallback"
#define BUTTON_WAKEUP_FAILED_LOG_FORMAT "button light sleep wakeup failed: %s; using polling fallback"
#define BUTTON_EDGE_WAKEUP_READY_LOG_FORMAT "button edge wakeup ready for low-refresh idle"
#define BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT "switch work page: %d"
#define BUTTON_SHOW_SETTINGS_LOG_FORMAT "key button clicked, showing settings page"

namespace {
constexpr int kButtonDebounceMs = 18;
constexpr int kButtonLongPressMs = 1200;
constexpr int kButtonBusyFeedbackMs = 2000;
constexpr uint64_t kBootButtonPinMask = 1ULL << kBootButtonGpio;
constexpr uint64_t kKeyButtonPinMask = 1ULL << kKeyButtonGpio;
constexpr uint64_t kButtonInputPinMask = kBootButtonPinMask | kKeyButtonPinMask;
constexpr TickType_t kButtonDebounceTicks = pdMS_TO_TICKS(kButtonDebounceMs);
constexpr TickType_t kButtonLongPressTicks = pdMS_TO_TICKS(kButtonLongPressMs);
constexpr const char *kSettingsBusyFeedbackText = "请等待操作完成";
TaskNotificationTarget s_button_task_target;

static_assert(kButtonLongPressMs > kButtonDebounceMs,
              "button long-press duration must be longer than debounce duration");
static_assert(kBootButtonPinMask != 0, "BOOT button pin mask must not be empty");
static_assert(kKeyButtonPinMask != 0, "KEY button pin mask must not be empty");
static_assert(kButtonInputPinMask == (kBootButtonPinMask | kKeyButtonPinMask),
              "button input pin mask must include BOOT and KEY");
static_assert(kButtonLongPressTicks > kButtonDebounceTicks,
              "button long-press tick duration must be longer than debounce duration");
static_assert(kButtonLowRefreshIdlePollMs <= kButtonIdlePollMs,
              "low-refresh button polling must stay at least as responsive as idle polling");
static_assert(kButtonActivePollMs <= kButtonIdlePollMs,
              "active button polling must stay at least as responsive as idle polling");
static_assert(kButtonPressedPollMs <= kButtonActivePollMs,
              "pressed button polling must stay at least as responsive as active polling");

bool button_press_is_short(TickType_t held)
{
    return held >= kButtonDebounceTicks &&
           held < kButtonLongPressTicks;
}

bool button_press_is_long(TickType_t held)
{
    return held >= kButtonLongPressTicks;
}

bool low_refresh_button_idle_context()
{
    if (battery_charging_load() ||
        setup_portal_active_load() ||
        settings_page_requested() ||
        info_page_requested() ||
        network_diag_page_requested() ||
        ota_flow_active() ||
        is_audio_playing() ||
        wifi_radio_on_load()) {
        return false;
    }
    if (battery_low_mode_load()) {
        return true;
    }
    return work_page_uses_low_refresh_idle(active_work_page_load());
}

void return_to_system_settings_item(int selection, TickType_t now)
{
    settings_page_request();
    enter_settings_system_item_navigation(selection);
    settings_activity_record(now);
}

void enter_settings_primary_menu(TickType_t now)
{
    info_page_clear();
    settings_page_request();
    enter_settings_primary_navigation();
    settings_activity_record(now);
}

void handle_settings_key_long_or_busy()
{
    if (!is_settings_sync_busy() && !ota_flow_active()) {
        handle_settings_key_long();
        return;
    }
    set_settings_feedback(kSettingsBusyFeedbackText, kButtonBusyFeedbackMs);
}

void IRAM_ATTR notify_button_edge(void *)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (!s_button_task_target.notify_from_isr(&higher_priority_task_woken)) {
        return;
    }
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void disable_button_interrupts()
{
    (void)gpio_set_intr_type(kBootButtonGpio, GPIO_INTR_DISABLE);
    (void)gpio_set_intr_type(kKeyButtonGpio, GPIO_INTR_DISABLE);
}

void remove_button_isr_handlers(bool boot_registered, bool key_registered)
{
    if (boot_registered) {
        (void)gpio_isr_handler_remove(kBootButtonGpio);
    }
    if (key_registered) {
        (void)gpio_isr_handler_remove(kKeyButtonGpio);
    }
    disable_button_interrupts();
}

bool setup_button_edge_wakeup()
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, BUTTON_ISR_SERVICE_FAILED_LOG_FORMAT, esp_err_to_name(err));
        disable_button_interrupts();
        return false;
    }

    bool boot_registered = false;
    bool key_registered = false;
    err = gpio_isr_handler_add(kBootButtonGpio, notify_button_edge, nullptr);
    if (err == ESP_OK) {
        boot_registered = true;
    } else {
        ESP_LOGW(TAG,
                 BUTTON_ISR_HANDLER_FAILED_LOG_FORMAT,
                 static_cast<int>(kBootButtonGpio),
                 esp_err_to_name(err));
        remove_button_isr_handlers(boot_registered, key_registered);
        return false;
    }

    err = gpio_isr_handler_add(kKeyButtonGpio, notify_button_edge, nullptr);
    if (err == ESP_OK) {
        key_registered = true;
    } else {
        ESP_LOGW(TAG,
                 BUTTON_ISR_HANDLER_FAILED_LOG_FORMAT,
                 static_cast<int>(kKeyButtonGpio),
                 esp_err_to_name(err));
        remove_button_isr_handlers(boot_registered, key_registered);
        return false;
    }

    err = esp_sleep_enable_ext1_wakeup_io(kButtonInputPinMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, BUTTON_WAKEUP_FAILED_LOG_FORMAT, esp_err_to_name(err));
        remove_button_isr_handlers(boot_registered, key_registered);
        return false;
    }

    ESP_LOGI(TAG, BUTTON_EDGE_WAKEUP_READY_LOG_FORMAT);
    return true;
}
} // namespace

void button_task(void *)
{
    gpio_config_t button = {};
    button.intr_type = GPIO_INTR_ANYEDGE;
    button.mode = GPIO_MODE_INPUT;
    button.pin_bit_mask = kButtonInputPinMask;
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t err = gpio_config(&button);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, BUTTON_GPIO_CONFIG_FAILED_LOG_FORMAT, esp_err_to_name(err));
        return;
    }

    s_button_task_target.publish(xTaskGetCurrentTaskHandle());
    const bool edge_wakeup_ready = setup_button_edge_wakeup();

    TickType_t boot_pressed_since = 0;
    TickType_t key_pressed_since = 0;
    bool key_press_opened_settings = false;
    bool key_long_handled = false;
    bool boot_press_stopped_alert = false;
    bool key_press_stopped_alert = false;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool boot_pressed = gpio_get_level(kBootButtonGpio) == 0;
        bool key_pressed = gpio_get_level(kKeyButtonGpio) == 0;

        if (boot_pressed) {
            if (boot_pressed_since == 0) {
                boot_pressed_since = now;
                boot_press_stopped_alert = alarm_stop_ringing_from_button() ||
                                           pomodoro_stop_alert_from_button();
                if (settings_page_requested()) {
                    settings_activity_record(now);
                }
            }
        } else {
            if (boot_pressed_since != 0 && boot_press_stopped_alert) {
                // 提醒音播放期间任意按键只负责停止音频，不继续执行原按键动作。
            } else if (boot_pressed_since != 0 && settings_page_requested()) {
                TickType_t held = now - boot_pressed_since;
                if (button_press_is_short(held)) {
                    settings_activity_record_action(now);
                    notify_ui_task();
                } else {
                    settings_activity_record(now);
                }
            } else if (boot_pressed_since != 0 &&
                       !info_page_requested() &&
                       !network_diag_page_requested() &&
                       !setup_portal_active_load() &&
                       !battery_low_mode_load()) {
                TickType_t held = now - boot_pressed_since;
                if (button_press_is_short(held)) {
                    // 音乐模式下BOOT键先退出音乐模式
                    if (active_work_page_load() == kWorkPageGallery &&
                        g_gallery_music_mode.load()) {
                        gallery_set_music_mode(false);
                    } else {
                        int next_page = next_enabled_work_page(active_work_page_load());
                        active_work_page_store(next_page);
                        ESP_LOGI(TAG, BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT, next_page + 1);
                    }
                    notify_ui_task();
                }
            }
            boot_pressed_since = 0;
            boot_press_stopped_alert = false;
        }

        if (key_pressed) {
            if (key_pressed_since == 0) {
                key_pressed_since = now;
                key_press_opened_settings = false;
                key_long_handled = false;
                key_press_stopped_alert = alarm_stop_ringing_from_button() ||
                                          pomodoro_stop_alert_from_button();
                if (settings_page_requested()) {
                    settings_activity_record(now);
                }
                if (!key_press_stopped_alert &&
                    !settings_page_requested() && !info_page_requested() && !network_diag_page_requested()) {
                    if (active_work_page_load() == kWorkPageRadio) {
                        radio_next_station();
                        key_press_opened_settings = false;
                    } else if (active_work_page_load() == kWorkPageXiaozhiAI) {
                        xiaozhi_voice_trigger_wake();
                        key_press_opened_settings = false;
                    } else if (active_work_page_load() == kWorkPageGallery) {
                        // Gallery页面：默认模式KEY键进入音乐模式，音乐模式KEY键切下一首
                        if (!g_gallery_music_mode.load()) {
                            gallery_set_music_mode(true);
                        } else {
                            music_next_song();
                        }
                        key_press_opened_settings = false;
                    } else {
                        ESP_LOGI(TAG, BUTTON_SHOW_SETTINGS_LOG_FORMAT);
                        enter_settings_primary_menu(now);
                        key_press_opened_settings = true;
                    }
                    notify_ui_task();
                }
            } else if (!key_press_stopped_alert &&
                       !key_press_opened_settings &&
                       !key_long_handled &&
                       settings_page_requested() &&
                       button_press_is_long(now - key_pressed_since)) {
                settings_activity_record(now);
                handle_settings_key_long_or_busy();
                key_long_handled = true;
                notify_ui_task();
            } else if (!key_long_handled &&
                       info_page_requested() &&
                       !settings_page_requested() &&
                       button_press_is_long(now - key_pressed_since)) {
                info_page_clear();
                return_to_system_settings_item(kSystemSettingsInfoItem, now);
                key_long_handled = true;
                notify_ui_task();
            } else if (!key_long_handled &&
                       network_diag_page_requested() &&
                       !settings_page_requested() &&
                       button_press_is_long(now - key_pressed_since)) {
                network_diag_page_clear();
                return_to_system_settings_item(kSystemSettingsNetworkDiagItem, now);
                key_long_handled = true;
                notify_ui_task();
            }
        } else {
            if (key_pressed_since != 0 &&
                !key_press_stopped_alert &&
                !key_press_opened_settings && !key_long_handled && settings_page_requested()) {
                TickType_t held = now - key_pressed_since;
                if (button_press_is_long(held)) {
                    settings_activity_record(now);
                    handle_settings_key_long_or_busy();
                    notify_ui_task();
                } else if (button_press_is_short(held)) {
                    settings_activity_record(now);
                    if (!is_settings_sync_busy() && !ota_flow_active()) {
                        handle_settings_key_short();
                    } else {
                        set_settings_feedback(kSettingsBusyFeedbackText, kButtonBusyFeedbackMs);
                        notify_ui_task();
                    }
                }
            }
            if (key_pressed_since != 0 && settings_page_requested()) {
                settings_activity_record(now);
            }
            key_pressed_since = 0;
            key_press_opened_settings = false;
            key_long_handled = false;
            key_press_stopped_alert = false;
        }
        const bool low_refresh_idle = low_refresh_button_idle_context();
        const bool press_tracking_active = boot_pressed_since != 0 || key_pressed_since != 0;
        if (button_task_can_wait_for_edge(edge_wakeup_ready,
                                          low_refresh_idle,
                                          boot_pressed,
                                          key_pressed,
                                          press_tracking_active)) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        int delay_ms = low_refresh_idle ? kButtonLowRefreshIdlePollMs : kButtonIdlePollMs;
        if (boot_pressed || key_pressed) {
            delay_ms = kButtonPressedPollMs;
        } else if (settings_page_requested() || info_page_requested() || network_diag_page_requested() || setup_portal_active_load()) {
            delay_ms = kButtonActivePollMs;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
