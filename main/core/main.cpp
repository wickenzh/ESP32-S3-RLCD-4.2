// 初始化硬件、系统服务和常驻任务，是固件应用入口。
#include "app_state.h"
#include "app_constexpr.h"
#include "alarm_services.h"
#include "pomodoro_services.h"
#include "weather_city_mcp.h"
#include "audio_services.h"
#include "custom_assets.h"
#include "input_tasks.h"
#include "network_services.h"
#include "ota_services.h"
#include "radio_services.h"
#include "music_player.h"
#include "sensor_services.h"
#include "startup_state.h"
#include "ui_display_flush.h"
#include "ui_task_notify.h"
#include "ui_views.h"
#include "xiaozhi_ai.h"

#include <new>

#define MAIN_INVALID_TASK_CREATE_LOG_FORMAT "%s: invalid task create request"
#define MAIN_TASK_CREATE_FAILED_LOG_FORMAT "%s task create failed"
#define MAIN_NVS_INIT_REQUIRES_ERASE_LOG_FORMAT "nvs init requires erase: %s"
#define MAIN_NVS_ERASE_FAILED_LOG_FORMAT "nvs erase failed: %s"
#define MAIN_NVS_REINIT_FAILED_LOG_FORMAT "nvs re-init failed: %s"
#define MAIN_NVS_INIT_FAILED_LOG_FORMAT "nvs init failed: %s"
#define MAIN_EVENT_GROUP_CREATE_FAILED_LOG_FORMAT "app event group create failed"
#define MAIN_NETIF_INIT_FAILED_LOG_FORMAT "netif init failed: %s"
#define MAIN_EVENT_LOOP_INIT_FAILED_LOG_FORMAT "event loop init failed: %s"
#define MAIN_INVALID_BOOT_TASK_LOG_FORMAT "%s: invalid boot task request"
#define MAIN_BOOT_TASK_CREATE_FAILED_LOG_FORMAT "%s"
#define MAIN_SHTC3_ALLOCATION_FAILED_LOG_FORMAT "shtc3 allocation failed"
#define MAIN_DISPLAY_UNAVAILABLE_LOG_FORMAT "RLCD display resources unavailable; startup stopped"
#define MAIN_I2C_UNAVAILABLE_LOG_FORMAT "I2C master bus unavailable; startup stopped"
#define MAIN_LVGL_INIT_FAILED_LOG_FORMAT "LVGL initialization failed; startup stopped"

namespace {
constexpr uint32_t kBootAnimTaskStack = 6144;
constexpr uint32_t kBootSyncTaskStack = 20480;
constexpr uint32_t kNetworkSyncTaskStack = 20480;
constexpr uint32_t kOtaTaskStack = 16384;
constexpr uint32_t kHousekeepingTaskStack = 5120;
constexpr uint32_t kUiTaskStack = 8192;
constexpr uint32_t kButtonTaskStack = 3072;
constexpr uint32_t kAlarmTaskStack = 4096;
constexpr uint32_t kPomodoroTaskStack = 4096;
constexpr uint32_t kBootSyncWaitMarginMs = 500;
constexpr uint32_t kBootAnimStopWaitMs = 1500;
constexpr uint32_t kSetupPromptStartDelayMs = 350;
constexpr UBaseType_t kHighServiceTaskPriority = 4;
constexpr UBaseType_t kNormalServiceTaskPriority = 3;
constexpr UBaseType_t kInputTaskPriority = 2;
constexpr BaseType_t kNetworkTaskCore = 0;
constexpr BaseType_t kUiTaskCore = 1;
constexpr const char *kFallbackAppTaskName = "app_task";
constexpr const char *kFallbackBootTaskName = "boot_task";
constexpr const char *kBootAnimTaskName = "boot_anim_task";
constexpr const char *kBootSyncTaskName = "boot_sync";
constexpr const char *kNetworkSyncTaskName = "network_sync";
constexpr const char *kOtaTaskName = "ota_task";
constexpr const char *kHousekeepingTaskName = "housekeeping";
constexpr const char *kUiTaskName = "ui_task";
constexpr const char *kButtonTaskName = "button_task";
constexpr const char *kAlarmTaskName = "alarm_task";
constexpr const char *kPomodoroTaskName = "pomodoro_task";
constexpr const char *kBootAnimTaskCreateFailed = "boot animation task create failed";
constexpr const char *kBootConnectivityTaskCreateFailed = "boot connectivity task create failed";
constexpr const char *kBootReadyStatus = "Ready";
constexpr const char *kBootReadyDetail = "Starting clock";
StaticEventGroup_t s_app_event_group_storage = {};

struct AppTaskSpec {
    TaskFunction_t task;
    const char *name;
    uint32_t stack_depth;
    UBaseType_t priority;
    bool register_ui_handle;
    BaseType_t core_id;
};

constexpr AppTaskSpec kRegularAppTasks[] = {
    {network_sync_task, kNetworkSyncTaskName, kNetworkSyncTaskStack, kHighServiceTaskPriority, false, kNetworkTaskCore},
    {ota_task, kOtaTaskName, kOtaTaskStack, kHighServiceTaskPriority, false, kNetworkTaskCore},
    {housekeeping_task, kHousekeepingTaskName, kHousekeepingTaskStack, kNormalServiceTaskPriority, false, kUiTaskCore},
    {ui_task, kUiTaskName, kUiTaskStack, kNormalServiceTaskPriority, true, kUiTaskCore},
    {button_task, kButtonTaskName, kButtonTaskStack, kInputTaskPriority, false, kUiTaskCore},
    {alarm_task, kAlarmTaskName, kAlarmTaskStack, kNormalServiceTaskPriority, false, kUiTaskCore},
    {pomodoro_task, kPomodoroTaskName, kPomodoroTaskStack, kNormalServiceTaskPriority, false, kUiTaskCore},
};

constexpr bool app_task_specs_valid()
{
    for (const AppTaskSpec &spec : kRegularAppTasks) {
        if (!spec.task || !spec.name || spec.name[0] == '\0' ||
            spec.stack_depth == 0 || spec.priority >= configMAX_PRIORITIES ||
            spec.core_id < 0 || spec.core_id >= portNUM_PROCESSORS ||
            (spec.register_ui_handle && spec.task != ui_task)) {
            return false;
        }
    }
    return true;
}

static_assert(array_count(kRegularAppTasks) > 0,
              "regular task table must not be empty");
static_assert(app_task_specs_valid(), "regular app task specs must be valid");
} // namespace

static TaskHandle_t create_app_task(TaskFunction_t task,
                                    const char *name,
                                    uint32_t stack_depth,
                                    UBaseType_t priority,
                                    BaseType_t core_id)
{
    const char *task_name = name ? name : kFallbackAppTaskName;
    if (!task || stack_depth == 0) {
        ESP_LOGE(TAG, MAIN_INVALID_TASK_CREATE_LOG_FORMAT, task_name);
        return nullptr;
    }
    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCore(task, task_name, stack_depth, nullptr, priority, &handle, core_id) != pdPASS) {
        ESP_LOGE(TAG, MAIN_TASK_CREATE_FAILED_LOG_FORMAT, task_name);
        return nullptr;
    }
    return handle;
}

static void create_regular_app_tasks()
{
    for (const AppTaskSpec &task : kRegularAppTasks) {
        TaskHandle_t handle = create_app_task(task.task,
                                              task.name,
                                              task.stack_depth,
                                              task.priority,
                                              task.core_id);
        if (task.register_ui_handle) {
            register_ui_task_handle(handle);
        }
    }
}

static bool init_nvs_storage()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, MAIN_NVS_INIT_REQUIRES_ERASE_LOG_FORMAT, esp_err_to_name(ret));
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, MAIN_NVS_ERASE_FAILED_LOG_FORMAT, esp_err_to_name(ret));
            return false;
        }
        ret = nvs_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, MAIN_NVS_REINIT_FAILED_LOG_FORMAT, esp_err_to_name(ret));
            return false;
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, MAIN_NVS_INIT_FAILED_LOG_FORMAT, esp_err_to_name(ret));
        return false;
    }
    return true;
}

static void release_app_event_group()
{
    if (!g_app_events) {
        return;
    }
    vEventGroupDelete(g_app_events);
    g_app_events = nullptr;
}

static bool init_system_event_services()
{
    if (g_app_events) {
        return true;
    }
    g_app_events = xEventGroupCreateStatic(&s_app_event_group_storage);
    if (!g_app_events) {
        ESP_LOGE(TAG, MAIN_EVENT_GROUP_CREATE_FAILED_LOG_FORMAT);
        return false;
    }
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, MAIN_NETIF_INIT_FAILED_LOG_FORMAT, esp_err_to_name(ret));
        release_app_event_group();
        return false;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, MAIN_EVENT_LOOP_INIT_FAILED_LOG_FORMAT, esp_err_to_name(ret));
        // ESP-IDF 5.5 does not support esp_netif_deinit(). Release the owned
        // event group and leave the TCP/IP stack inert instead of retaining a
        // stale global handle that later code could mistake for a usable bus.
        release_app_event_group();
        return false;
    }
    return true;
}

static void create_boot_task_or_signal(TaskFunction_t task,
                                       const char *name,
                                       uint32_t stack_depth,
                                       BaseType_t core_id,
                                       EventBits_t done_bit,
                                       const char *failure_log)
{
    const char *task_name = name ? name : kFallbackBootTaskName;
    if (!task || stack_depth == 0) {
        ESP_LOGW(TAG, MAIN_INVALID_BOOT_TASK_LOG_FORMAT, failure_log ? failure_log : task_name);
        xEventGroupSetBits(g_app_events, done_bit);
        return;
    }
    if (xTaskCreatePinnedToCore(task,
                                task_name,
                                stack_depth,
                                nullptr,
                                kHighServiceTaskPriority,
                                nullptr,
                                core_id) != pdPASS) {
        ESP_LOGW(TAG, MAIN_BOOT_TASK_CREATE_FAILED_LOG_FORMAT, failure_log);
        xEventGroupSetBits(g_app_events, done_bit);
    }
}

extern "C" void app_main(void)
{
    if (!g_display.IsReady()) {
        ESP_LOGE(TAG, MAIN_DISPLAY_UNAVAILABLE_LOG_FORMAT);
        return;
    }
    if (!g_i2c.IsReady()) {
        ESP_LOGE(TAG, MAIN_I2C_UNAVAILABLE_LOG_FORMAT);
        return;
    }
    if (!init_nvs_storage()) {
        return;
    }

    ota_mark_running_app_valid();
    if (!init_system_event_services()) {
        return;
    }
    if (!init_network_http_transaction_lock()) {
        return;
    }
    init_power_management();
    load_hourly_sensor_history();
    load_daily_saying_cache();
    custom_assets_init();

    (void)load_saved_config();
    Rtc_Setup(&g_i2c, 0x51);
    setenv("TZ", "CST-8", 1);
    tzset();
    restore_system_time_from_rtc();
    g_shtc3 = new (std::nothrow) Shtc3Port(g_i2c);
    if (!g_shtc3) {
        ESP_LOGW(TAG, MAIN_SHTC3_ALLOCATION_FAILED_LOG_FORMAT);
    }
    sample_battery();
    if (!battery_low_mode_load()) {
        sample_sensor();
    }
    init_wifi();
    park_unused_audio_peripherals();
    radio_init();
    music_init();
    xiaozhi_ai_init();
    alarm_services_init();
    pomodoro_services_init();
    weather_city_mcp_init();

    g_display.RLCD_Init();
    g_display.RLCD_ColorClear(ColorWhite);
    g_display.RLCD_Display();
    if (!Lvgl_PortInit(kDisplayWidth, kDisplayHeight, flush_callback)) {
        ESP_LOGE(TAG, MAIN_LVGL_INIT_FAILED_LOG_FORMAT);
        return;
    }
    if (Lvgl_lock(-1)) {
        show_boot_screen();
        Lvgl_unlock();
    }
    prepare_boot_animation();
    xEventGroupClearBits(g_app_events, kBootSyncDoneBit | kBootAnimDoneBit);
    create_boot_task_or_signal(boot_anim_task,
                               kBootAnimTaskName,
                               kBootAnimTaskStack,
                               kUiTaskCore,
                               kBootAnimDoneBit,
                               kBootAnimTaskCreateFailed);
    create_boot_task_or_signal(boot_connectivity_task,
                               kBootSyncTaskName,
                               kBootSyncTaskStack,
                               kNetworkTaskCore,
                               kBootSyncDoneBit,
                               kBootConnectivityTaskCreateFailed);
    xEventGroupWaitBits(g_app_events,
                        kBootSyncDoneBit,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(kBootStartupBudgetMs + kBootSyncWaitMarginMs));
    update_boot_screen(100, kBootReadyStatus, kBootReadyDetail);
    request_boot_animation_stop();
    xEventGroupWaitBits(g_app_events,
                        kBootAnimDoneBit,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(kBootAnimStopWaitMs));
    finish_boot_anim_to_last_frame();
    finish_boot_screen();
    startup_screen_mark_finished();

    create_regular_app_tasks();

    if (setup_prompt_playback_pending()) {
        vTaskDelay(pdMS_TO_TICKS(kSetupPromptStartDelayMs));
        (void)start_setup_prompt_playback();
    }
}
