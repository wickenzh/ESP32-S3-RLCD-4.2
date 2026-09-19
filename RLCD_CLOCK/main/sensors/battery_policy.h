// 统一定义低电量、充电识别、动画停止和充电快速采样策略。
#pragma once

inline constexpr int kLowBatteryEnterPercent = 10;
inline constexpr int kLowBatteryExitPercent = 13;
inline constexpr float kBatteryChargingRiseVoltage = 0.035f;
inline constexpr float kBatteryChargingStopVoltage = 0.006f;
inline constexpr int kBatteryChargingRiseSamples = 3;
inline constexpr int kBatteryChargingConfirmMs = 2000;
inline constexpr int kBatteryChargingConfirmTimeoutMs = 5000;
inline constexpr int kBatteryChargingStopSamples = 5;
inline constexpr int kBatteryChargingAnimationStopPercent = 96;
inline constexpr int kBatteryChargingAnimationIdleMs = 10 * 60 * 1000;
inline constexpr int kBatteryChargingSampleMs = 1000;
inline constexpr int kBatteryChargeHistoryMinSessionMs = 60 * 1000;
inline constexpr int kBatteryIdleReadFailureGraceSamples = 1;
inline constexpr int kBatteryChargingReadFailureGraceSamples = 5;
inline constexpr int kBatteryReadFailureMaxGraceSamples =
    kBatteryChargingReadFailureGraceSamples > kBatteryIdleReadFailureGraceSamples
        ? kBatteryChargingReadFailureGraceSamples
        : kBatteryIdleReadFailureGraceSamples;

constexpr bool battery_charging_requires_fast_sampling(bool charging)
{
    return charging;
}

constexpr bool battery_full_charge_history_should_update(
    bool is_charging,
    bool session_started_below_full_threshold,
    bool session_already_recorded,
    bool at_full_threshold,
    bool session_elapsed)
{
    return is_charging &&
           session_started_below_full_threshold &&
           !session_already_recorded &&
           at_full_threshold &&
           session_elapsed;
}

constexpr int battery_read_failure_grace_samples(bool charging)
{
    return charging
               ? kBatteryChargingReadFailureGraceSamples
               : kBatteryIdleReadFailureGraceSamples;
}

constexpr bool battery_read_failure_within_grace(bool charging,
                                                  int consecutive_failures)
{
    return consecutive_failures > 0 &&
           consecutive_failures <= battery_read_failure_grace_samples(charging);
}

static_assert(kLowBatteryEnterPercent >= 0,
              "low-battery entry threshold must be non-negative");
static_assert(kLowBatteryExitPercent > kLowBatteryEnterPercent,
              "low-battery exit threshold must exceed entry threshold");
static_assert(kBatteryChargingRiseVoltage > kBatteryChargingStopVoltage,
              "charging rise threshold must exceed stop threshold");
static_assert(kBatteryChargingRiseSamples > 0,
              "charging detection must require a rising sample");
static_assert(kBatteryChargingConfirmMs >= 2 * kBatteryChargingSampleMs &&
                  kBatteryChargingConfirmTimeoutMs > kBatteryChargingConfirmMs,
              "charging confirmation must span fast samples and remain bounded");
static_assert(kBatteryChargingStopSamples > 0,
              "charging clear must require a confirming sample");
static_assert(kBatteryChargingAnimationStopPercent > kLowBatteryExitPercent &&
                  kBatteryChargingAnimationStopPercent <= 100,
              "charging animation stop threshold must be a valid percentage");
static_assert(kBatteryChargingAnimationIdleMs > 0,
              "charging animation idle timeout must be positive");
static_assert(kBatteryChargingSampleMs > 0,
              "charging sample interval must be positive");
static_assert(kBatteryChargeHistoryMinSessionMs > kBatteryChargingSampleMs,
              "charge history must reject short voltage-recovery sessions");
static_assert(kBatteryIdleReadFailureGraceSamples > 0,
              "idle ADC read failure grace must be positive");
static_assert(kBatteryChargingReadFailureGraceSamples > 0,
              "charging ADC read failure grace must be positive");
static_assert(kBatteryReadFailureMaxGraceSamples >=
                  kBatteryIdleReadFailureGraceSamples &&
                  kBatteryReadFailureMaxGraceSamples >=
                      kBatteryChargingReadFailureGraceSamples,
              "battery ADC failure counter must cover every grace window");
