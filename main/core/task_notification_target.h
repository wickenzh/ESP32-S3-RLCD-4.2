// 以极短临界区安全发布常驻任务目标，并统一普通任务与 ISR 通知。
#pragma once

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class TaskNotificationTarget {
public:
    void publish(TaskHandle_t handle) noexcept
    {
        portENTER_CRITICAL(&mux_);
        handle_ = handle;
        portEXIT_CRITICAL(&mux_);
    }

    bool notify() const noexcept
    {
        portENTER_CRITICAL(&mux_);
        TaskHandle_t handle = handle_;
        portEXIT_CRITICAL(&mux_);
        if (!handle) {
            return false;
        }
        xTaskNotifyGive(handle);
        return true;
    }

    bool IRAM_ATTR notify_from_isr(BaseType_t *higher_priority_task_woken) const noexcept
    {
        portENTER_CRITICAL_ISR(&mux_);
        TaskHandle_t handle = handle_;
        portEXIT_CRITICAL_ISR(&mux_);
        if (!handle) {
            return false;
        }
        vTaskNotifyGiveFromISR(handle, higher_priority_task_woken);
        return true;
    }

private:
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    TaskHandle_t handle_ = nullptr;
};
