// 提供运行健康统计使用的纯时间与低水位策略。
#pragma once

#include <stdint.h>

constexpr uint64_t runtime_health_wifi_total_us(bool radio_on,
                                                int64_t radio_started_us,
                                                int64_t now_us,
                                                uint64_t completed_us)
{
    return radio_on && radio_started_us >= 0 && now_us > radio_started_us
               ? completed_us + static_cast<uint64_t>(now_us - radio_started_us)
               : completed_us;
}

constexpr uint32_t runtime_health_lower_value(uint32_t previous,
                                              uint32_t current)
{
    return current < previous ? current : previous;
}

constexpr bool runtime_health_periodic_due(int64_t now_us,
                                           int64_t next_due_us)
{
    return next_due_us > 0 && now_us >= next_due_us;
}
