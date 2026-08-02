// 实现 UI 主循环轮询边界计算，不访问 LVGL、页面状态或硬件。
#include "ui_loop_schedule.h"

namespace {
constexpr int64_t kUsPerSecond = 1000000;
constexpr uint32_t kMsPerSecond = 1000;
constexpr int kSecondsPerMinute = 60;
constexpr uint32_t kNextSecondDelayMinMs = 10;
constexpr uint32_t kNextSecondDelayMaxMs = kMsPerSecond + kUiLoopBoundaryWakeSlackMs;

static_assert(kUsPerSecond == 1000LL * kMsPerSecond,
              "UI microsecond and millisecond constants must stay consistent");
static_assert(kUiLoopRadioPollMs > 0 && kUiLoopRadioPollMs <= kMsPerSecond,
              "radio poll interval must stay within one second");
static_assert(kUiLoopBoundaryWakeSlackMs <= kMsPerSecond,
              "UI boundary wake slack must stay within one second");
static_assert(kNextSecondDelayMinMs > 0,
              "next-second delay minimum must be positive");
static_assert(kNextSecondDelayMaxMs >= kMsPerSecond,
              "next-second delay maximum must cover one second");
} // namespace

uint32_t ui_next_second_delay_ms(int64_t monotonic_us)
{
    int64_t second_offset_us = monotonic_us % kUsPerSecond;
    int64_t until_next_us = kUsPerSecond - second_offset_us;
    uint32_t delay_ms = static_cast<uint32_t>(until_next_us / kMsPerSecond) +
                        kUiLoopBoundaryWakeSlackMs;
    if (delay_ms < kNextSecondDelayMinMs) {
        return kNextSecondDelayMinMs;
    }
    return delay_ms > kNextSecondDelayMaxMs ? kNextSecondDelayMaxMs : delay_ms;
}

uint32_t ui_next_minute_delay_ms(int local_second)
{
    int seconds_to_next = kSecondsPerMinute - local_second;
    if (seconds_to_next <= 0 || seconds_to_next > kSecondsPerMinute) {
        seconds_to_next = kSecondsPerMinute;
    }
    return static_cast<uint32_t>(seconds_to_next) * kMsPerSecond +
           kUiLoopBoundaryWakeSlackMs;
}

uint32_t ui_pomodoro_boundary_delay_ms(uint32_t boundary_ms)
{
    if (boundary_ms == 0) {
        return 0;
    }
    return boundary_ms + kUiLoopBoundaryWakeSlackMs;
}

uint32_t ui_nonzero_delay_ticks(uint32_t ticks)
{
    return ticks == 0 ? 1 : ticks;
}

uint32_t ui_shortest_delay_ticks(const uint32_t *candidates, size_t count)
{
    if (!candidates || count == 0) {
        return 0;
    }
    uint32_t shortest = candidates[0];
    for (size_t i = 1; i < count; ++i) {
        if (candidates[i] > 0 && (shortest == 0 || candidates[i] < shortest)) {
            shortest = candidates[i];
        }
    }
    return shortest;
}
