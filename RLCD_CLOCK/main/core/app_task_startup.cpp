// 集中维护常驻应用任务规格、参数校验和有界补建流程。
#include "app_task_startup.h"

#include "alarm_services.h"
#include "app_constexpr.h"
#include "app_metadata.h"
#include "input_tasks.h"
#include "network_sync_task.h"
#include "ota_services.h"
#include "pomodoro_services.h"
#include "runtime_health.h"
#include "sensor_services_internal.h"
#include "ui_task.h"
#include "ui_task_notify.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stddef.h>
#include <stdint.h>

#define APP_TASK_INVALID_CREATE_LOG_FORMAT "%s: invalid task create request"
#define APP_TASK_CREATE_FAILED_LOG_FORMAT "%s task create failed"
#define APP_TASK_CREATE_RETRY_LOG_FORMAT "regular task create retry: attempt=%u pending=0x%08lx"
#define APP_TASK_CREATE_EXHAUSTED_LOG_FORMAT "regular task create incomplete: pending=0x%08lx"

namespace {
constexpr uint32_t kNetworkSyncTaskStack = 20480;
constexpr uint32_t kOtaTaskStack = 16384;
constexpr uint32_t kHousekeepingTaskStack = 5120;
constexpr uint32_t kUiTaskStack = 8192;
constexpr uint32_t kButtonTaskStack = 3072;
constexpr uint32_t kAlarmTaskStack = 4096;
constexpr uint32_t kPomodoroTaskStack = 4096;
constexpr uint32_t kRegularTaskCreateRetryDelayMs = 100;
constexpr uint32_t kRegularTaskCreateMaxAttempts = 3;
constexpr UBaseType_t kHighServiceTaskPriority = 4;
constexpr UBaseType_t kNormalServiceTaskPriority = 3;
constexpr UBaseType_t kInputTaskPriority = 2;
constexpr BaseType_t kNetworkTaskCore = 0;
constexpr BaseType_t kUiTaskCore = 1;
constexpr const char *kFallbackAppTaskName = "app_task";
constexpr const char *kNetworkSyncTaskName = "network_sync";
constexpr const char *kOtaTaskName = "ota_task";
constexpr const char *kHousekeepingTaskName = "housekeeping";
constexpr const char *kUiTaskName = "ui_task";
constexpr const char *kButtonTaskName = "button_task";
constexpr const char *kAlarmTaskName = "alarm_task";
constexpr const char *kPomodoroTaskName = "pomodoro_task";

struct AppTaskSpec {
    RegularAppTaskId id;
    TaskFunction_t task;
    const char *name;
    uint32_t stack_depth;
    UBaseType_t priority;
    bool register_ui_handle;
    BaseType_t core_id;
};

constexpr AppTaskSpec kRegularAppTasks[] = {
    {RegularAppTaskId::kNetworkSync, network_sync_task, kNetworkSyncTaskName, kNetworkSyncTaskStack, kHighServiceTaskPriority, false, kNetworkTaskCore},
    {RegularAppTaskId::kOta, ota_task, kOtaTaskName, kOtaTaskStack, kHighServiceTaskPriority, false, kNetworkTaskCore},
    {RegularAppTaskId::kHousekeeping, housekeeping_task, kHousekeepingTaskName, kHousekeepingTaskStack, kNormalServiceTaskPriority, false, kUiTaskCore},
    {RegularAppTaskId::kUi, ui_task, kUiTaskName, kUiTaskStack, kNormalServiceTaskPriority, true, kUiTaskCore},
    {RegularAppTaskId::kButton, button_task, kButtonTaskName, kButtonTaskStack, kInputTaskPriority, false, kUiTaskCore},
    {RegularAppTaskId::kAlarm, alarm_task, kAlarmTaskName, kAlarmTaskStack, kNormalServiceTaskPriority, false, kUiTaskCore},
    {RegularAppTaskId::kPomodoro, pomodoro_task, kPomodoroTaskName, kPomodoroTaskStack, kNormalServiceTaskPriority, false, kUiTaskCore},
};

constexpr bool app_task_specs_valid()
{
    for (const AppTaskSpec &spec : kRegularAppTasks) {
        if (!spec.task || !spec.name || spec.name[0] == '\0' ||
            spec.stack_depth == 0 || spec.priority >= configMAX_PRIORITIES ||
            (regular_app_task_bit(spec.id) & kAllRegularAppTaskBits) == 0 ||
            spec.core_id < 0 || spec.core_id >= portNUM_PROCESSORS ||
            (spec.register_ui_handle && spec.task != ui_task)) {
            return false;
        }
    }
    return true;
}

constexpr bool app_task_specs_are_unique()
{
    for (size_t i = 0; i < array_count(kRegularAppTasks); ++i) {
        for (size_t j = i + 1; j < array_count(kRegularAppTasks); ++j) {
            if (kRegularAppTasks[i].id == kRegularAppTasks[j].id ||
                kRegularAppTasks[i].task == kRegularAppTasks[j].task ||
                cstr_equal(kRegularAppTasks[i].name,
                           kRegularAppTasks[j].name)) {
                return false;
            }
        }
    }
    return true;
}

constexpr bool app_task_specs_have_single_ui_owner()
{
    size_t ui_owner_count = 0;
    for (const AppTaskSpec &spec : kRegularAppTasks) {
        if (spec.register_ui_handle) {
            ++ui_owner_count;
            if (spec.task != ui_task) {
                return false;
            }
        }
    }
    return ui_owner_count == 1;
}

static_assert(array_count(kRegularAppTasks) > 0,
              "regular task table must not be empty");
static_assert(array_count(kRegularAppTasks) ==
                  static_cast<size_t>(RegularAppTaskId::kCount),
              "regular task table must cover every task id");
static_assert(array_count(kRegularAppTasks) < 32,
              "regular task retry mask must fit in uint32_t");
static_assert(kRegularTaskCreateRetryDelayMs > 0,
              "regular task retry delay must be positive");
static_assert(kRegularTaskCreateMaxAttempts > 1,
              "regular task creation must retain a retry opportunity");
static_assert(app_task_specs_valid(), "regular app task specs must be valid");
static_assert(app_task_specs_are_unique(),
              "regular app task functions and names must be unique");
static_assert(app_task_specs_have_single_ui_owner(),
              "regular app task table must register exactly one UI owner");

TaskHandle_t create_app_task(const AppTaskSpec &spec)
{
    const char *task_name = spec.name ? spec.name : kFallbackAppTaskName;
    if (!spec.task || spec.stack_depth == 0) {
        ESP_LOGE(TAG, APP_TASK_INVALID_CREATE_LOG_FORMAT, task_name);
        return nullptr;
    }
    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCore(spec.task,
                                task_name,
                                spec.stack_depth,
                                nullptr,
                                spec.priority,
                                &handle,
                                spec.core_id) != pdPASS) {
        ESP_LOGE(TAG, APP_TASK_CREATE_FAILED_LOG_FORMAT, task_name);
        runtime_health_note_event(RuntimeHealthEvent::kTaskCreateFailure);
        runtime_health_log_snapshot("task-create-failed");
        return nullptr;
    }
    return handle;
}
} // namespace

AppTaskStartupResult create_regular_app_tasks()
{
    AppTaskStartupResult result = {};
    uint32_t pending = kAllRegularAppTaskBits;
    for (uint32_t attempt = 1;
         attempt <= kRegularTaskCreateMaxAttempts && pending != 0;
         ++attempt) {
        uint32_t failed = 0;
        for (size_t index = 0; index < array_count(kRegularAppTasks); ++index) {
            const AppTaskSpec &task = kRegularAppTasks[index];
            const uint32_t task_bit = regular_app_task_bit(task.id);
            if ((pending & task_bit) == 0) {
                continue;
            }
            TaskHandle_t handle = create_app_task(task);
            if (!handle) {
                failed |= task_bit;
                continue;
            }
            if (task.register_ui_handle) {
                register_ui_task_handle(handle);
            }
            result.created_mask |= task_bit;
        }
        pending = failed;
        if (pending != 0 && attempt < kRegularTaskCreateMaxAttempts) {
            ESP_LOGW(TAG,
                     APP_TASK_CREATE_RETRY_LOG_FORMAT,
                     static_cast<unsigned>(attempt + 1),
                     static_cast<unsigned long>(pending));
            // Recently deleted boot tasks release dynamic stacks from the
            // Idle task. Yield before retrying only the missing services.
            vTaskDelay(pdMS_TO_TICKS(kRegularTaskCreateRetryDelayMs));
        }
    }
    if (pending != 0) {
        ESP_LOGE(TAG,
                 APP_TASK_CREATE_EXHAUSTED_LOG_FORMAT,
                 static_cast<unsigned long>(pending));
    }
    result.failed_mask = pending;
    return result;
}
