// 定义常驻任务标识、创建结果和 OTA 启动健康判定的纯策略。
#pragma once

#include <stdint.h>

enum class RegularAppTaskId : uint8_t {
    kNetworkSync = 0,
    kOta,
    kHousekeeping,
    kUi,
    kButton,
    kAlarm,
    kPomodoro,
    kCount,
};

constexpr uint32_t regular_app_task_bit(RegularAppTaskId id)
{
    return uint32_t{1} << static_cast<uint8_t>(id);
}

inline constexpr uint32_t kAllRegularAppTaskBits =
    (uint32_t{1} << static_cast<uint8_t>(RegularAppTaskId::kCount)) - 1U;

struct AppTaskStartupResult {
    uint32_t created_mask = 0;
    uint32_t failed_mask = 0;
};

constexpr uint32_t regular_app_task_missing_ready_mask(
    const AppTaskStartupResult &startup,
    uint32_t ready_mask)
{
    return kAllRegularAppTaskBits &
           ~(startup.created_mask & ready_mask);
}

constexpr bool regular_app_tasks_healthy_for_ota(
    const AppTaskStartupResult &startup,
    uint32_t ready_mask)
{
    return startup.failed_mask == 0 &&
           regular_app_task_missing_ready_mask(startup, ready_mask) == 0;
}

static_assert(static_cast<uint8_t>(RegularAppTaskId::kCount) > 0,
              "regular task catalog must not be empty");
static_assert(static_cast<uint8_t>(RegularAppTaskId::kCount) < 32,
              "regular task readiness mask must fit uint32_t");
