// 初始化硬件、系统服务和常驻任务，是固件应用入口。
#include "app_display_config.h"
#include "app_event_group.h"
#include "app_event_group_internal.h"
#include "app_hardware.h"
#include "app_constexpr.h"
#include "app_metadata.h"
#include "app_task_readiness.h"
#include "app_task_startup.h"
#include "lvgl_bsp.h"
#include "alarm_services_internal.h"
#include "battery_runtime_state_internal.h"
#include "pomodoro_services_internal.h"
#include "weather_city_mcp_internal.h"
#include "audio_services.h"
#include "audio_services_internal.h"
#include "custom_assets_internal.h"
#include "daily_saying_state_internal.h"
#include "manual_weather_city_state_internal.h"
#include "network_credentials_state_internal.h"
#include "network_diagnostics_state_internal.h"
#include "network_boot_sync.h"
#include "network_http_transaction_lock_internal.h"
#include "network_sync_request_generation_internal.h"
#include "saved_config_loader_internal.h"
#include "wifi_radio_services_internal.h"
#include "ntp_runtime_state_internal.h"
#include "ota_runtime_state_internal.h"
#include "ota_services.h"
#include "power_services_internal.h"
#include "rtc_services.h"
#include "runtime_health.h"
#include "sensor_services_internal.h"
#include "startup_state_internal.h"
#include "ui_boot_screen.h"
#include "ui_display_flush.h"
#include "ui_info_page_state_internal.h"
#include "ui_settings_feedback_internal.h"
#include "ui_settings_activity_state_internal.h"
#include "ui_work_page_catalog_internal.h"
#include "weather_state_internal.h"
#include "wifi_portal_state_internal.h"
#include "xiaozhi_ai.h"

#include "i2c_equipment.h"

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <stdlib.h>
#include <time.h>

#define MAIN_NVS_INIT_REQUIRES_ERASE_LOG_FORMAT "nvs init requires erase: %s"
#define MAIN_NVS_ERASE_FAILED_LOG_FORMAT "nvs erase failed: %s"
#define MAIN_NVS_REINIT_FAILED_LOG_FORMAT "nvs re-init failed: %s"
#define MAIN_NVS_INIT_FAILED_LOG_FORMAT "nvs init failed: %s"
#define MAIN_EVENT_GROUP_CREATE_FAILED_LOG_FORMAT "app event group create failed"
#define MAIN_NETIF_INIT_FAILED_LOG_FORMAT "netif init failed: %s"
#define MAIN_EVENT_LOOP_INIT_FAILED_LOG_FORMAT "event loop init failed: %s"
#define MAIN_WEATHER_STATE_INIT_FAILED_LOG_FORMAT "weather state initialization failed"
#define MAIN_NETWORK_CREDENTIALS_STATE_INIT_FAILED_LOG_FORMAT "network credentials state initialization failed"
#define MAIN_NETWORK_DIAG_STATE_INIT_FAILED_LOG_FORMAT "network diagnostics state initialization failed"
#define MAIN_NETWORK_REQUEST_GENERATION_INIT_FAILED_LOG_FORMAT "network request generation initialization failed"
#define MAIN_NTP_RUNTIME_STATE_INIT_FAILED_LOG_FORMAT "NTP runtime state initialization failed"
#define MAIN_SENSOR_SERVICES_STATE_INIT_FAILED_LOG_FORMAT "sensor services state initialization failed"
#define MAIN_OTA_RUNTIME_STATE_INIT_FAILED_LOG_FORMAT "OTA runtime state initialization failed"
#define MAIN_DAILY_SAYING_STATE_INIT_FAILED_LOG_FORMAT "daily saying state initialization failed"
#define MAIN_MANUAL_WEATHER_CITY_STATE_INIT_FAILED_LOG_FORMAT "manual weather city state initialization failed"
#define MAIN_WEATHER_CITY_MCP_STATE_INIT_FAILED_LOG_FORMAT "weather city MCP state initialization failed"
#define MAIN_WIFI_PORTAL_STATE_INIT_FAILED_LOG_FORMAT "Wi-Fi portal state initialization failed"
#define MAIN_BATTERY_RUNTIME_STATE_INIT_FAILED_LOG_FORMAT "battery runtime state initialization failed"
#define MAIN_INFO_PAGE_STATE_INIT_FAILED_LOG_FORMAT "info page state initialization failed"
#define MAIN_SETTINGS_ACTIVITY_STATE_INIT_FAILED_LOG_FORMAT "settings activity state initialization failed"
#define MAIN_SETTINGS_FEEDBACK_STATE_INIT_FAILED_LOG_FORMAT "settings feedback state initialization failed"
#define MAIN_WORK_PAGE_CATALOG_INIT_FAILED_LOG_FORMAT "work page catalog initialization failed"
#define MAIN_POMODORO_STATE_INIT_FAILED_LOG_FORMAT "pomodoro runtime state initialization failed"
#define MAIN_ALARM_STATE_INIT_FAILED_LOG_FORMAT "alarm runtime state initialization failed"
#define MAIN_INVALID_BOOT_TASK_LOG_FORMAT "%s: invalid boot task request"
#define MAIN_BOOT_TASK_CREATE_FAILED_LOG_FORMAT "%s"
#define MAIN_DISPLAY_UNAVAILABLE_LOG_FORMAT "RLCD display resources unavailable; startup stopped"
#define MAIN_I2C_UNAVAILABLE_LOG_FORMAT "I2C master bus unavailable; startup stopped"
#define MAIN_LVGL_INIT_FAILED_LOG_FORMAT "LVGL initialization failed; startup stopped"
#define MAIN_BOOT_SCREEN_FINISH_RETRY_LOG_FORMAT "boot screen finish retry: attempt=%u/%u"
#define MAIN_BOOT_SCREEN_FINISH_FAILED_LOG_FORMAT "boot screen finish failed; startup stopped"
#define MAIN_BOOT_TASK_COMPLETION_DELAYED_LOG_FORMAT \
    "%s completion delayed; holding startup until resources are released"
#define MAIN_STARTUP_RESOURCE_CLEANUP_LOG_FORMAT \
    "startup failed after resource activation; stopping Wi-Fi and parking audio"
#define MAIN_REGULAR_TASK_HEALTH_INCOMPLETE_LOG_FORMAT \
    "regular tasks incomplete; OTA remains pending: created=0x%08lx ready=0x%08lx failed=0x%08lx missing=0x%08lx"

namespace {
constexpr uint32_t kBootAnimTaskStack = 6144;
constexpr uint32_t kBootSyncTaskStack = 20480;
constexpr uint32_t kBootSyncWaitMarginMs = 500;
constexpr uint32_t kBootAnimStopWaitMs = 1500;
constexpr uint32_t kBootScreenFinishRetryDelayMs = 50;
constexpr uint32_t kBootScreenFinishMaxAttempts = 3;
constexpr uint32_t kSetupPromptStartDelayMs = 350;
constexpr uint32_t kRegularTaskReadyTimeoutMs = 10U * 1000U;
constexpr UBaseType_t kHighServiceTaskPriority = 4;
constexpr BaseType_t kNetworkTaskCore = 0;
constexpr BaseType_t kUiTaskCore = 1;
constexpr const char *kFallbackBootTaskName = "boot_task";
constexpr const char *kBootAnimTaskName = "boot_anim_task";
constexpr const char *kBootSyncTaskName = "boot_sync";
constexpr const char *kBootAnimTaskCreateFailed = "boot animation task create failed";
constexpr const char *kBootConnectivityTaskCreateFailed = "boot connectivity task create failed";
constexpr const char *kBootReadyStatus = "Ready";
constexpr const char *kBootReadyDetail = "Starting clock";
static_assert(kRegularTaskReadyTimeoutMs > 0,
              "regular task ready timeout must be positive");
struct AppInitializerSpec {
    bool (*initialize)();
    const char *failure_log;
};

constexpr AppInitializerSpec kCoreRuntimeStateInitializers[] = {
    {init_network_sync_request_generation, MAIN_NETWORK_REQUEST_GENERATION_INIT_FAILED_LOG_FORMAT},
    {init_weather_state, MAIN_WEATHER_STATE_INIT_FAILED_LOG_FORMAT},
    {network_credentials_state_init, MAIN_NETWORK_CREDENTIALS_STATE_INIT_FAILED_LOG_FORMAT},
    {network_diagnostics_state_init, MAIN_NETWORK_DIAG_STATE_INIT_FAILED_LOG_FORMAT},
    {ntp_runtime_state_init, MAIN_NTP_RUNTIME_STATE_INIT_FAILED_LOG_FORMAT},
    {init_sensor_services_state, MAIN_SENSOR_SERVICES_STATE_INIT_FAILED_LOG_FORMAT},
    {daily_saying_state_init, MAIN_DAILY_SAYING_STATE_INIT_FAILED_LOG_FORMAT},
    {init_manual_weather_city_state, MAIN_MANUAL_WEATHER_CITY_STATE_INIT_FAILED_LOG_FORMAT},
    {wifi_portal_state_init, MAIN_WIFI_PORTAL_STATE_INIT_FAILED_LOG_FORMAT},
    {battery_runtime_state_init, MAIN_BATTERY_RUNTIME_STATE_INIT_FAILED_LOG_FORMAT},
    {info_page_state_init, MAIN_INFO_PAGE_STATE_INIT_FAILED_LOG_FORMAT},
    {settings_activity_state_init, MAIN_SETTINGS_ACTIVITY_STATE_INIT_FAILED_LOG_FORMAT},
    {settings_feedback_state_init, MAIN_SETTINGS_FEEDBACK_STATE_INIT_FAILED_LOG_FORMAT},
    {work_page_catalog_init, MAIN_WORK_PAGE_CATALOG_INIT_FAILED_LOG_FORMAT},
};

constexpr AppInitializerSpec kFeatureRuntimeInitializers[] = {
    {alarm_services_init, MAIN_ALARM_STATE_INIT_FAILED_LOG_FORMAT},
    {pomodoro_services_init, MAIN_POMODORO_STATE_INIT_FAILED_LOG_FORMAT},
    {weather_city_mcp_init, MAIN_WEATHER_CITY_MCP_STATE_INIT_FAILED_LOG_FORMAT},
};

template <size_t Count>
constexpr bool app_initializer_specs_valid(
    const AppInitializerSpec (&specs)[Count])
{
    for (const AppInitializerSpec &spec : specs) {
        if (!spec.initialize || !spec.failure_log ||
            spec.failure_log[0] == '\0') {
            return false;
        }
    }
    return true;
}

template <size_t Count>
bool initialize_app_states(const AppInitializerSpec (&specs)[Count])
{
    for (const AppInitializerSpec &spec : specs) {
        if (!spec.initialize()) {
            ESP_LOGE(TAG, "%s", spec.failure_log);
            return false;
        }
    }
    return true;
}

static_assert(kBootScreenFinishRetryDelayMs > 0,
              "boot screen finish retry delay must be positive");
static_assert(kBootScreenFinishMaxAttempts > 1,
              "boot screen finish must retain a retry opportunity");
static_assert(array_count(kCoreRuntimeStateInitializers) > 0,
              "core runtime initializer table must not be empty");
static_assert(array_count(kFeatureRuntimeInitializers) > 0,
              "feature runtime initializer table must not be empty");
static_assert(app_initializer_specs_valid(kCoreRuntimeStateInitializers),
              "core runtime initializer specs must be valid");
static_assert(app_initializer_specs_valid(kFeatureRuntimeInitializers),
              "feature runtime initializer specs must be valid");

} // namespace

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

static bool init_system_event_services()
{
    if (app_event_group_ready()) {
        return true;
    }
    if (!app_event_group_init()) {
        ESP_LOGE(TAG, MAIN_EVENT_GROUP_CREATE_FAILED_LOG_FORMAT);
        return false;
    }
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, MAIN_NETIF_INIT_FAILED_LOG_FORMAT, esp_err_to_name(ret));
        app_event_group_release();
        return false;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, MAIN_EVENT_LOOP_INIT_FAILED_LOG_FORMAT, esp_err_to_name(ret));
        // ESP-IDF 5.5 does not support esp_netif_deinit(). Release the owned
        // event group and leave the TCP/IP stack inert instead of retaining a
        // stale global handle that later code could mistake for a usable bus.
        app_event_group_release();
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
        app_event_group_set_bits(done_bit);
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
        app_event_group_set_bits(done_bit);
    }
}

static bool finish_boot_screen_with_retry()
{
    for (uint32_t attempt = 1; attempt <= kBootScreenFinishMaxAttempts; ++attempt) {
        if (finish_boot_screen()) {
            return true;
        }
        if (attempt < kBootScreenFinishMaxAttempts) {
            ESP_LOGW(TAG,
                     MAIN_BOOT_SCREEN_FINISH_RETRY_LOG_FORMAT,
                     static_cast<unsigned>(attempt + 1),
                     static_cast<unsigned>(kBootScreenFinishMaxAttempts));
            vTaskDelay(pdMS_TO_TICKS(kBootScreenFinishRetryDelayMs));
        }
    }
    ESP_LOGE(TAG, "%s", MAIN_BOOT_SCREEN_FINISH_FAILED_LOG_FORMAT);
    return false;
}

static void wait_for_boot_task_completion(EventBits_t done_bit,
                                          TickType_t expected_wait,
                                          const char *task_name)
{
    EventBits_t bits = app_event_group_wait_bits(done_bit,
                                                 pdFALSE,
                                                 pdTRUE,
                                                 expected_wait);
    if ((bits & done_bit) != 0) {
        return;
    }
    ESP_LOGW(TAG,
             MAIN_BOOT_TASK_COMPLETION_DELAYED_LOG_FORMAT,
             task_name ? task_name : kFallbackBootTaskName);
    // Both boot tasks own temporary stacks and the connectivity task may also
    // own Wi-Fi/PM resources. Do not create permanent services against those
    // resources after only the expected-duration window has elapsed.
    app_event_group_wait_bits(done_bit,
                              pdFALSE,
                              pdTRUE,
                              portMAX_DELAY);
}

static void cleanup_failed_startup_resources()
{
    ESP_LOGW(TAG, "%s", MAIN_STARTUP_RESOURCE_CLEANUP_LOG_FORMAT);
    stop_wifi_radio(true);
    park_unused_audio_peripherals();
}

extern "C" void app_main(void)
{
    DisplayPort &display = app_display();
    I2cMasterBus &i2c = app_i2c();
    if (!display.IsReady()) {
        ESP_LOGE(TAG, MAIN_DISPLAY_UNAVAILABLE_LOG_FORMAT);
        return;
    }
    if (!i2c.IsReady()) {
        ESP_LOGE(TAG, MAIN_I2C_UNAVAILABLE_LOG_FORMAT);
        return;
    }
    if (!init_nvs_storage()) {
        return;
    }

    if (!ota_runtime_state_init()) {
        ESP_LOGE(TAG, MAIN_OTA_RUNTIME_STATE_INIT_FAILED_LOG_FORMAT);
        return;
    }
    if (!init_system_event_services()) {
        return;
    }
    if (!init_network_http_transaction_lock()) {
        return;
    }
    if (!initialize_app_states(kCoreRuntimeStateInitializers)) {
        return;
    }
    init_power_management();
    load_hourly_sensor_history();
    reset_daily_saying_cache();
    custom_assets_init();

    (void)load_saved_config();
    Rtc_Setup(&i2c, 0x51);
    setenv("TZ", "CST-8", 1);
    tzset();
    restore_system_time_from_rtc();
    load_battery_charge_history();
    init_shtc3_sensor(i2c);
    sample_battery();
    if (!battery_low_mode_load()) {
        sample_sensor();
    }
    init_wifi();
    park_unused_audio_peripherals();
    xiaozhi_ai_init();
    if (!initialize_app_states(kFeatureRuntimeInitializers)) {
        cleanup_failed_startup_resources();
        return;
    }

    display.RLCD_Init();
    if (!display.IsReady()) {
        ESP_LOGE(TAG, MAIN_DISPLAY_UNAVAILABLE_LOG_FORMAT);
        cleanup_failed_startup_resources();
        return;
    }
    display.RLCD_ColorClear(ColorWhite);
    display.RLCD_Display();
    if (!Lvgl_PortInit(kDisplayWidth, kDisplayHeight, flush_callback)) {
        ESP_LOGE(TAG, MAIN_LVGL_INIT_FAILED_LOG_FORMAT);
        cleanup_failed_startup_resources();
        return;
    }
    if (Lvgl_lock(-1)) {
        show_boot_screen();
        Lvgl_unlock();
    }
    prepare_boot_animation();
    app_event_group_clear_bits(kBootSyncDoneBit | kBootAnimDoneBit);
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
    wait_for_boot_task_completion(
        kBootSyncDoneBit,
        pdMS_TO_TICKS(kBootStartupBudgetMs + kBootSyncWaitMarginMs),
        kBootSyncTaskName);
    update_boot_screen(100, kBootReadyStatus, kBootReadyDetail);
    request_boot_animation_stop();
    wait_for_boot_task_completion(kBootAnimDoneBit,
                                  pdMS_TO_TICKS(kBootAnimStopWaitMs),
                                  kBootAnimTaskName);
    finish_boot_anim_to_last_frame();
    if (!finish_boot_screen_with_retry()) {
        cleanup_failed_startup_resources();
        return;
    }
    startup_screen_mark_finished();

    // A transient early allocation or PM-driver failure must not permanently
    // disable runtime sleep or network/audio protection. Successful resources
    // are retained, so the normal path only checks the ready catalog.
    init_power_management();
    regular_app_task_readiness_begin();
    const AppTaskStartupResult task_startup = create_regular_app_tasks();
    const uint32_t ready_mask = wait_for_regular_app_tasks_ready(
        task_startup.created_mask,
        pdMS_TO_TICKS(kRegularTaskReadyTimeoutMs));
    if (regular_app_tasks_healthy_for_ota(task_startup, ready_mask)) {
        ota_mark_running_app_valid();
        runtime_health_log_snapshot("startup-ready");
    } else {
        ESP_LOGE(TAG,
                 MAIN_REGULAR_TASK_HEALTH_INCOMPLETE_LOG_FORMAT,
                 static_cast<unsigned long>(task_startup.created_mask),
                 static_cast<unsigned long>(ready_mask),
                 static_cast<unsigned long>(task_startup.failed_mask),
                 static_cast<unsigned long>(
                     regular_app_task_missing_ready_mask(task_startup,
                                                         ready_mask)));
    }

    if (setup_prompt_playback_pending()) {
        vTaskDelay(pdMS_TO_TICKS(kSetupPromptStartDelayMs));
        if (setup_prompt_playback_pending()) {
            (void)start_setup_prompt_playback();
        }
    }
}
