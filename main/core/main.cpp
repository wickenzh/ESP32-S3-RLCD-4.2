// 初始化硬件、系统服务和常驻任务，是固件应用入口。
#include "app_state.h"
#include "audio_services.h"
#include "custom_assets.h"
#include "input_tasks.h"
#include "network_services.h"
#include "ota_services.h"
#include "sensor_services.h"
#include "ui_views.h"

#include <new>

namespace {
constexpr uint32_t kBootAnimTaskStack = 6144;
constexpr uint32_t kBootSyncTaskStack = 20480;
constexpr uint32_t kNetworkSyncTaskStack = 20480;
constexpr uint32_t kOtaTaskStack = 16384;
constexpr uint32_t kHousekeepingTaskStack = 5120;
constexpr uint32_t kUiTaskStack = 8192;
constexpr uint32_t kButtonTaskStack = 3072;
constexpr UBaseType_t kHighServiceTaskPriority = 4;
constexpr UBaseType_t kNormalServiceTaskPriority = 3;
constexpr UBaseType_t kInputTaskPriority = 2;
constexpr BaseType_t kNetworkTaskCore = 0;
constexpr BaseType_t kUiTaskCore = 1;
} // namespace

static void create_app_task(TaskFunction_t task,
                            const char *name,
                            uint32_t stack_depth,
                            UBaseType_t priority,
                            TaskHandle_t *handle,
                            BaseType_t core_id)
{
    if (handle) {
        *handle = nullptr;
    }
    if (!task || !name || stack_depth == 0) {
        ESP_LOGE(TAG, "invalid task create request");
        return;
    }
    if (xTaskCreatePinnedToCore(task, name, stack_depth, nullptr, priority, handle, core_id) != pdPASS) {
        ESP_LOGE(TAG, "%s task create failed", name);
    }
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs init requires erase: %s", esp_err_to_name(ret));
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs erase failed: %s", esp_err_to_name(ret));
            return;
        }
        ret = nvs_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs re-init failed: %s", esp_err_to_name(ret));
            return;
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
        return;
    }

    ota_mark_running_app_valid();
    g_app_events = xEventGroupCreate();
    if (!g_app_events) {
        ESP_LOGE(TAG, "app event group create failed");
        return;
    }
    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(ret));
        return;
    }
    init_power_management();
    load_hourly_sensor_history();
    load_daily_saying_cache();
    custom_assets_init();

    g_have_wifi_creds = load_saved_config();
    Rtc_Setup(&g_i2c, 0x51);
    setenv("TZ", "CST-8", 1);
    tzset();
    restore_system_time_from_rtc();
    g_shtc3 = new (std::nothrow) Shtc3Port(g_i2c);
    if (!g_shtc3) {
        ESP_LOGW(TAG, "shtc3 allocation failed");
    }
    sample_battery();
    init_wifi();

    g_display.RLCD_Init();
    g_display.RLCD_ColorClear(ColorWhite);
    g_display.RLCD_Display();
    Lvgl_PortInit(kDisplayWidth, kDisplayHeight, flush_callback);
    if (Lvgl_lock(-1)) {
        show_boot_screen();
        Lvgl_unlock();
    }
    g_boot_anim_current_frame = 0;
    g_boot_anim_running = true;
    xEventGroupClearBits(g_app_events, kBootSyncDoneBit | kBootAnimDoneBit);
    if (xTaskCreatePinnedToCore(boot_anim_task,
                                "boot_anim_task",
                                kBootAnimTaskStack,
                                nullptr,
                                kHighServiceTaskPriority,
                                &g_boot_anim_task_handle,
                                kUiTaskCore) != pdPASS) {
        ESP_LOGW(TAG, "boot animation task create failed");
        xEventGroupSetBits(g_app_events, kBootAnimDoneBit);
    }
    if (xTaskCreatePinnedToCore(boot_connectivity_task,
                                "boot_sync",
                                kBootSyncTaskStack,
                                nullptr,
                                kHighServiceTaskPriority,
                                &g_boot_sync_task_handle,
                                kNetworkTaskCore) != pdPASS) {
        ESP_LOGW(TAG, "boot connectivity task create failed");
        xEventGroupSetBits(g_app_events, kBootSyncDoneBit);
    }
    xEventGroupWaitBits(g_app_events,
                        kBootSyncDoneBit,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(kBootStartupBudgetMs + 500));
    update_boot_screen(100, "Ready", "Starting clock");
    g_boot_anim_running = false;
    xEventGroupWaitBits(g_app_events,
                        kBootAnimDoneBit,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(1500));
    finish_boot_anim_to_last_frame();
    finish_boot_screen();
    g_startup_screen_active = false;

    create_app_task(network_sync_task,
                    "network_sync",
                    kNetworkSyncTaskStack,
                    kHighServiceTaskPriority,
                    nullptr,
                    kNetworkTaskCore);
    create_app_task(ota_task,
                    "ota_task",
                    kOtaTaskStack,
                    kHighServiceTaskPriority,
                    nullptr,
                    kNetworkTaskCore);
    create_app_task(housekeeping_task,
                    "housekeeping",
                    kHousekeepingTaskStack,
                    kNormalServiceTaskPriority,
                    nullptr,
                    kUiTaskCore);
    create_app_task(ui_task,
                    "ui_task",
                    kUiTaskStack,
                    kNormalServiceTaskPriority,
                    &g_ui_task_handle,
                    kUiTaskCore);
    create_app_task(button_task,
                    "button_task",
                    kButtonTaskStack,
                    kInputTaskPriority,
                    nullptr,
                    kUiTaskCore);

    if (g_setup_prompt_pending) {
        vTaskDelay(pdMS_TO_TICKS(350));
        (void)start_setup_prompt_playback();
    }
}
