// 定义默认关闭的 12 小时 QWeather 首请求超时 A/B 分组策略。
#pragma once

#include "http_session_policy.h"

#include <stdint.h>

#ifdef WEATHER_CLOCK_QWEATHER_TIMEOUT_AB_TEST
#include <atomic>
inline std::atomic<int> qweather_timeout_ab_last_slot{-1};
#endif

#ifndef WEATHER_CLOCK_QWEATHER_TIMEOUT_AB_SLOT_MINUTES
#define WEATHER_CLOCK_QWEATHER_TIMEOUT_AB_SLOT_MINUTES 20
#endif

inline constexpr int64_t kQweatherTimeoutAbSlotDurationUs =
    static_cast<int64_t>(WEATHER_CLOCK_QWEATHER_TIMEOUT_AB_SLOT_MINUTES) *
    60 * 1000000;
inline constexpr int64_t kQweatherTimeoutAbDurationUs =
    12LL * 60 * 60 * 1000000;

struct QweatherTimeoutAbDecision {
    int slot = -1;
    bool candidate = false;
    int first_request_timeout_ms = 0;
};

constexpr int qweather_timeout_ab_slot(int64_t uptime_us)
{
    return uptime_us >= 0 && uptime_us < kQweatherTimeoutAbDurationUs
               ? static_cast<int>(uptime_us /
                                  kQweatherTimeoutAbSlotDurationUs)
               : -1;
}

constexpr QweatherTimeoutAbDecision qweather_timeout_ab_decision(
    int64_t uptime_us,
    bool eligible)
{
    const int slot = qweather_timeout_ab_slot(uptime_us);
    const bool candidate = slot >= 0 && (slot % 2) != 0;
    return {
        slot,
        candidate,
        eligible && candidate ? kQweatherTimeoutAbCandidateMs : 0,
    };
}

static_assert(WEATHER_CLOCK_QWEATHER_TIMEOUT_AB_SLOT_MINUTES > 0,
              "QWeather timeout A/B slot must be positive");
static_assert(kQweatherTimeoutAbDurationUs %
                      kQweatherTimeoutAbSlotDurationUs ==
                  0,
              "QWeather timeout A/B duration must contain whole slots");
