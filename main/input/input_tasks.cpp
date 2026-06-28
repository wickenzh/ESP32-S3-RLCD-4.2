// 处理 BOOT 和 KEY 按键输入、页面切换和设置页操作请求。
#include "input_tasks.h"

#include "audio_services.h"
#include "ota_services.h"
#include "ui_views.h"

namespace {
bool low_refresh_button_idle_context()
{
    if (g_battery_charging ||
        g_setup_portal_active ||
        g_settings_requested ||
        g_boot_info_requested ||
        g_network_diag_page_requested ||
        ota_flow_active() ||
        is_audio_playing() ||
        g_wifi_radio_on) {
        return false;
    }
    if (g_low_battery_mode) {
        return true;
    }
    return g_active_work_page == kWorkPageHistory ||
           g_active_work_page == kWorkPageGallery ||
           g_active_work_page == kWorkPageCalendar ||
           g_active_work_page == kWorkPageWeatherBoard;
}
} // namespace

void button_task(void *)
{
    gpio_config_t button = {};
    button.intr_type = GPIO_INTR_DISABLE;
    button.mode = GPIO_MODE_INPUT;
    button.pin_bit_mask = (1ULL << kBootButtonGpio) | (1ULL << kKeyButtonGpio);
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t err = gpio_config(&button);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button gpio config failed: %s", esp_err_to_name(err));
        return;
    }

    TickType_t boot_pressed_since = 0;
    TickType_t key_pressed_since = 0;
    bool key_press_opened_settings = false;
    bool key_long_handled = false;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool boot_pressed = gpio_get_level(kBootButtonGpio) == 0;
        bool key_pressed = gpio_get_level(kKeyButtonGpio) == 0;

        if (boot_pressed) {
            if (boot_pressed_since == 0) {
                boot_pressed_since = now;
                if (g_settings_requested) {
                    g_settings_last_activity_tick = now;
                }
            }
        } else {
            if (boot_pressed_since != 0 && g_settings_requested) {
                TickType_t held = now - boot_pressed_since;
                if (held >= pdMS_TO_TICKS(40) && held < pdMS_TO_TICKS(1200)) {
                    g_settings_action_seq = g_settings_action_seq + 1;
                    notify_ui_task();
                }
                g_settings_last_activity_tick = now;
            } else if (boot_pressed_since != 0 &&
                       !g_boot_info_requested &&
                       !g_network_diag_page_requested &&
                       !g_setup_portal_active &&
                       !g_low_battery_mode) {
                TickType_t held = now - boot_pressed_since;
                if (held >= pdMS_TO_TICKS(40) && held < pdMS_TO_TICKS(1200)) {
                    g_active_work_page = next_enabled_work_page(g_active_work_page);
                    ESP_LOGI(TAG, "switch work page: %d", g_active_work_page + 1);
                    notify_ui_task();
                }
            }
            boot_pressed_since = 0;
        }

        if (key_pressed) {
            if (key_pressed_since == 0) {
                key_pressed_since = now;
                key_press_opened_settings = false;
                key_long_handled = false;
                if (g_settings_requested) {
                    g_settings_last_activity_tick = now;
                }
                if (!g_settings_requested && !g_boot_info_requested && !g_network_diag_page_requested) {
                    ESP_LOGI(TAG, "key button clicked, showing settings page");
                    g_boot_info_requested = false;
                    g_settings_requested = true;
                    g_settings_focus_secondary = false;
                    g_settings_page_order_mode = false;
                    g_settings_primary_selection = kSettingsPrimaryNetwork;
                    g_settings_selection = 0;
                    g_settings_page_order_selection = 0;
                    g_settings_last_activity_tick = now;
                    key_press_opened_settings = true;
                    notify_ui_task();
                }
            } else if (!key_press_opened_settings &&
                       !key_long_handled &&
                       g_settings_requested &&
                       now - key_pressed_since >= pdMS_TO_TICKS(1200)) {
                g_settings_last_activity_tick = now;
                if (!is_settings_sync_busy() && !ota_flow_active()) {
                    handle_settings_key_long();
                } else {
                    set_settings_feedback("请等待操作完成", 2000);
                }
                key_long_handled = true;
                notify_ui_task();
            } else if (!key_long_handled &&
                       g_boot_info_requested &&
                       !g_settings_requested &&
                       now - key_pressed_since >= pdMS_TO_TICKS(1200)) {
                g_boot_info_requested = false;
                g_info_page_until_tick = 0;
                g_settings_requested = true;
                g_settings_focus_secondary = true;
                g_settings_page_order_mode = false;
                g_settings_primary_selection = kSettingsPrimarySystem;
                g_settings_selection = 3;
                g_settings_page_order_selection = 0;
                g_settings_last_activity_tick = now;
                key_long_handled = true;
                notify_ui_task();
            } else if (!key_long_handled &&
                       g_network_diag_page_requested &&
                       !g_settings_requested &&
                       now - key_pressed_since >= pdMS_TO_TICKS(1200)) {
                g_network_diag_page_requested = false;
                g_settings_requested = true;
                g_settings_focus_secondary = true;
                g_settings_page_order_mode = false;
                g_settings_primary_selection = kSettingsPrimarySystem;
                g_settings_selection = 0;
                g_settings_page_order_selection = 0;
                g_settings_last_activity_tick = now;
                key_long_handled = true;
                notify_ui_task();
            }
        } else {
            if (key_pressed_since != 0 && !key_press_opened_settings && !key_long_handled && g_settings_requested) {
                TickType_t held = now - key_pressed_since;
                if (held >= pdMS_TO_TICKS(1200)) {
                    g_settings_last_activity_tick = now;
                    if (!is_settings_sync_busy() && !ota_flow_active()) {
                        handle_settings_key_long();
                    } else {
                        set_settings_feedback("请等待操作完成", 2000);
                    }
                    notify_ui_task();
                } else if (held >= pdMS_TO_TICKS(40)) {
                    g_settings_last_activity_tick = now;
                    if (!is_settings_sync_busy() && !ota_flow_active()) {
                        handle_settings_key_short();
                    } else {
                        set_settings_feedback("请等待操作完成", 2000);
                        notify_ui_task();
                    }
                }
            }
            if (key_pressed_since != 0 && g_settings_requested) {
                g_settings_last_activity_tick = now;
            }
            key_pressed_since = 0;
            key_press_opened_settings = false;
            key_long_handled = false;
        }
        int delay_ms = low_refresh_button_idle_context() ? kButtonLowRefreshIdlePollMs : kButtonIdlePollMs;
        if (boot_pressed || key_pressed) {
            delay_ms = kButtonPressedPollMs;
        } else if (g_settings_requested || g_boot_info_requested || g_network_diag_page_requested || g_setup_portal_active) {
            delay_ms = kButtonActivePollMs;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
