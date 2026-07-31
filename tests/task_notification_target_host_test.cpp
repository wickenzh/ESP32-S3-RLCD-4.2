// 验证任务通知目标在发布前、普通任务和 ISR 路径中的语义。
#include "task_notification_target.h"

#include <atomic>
#include <cassert>
#include <thread>

namespace {
std::atomic<int> g_task_notifications{0};
std::atomic<int> g_isr_notifications{0};
TaskHandle_t g_expected_handle = reinterpret_cast<TaskHandle_t>(0x1234);
} // namespace

void xTaskNotifyGive(TaskHandle_t handle)
{
    assert(handle == g_expected_handle);
    g_task_notifications.fetch_add(1, std::memory_order_relaxed);
}

void vTaskNotifyGiveFromISR(TaskHandle_t handle,
                            BaseType_t *higher_priority_task_woken)
{
    assert(handle == g_expected_handle);
    g_isr_notifications.fetch_add(1, std::memory_order_relaxed);
    if (higher_priority_task_woken) {
        *higher_priority_task_woken = pdTRUE;
    }
}

int main()
{
    TaskNotificationTarget target;
    BaseType_t higher_priority_task_woken = pdFALSE;
    assert(!target.notify());
    assert(!target.notify_from_isr(&higher_priority_task_woken));
    assert(higher_priority_task_woken == pdFALSE);

    target.publish(g_expected_handle);
    assert(target.notify());
    assert(target.notify_from_isr(&higher_priority_task_woken));
    assert(higher_priority_task_woken == pdTRUE);

    constexpr int kThreadNotifications = 2000;
    std::thread notifier([&target]() {
        for (int i = 0; i < kThreadNotifications; ++i) {
            assert(target.notify());
        }
    });
    notifier.join();

    assert(g_task_notifications.load() == kThreadNotifications + 1);
    assert(g_isr_notifications.load() == 1);
    return 0;
}
