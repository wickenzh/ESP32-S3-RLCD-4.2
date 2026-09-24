// 汇总常驻任务 ready 位，并通过任务通知唤醒开机健康等待者。
#include "app_task_readiness.h"
#include "runtime_health.h"

#include <freertos/task.h>

#include <atomic>

namespace {
std::atomic<uint32_t> s_ready_mask{0};
portMUX_TYPE s_waiter_mux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t s_waiter = nullptr;

void publish_waiter(TaskHandle_t waiter)
{
    portENTER_CRITICAL(&s_waiter_mux);
    s_waiter = waiter;
    portEXIT_CRITICAL(&s_waiter_mux);
}

void notify_waiter()
{
    portENTER_CRITICAL(&s_waiter_mux);
    TaskHandle_t waiter = s_waiter;
    portEXIT_CRITICAL(&s_waiter_mux);
    if (waiter) {
        xTaskNotifyGive(waiter);
    }
}
} // namespace

void regular_app_task_readiness_begin()
{
    s_ready_mask.store(0, std::memory_order_release);
    publish_waiter(xTaskGetCurrentTaskHandle());
}

void regular_app_task_mark_ready(RegularAppTaskId id)
{
    const uint32_t bit = regular_app_task_bit(id);
    if ((bit & kAllRegularAppTaskBits) == 0) {
        return;
    }
    s_ready_mask.fetch_or(bit, std::memory_order_acq_rel);
    runtime_health_record_current_task_stack(id);
    notify_waiter();
}

uint32_t regular_app_task_ready_mask()
{
    return s_ready_mask.load(std::memory_order_acquire);
}

uint32_t wait_for_regular_app_tasks_ready(uint32_t expected_mask,
                                          TickType_t timeout)
{
    const TickType_t started = xTaskGetTickCount();
    for (;;) {
        const uint32_t ready = regular_app_task_ready_mask();
        if ((ready & expected_mask) == expected_mask) {
            publish_waiter(nullptr);
            return ready;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) {
            publish_waiter(nullptr);
            return ready;
        }
        (void)ulTaskNotifyTake(pdTRUE, timeout - elapsed);
    }
}
