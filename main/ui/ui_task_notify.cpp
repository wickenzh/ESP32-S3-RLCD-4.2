// 集中维护 UI 任务句柄并安全转发跨核心刷新通知。
#include "ui_task_notify.h"

#include "task_notification_target.h"

namespace {
TaskNotificationTarget s_ui_task_target;
} // namespace

void register_ui_task_handle(TaskHandle_t handle)
{
    s_ui_task_target.publish(handle);
}

void notify_ui_task()
{
    (void)s_ui_task_target.notify();
}
