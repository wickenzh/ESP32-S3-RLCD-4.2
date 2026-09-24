// 提供常驻任务启动 ready 掩码和开机阶段有界等待。
#pragma once

#include "app_task_startup_policy.h"

#include <freertos/FreeRTOS.h>

void regular_app_task_readiness_begin();
void regular_app_task_mark_ready(RegularAppTaskId id);
uint32_t regular_app_task_ready_mask();
uint32_t wait_for_regular_app_tasks_ready(uint32_t expected_mask,
                                          TickType_t timeout);
