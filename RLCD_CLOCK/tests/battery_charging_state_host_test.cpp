// 验证电池充电趋势状态机的进入、动画完成、回落退出和 Tick 回绕。
#include "battery_charging_state.h"
#include "battery_policy.h"
#include "battery_adc_filter.h"

#include <assert.h>
#include <stdint.h>

namespace {
static_assert(kLowBatteryEnterPercent == 10);
static_assert(kLowBatteryExitPercent == 13);
static_assert(kBatteryChargingRiseVoltage == 0.035f);
static_assert(kBatteryChargingStopVoltage == 0.006f);
static_assert(kBatteryChargingRiseSamples == 3);
static_assert(kBatteryChargingStopSamples == 5);
static_assert(kBatteryChargingAnimationStopPercent == 96);
static_assert(kBatteryChargingAnimationIdleMs == 10 * 60 * 1000);
static_assert(kBatteryChargingSampleMs == 1000);
static_assert(kBatteryChargeHistoryMinSessionMs == 60 * 1000);
static_assert(kBatteryIdleReadFailureGraceSamples == 1);
static_assert(kBatteryChargingReadFailureGraceSamples == 5);
static_assert(kBatteryReadFailureMaxGraceSamples ==
              kBatteryChargingReadFailureGraceSamples);
static_assert(!battery_charging_requires_fast_sampling(false));
static_assert(battery_charging_requires_fast_sampling(true));
static_assert(!battery_full_charge_history_should_update(false, true, false, true, true));
static_assert(!battery_full_charge_history_should_update(true, false, false, true, true));
static_assert(!battery_full_charge_history_should_update(true, true, false, false, true));
static_assert(!battery_full_charge_history_should_update(true, true, false, true, false));
static_assert(battery_full_charge_history_should_update(true, true, false, true, true));
static_assert(!battery_full_charge_history_should_update(true, true, true, true, true));
static_assert(!battery_read_failure_within_grace(false, 0));
static_assert(battery_read_failure_within_grace(false, 1));
static_assert(!battery_read_failure_within_grace(false, 2));
static_assert(!battery_read_failure_within_grace(true, 0));
static_assert(battery_read_failure_within_grace(true, 1));
static_assert(battery_read_failure_within_grace(
    true,
    kBatteryChargingReadFailureGraceSamples));
static_assert(!battery_read_failure_within_grace(
    true,
    kBatteryChargingReadFailureGraceSamples + 1));

constexpr BatteryChargingPolicy kPolicy = {
    3.0f,
    0.035f,
    0.006f,
    1,
    5,
    96,
    600,
    60,
};

BatteryChargingInput input(float previous,
                           float current,
                           int percent,
                           uint32_t tick)
{
    return {previous, current, percent, tick};
}
} // namespace

int main()
{
    int spikes[5] = {1360, 4095, 1359, 0, 1361};
    assert(battery_adc_median(spikes) == 1360);
    int stable[5] = {1360, 1360, 1360, 1360, 1360};
    assert(battery_adc_median(stable) == 1360);
    BatteryChargingPolicy confirmed = kPolicy;
    confirmed.rise_samples_required = 3;
    confirmed.confirmation_min_ticks = 2;
    confirmed.confirmation_timeout_ticks = 5;
    BatteryChargingTracker pending;
    BatteryChargingState pending_state;
    update_battery_charging_state(input(4.059f, 4.107f, 99, 100), confirmed, &pending, &pending_state);
    assert(!pending_state.charging && pending.rise_samples == 1);
    update_battery_charging_state(input(4.107f, 4.065f, 95, 101), confirmed, &pending, &pending_state);
    assert(!pending_state.charging && pending.rise_samples == 0);
    update_battery_charging_state(input(3.8f, 3.84f, 75, 200), confirmed, &pending, &pending_state);
    update_battery_charging_state(input(3.84f, 3.841f, 75, 201), confirmed, &pending, &pending_state);
    assert(!pending_state.charging);
    update_battery_charging_state(input(3.841f, 3.84f, 75, 202), confirmed, &pending, &pending_state);
    assert(pending_state.charging && pending.session_start_tick == 202);
    assert(!pending_state.animation_complete);
    pending = {}; pending_state = {};
    update_battery_charging_state(input(3.8f, 3.84f, 75, 100), confirmed, &pending, &pending_state);
    update_battery_charging_state(input(3.84f, 3.84f, 75, 106), confirmed, &pending, &pending_state);
    assert(!pending_state.charging && pending.rise_samples == 0);
    pending = {}; pending_state = {};
    update_battery_charging_state(input(3.8f, 3.84f, 75, UINT32_MAX - 1), confirmed, &pending, &pending_state);
    update_battery_charging_state(input(3.84f, 3.84f, 75, UINT32_MAX), confirmed, &pending, &pending_state);
    update_battery_charging_state(input(3.84f, 3.84f, 75, 0), confirmed, &pending, &pending_state);
    assert(pending_state.charging);

    assert(!update_battery_charging_state(input(3.8f, 3.9f, 50, 0),
                                           kPolicy,
                                           nullptr,
                                           nullptr));

    BatteryChargingTracker tracker = {1, 2, 4.0f, true, 10, true, 5, true};
    BatteryChargingState state = {false, true};
    assert(update_battery_charging_state(input(-1.0f, 3.8f, 50, 20),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(!state.charging && !state.animation_complete);
    assert(tracker.rise_samples == 0 && tracker.stop_samples == 0);
    assert(tracker.peak_voltage == 0.0f && !tracker.peak_tick_set);
    assert(!tracker.session_start_tick_set);
    assert(!tracker.session_started_below_full_threshold);

    tracker = {};
    state = {};
    assert(update_battery_charging_state(input(3.80f, 3.82f, 50, 100),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(!state.charging && tracker.rise_samples == 0);
    assert(update_battery_charging_state(input(3.82f, 3.855f, 50, 200),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(state.charging && !state.animation_complete);
    assert(tracker.peak_voltage == 3.855f && tracker.last_peak_tick == 200);
    assert(tracker.session_start_tick_set && tracker.session_start_tick == 200);
    assert(tracker.session_started_below_full_threshold);
    assert(!battery_charging_session_elapsed(tracker, 259, 60));
    assert(battery_charging_session_elapsed(tracker, 260, 60));

    assert(update_battery_charging_state(input(3.855f, 3.90f, 50, 300),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(state.charging && tracker.peak_voltage == 3.90f);
    assert(tracker.last_peak_tick == 300 && tracker.stop_samples == 0);
    assert(update_battery_charging_state(input(3.90f, 3.90f, 96, 301),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(state.charging && state.animation_complete);

    tracker = {};
    state = {};
    assert(update_battery_charging_state(input(3.90f, 3.94f, 96, 100),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(state.charging && !state.animation_complete);
    assert(!tracker.session_started_below_full_threshold);
    assert(update_battery_charging_state(input(3.94f, 3.94f, 96, 699),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(state.charging && !state.animation_complete);
    assert(update_battery_charging_state(input(3.94f, 3.94f, 96, 700),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(state.charging && state.animation_complete);

    tracker = {};
    state = {};
    assert(update_battery_charging_state(input(3.80f, 3.84f, 50, UINT32_MAX - 300U),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(state.charging && !state.animation_complete);
    assert(tracker.session_start_tick_set &&
           tracker.session_start_tick == UINT32_MAX - 300U);
    assert(!battery_charging_session_elapsed(tracker, 298U, 600U));
    assert(battery_charging_session_elapsed(tracker, 299U, 600U));
    assert(update_battery_charging_state(input(3.84f, 3.84f, 50, 298U),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(state.charging && !state.animation_complete);
    assert(update_battery_charging_state(input(3.84f, 3.84f, 50, 299U),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(state.charging && state.animation_complete);

    tracker = {};
    state = {};
    assert(update_battery_charging_state(input(3.80f, 3.84f, 50, 100),
                                         kPolicy,
                                         &tracker,
                                         &state));
    for (int i = 0; i < 4; ++i) {
        assert(update_battery_charging_state(input(3.84f, 3.833f, 50, 101 + i),
                                             kPolicy,
                                             &tracker,
                                             &state));
        assert(state.charging);
    }
    assert(tracker.stop_samples == 4);
    assert(update_battery_charging_state(input(3.84f, 3.833f, 50, 105),
                                         kPolicy,
                                         &tracker,
                                         &state));
    assert(!state.charging && !state.animation_complete);
    assert(tracker.rise_samples == 0 && tracker.stop_samples == 0);
    assert(tracker.peak_voltage == 0.0f && !tracker.peak_tick_set);
    assert(!tracker.session_start_tick_set);
    assert(!tracker.session_started_below_full_threshold);
    return 0;
}
