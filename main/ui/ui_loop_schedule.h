// 声明 UI 主循环秒/分钟边界和最短轮询延迟的纯计算接口。
#pragma once

#include <stddef.h>
#include <stdint.h>

inline constexpr uint32_t kUiLoopRadioPollMs = 1000;
inline constexpr uint32_t kUiLoopBoundaryWakeSlackMs = 5;

uint32_t ui_next_second_delay_ms(int64_t monotonic_us);
uint32_t ui_next_minute_delay_ms(int local_second);
uint32_t ui_pomodoro_boundary_delay_ms(uint32_t boundary_ms);
uint32_t ui_nonzero_delay_ticks(uint32_t ticks);
uint32_t ui_shortest_delay_ticks(const uint32_t *candidates, size_t count);
