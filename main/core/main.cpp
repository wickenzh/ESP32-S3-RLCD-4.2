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
constexpr uint32_t kBootSyncWaitMarginMs = 500;
constexpr uint32_t kBootAnimStopWaitMs = 1500;
constexpr uint32_t kSetupPromptStartDelayMs = 350;
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
    const char *task_name = name ? name : "app_task";
    if (handle) {
        *handle = nullptr;
    }
    if (!task || stack_depth == 0) {
        ESP_LOGE(TAG, "%s: invalid task create request", task_name);
        return;
    }
    if (xTaskCreatePinnedToCore(task, task_name, stack_depth, nullptr, priority, handle, core_id) != pdPASS) {
        ESP_LOGE(TAG, "%s task create failed", task_name);
    }
}

static bool init_nvs_storage()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs init requires erase: %s", esp_err_to_name(ret));
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs erase failed: %s", esp_err_to_name(ret));
            return false;
        }
        ret = nvs_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs re-init failed: %s", esp_err_to_name(ret));
            return false;
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

static bool init_system_event_services()
{
    g_app_events = xEventGroupCreate();
    if (!g_app_events) {
        ESP_LOGE(TAG, "app event group create failed");
        return false;
    }
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

static void create_boot_task_or_signal(TaskFunction_t task,
                                       const char *name,
                                       uint32_t stack_depth,
                                       TaskHandle_t *handle,
                                       BaseType_t core_id,
                                       EventBits_t done_bit,
                                       const char *failure_log)
{
    const char *task_name = name ? name : "boot_task";
    if (handle) {
        *handle = nullptr;
    }
    if (!task || stack_depth == 0) {
        ESP_LOGW(TAG, "%s: invalid boot task request", failure_log ? failure_log : task_name);
        xEventGroupSetBits(g_app_events, done_bit);
        return;
    }
    if (xTaskCreatePinnedToCore(task,
                                task_name,
                                stack_depth,
                                nullptr,
                                kHighServiceTaskPriority,
                                handle,
                                core_id) != pdPASS) {
        ESP_LOGW(TAG, "%s", failure_log);
        xEventGroupSetBits(g_app_events, done_bit);
    }
}

extern "C" void app_main(void)
{
    if (!init_nvs_storage()) {
        return;
    }

    ota_mark_running_app_valid();
    if (!init_system_event_services()) {
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
    create_boot_task_or_signal(boot_anim_task,
                               "boot_anim_task",
                               kBootAnimTaskStack,
                               &g_boot_anim_task_handle,
                               kUiTaskCore,
                               kBootAnimDoneBit,
                               "boot animation task create failed");
    create_boot_task_or_signal(boot_connectivity_task,
                               "boot_sync",
                               kBootSyncTaskStack,
                               &g_boot_sync_task_handle,
                               kNetworkTaskCore,
                               kBootSyncDoneBit,
                               "boot connectivity task create failed");
    xEventGroupWaitBits(g_app_events,
                        kBootSyncDoneBit,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(kBootStartupBudgetMs + kBootSyncWaitMarginMs));
    update_boot_screen(100, "Ready", "Starting clock");
    g_boot_anim_running = false;
    xEventGroupWaitBits(g_app_events,
                        kBootAnimDoneBit,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(kBootAnimStopWaitMs));
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
        vTaskDelay(pdMS_TO_TICKS(kSetupPromptStartDelayMs));
        (void)start_setup_prompt_playback();
    }
}
