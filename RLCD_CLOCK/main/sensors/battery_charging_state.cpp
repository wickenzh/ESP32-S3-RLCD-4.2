// 实现电池充电进入、退出和动画完成的纯趋势状态机。
#include "battery_charging_state.h"

#include "app_tick_time.h"

namespace {
bool previous_voltage_valid(float voltage, const BatteryChargingPolicy &policy)
{
    return voltage >= policy.valid_previous_voltage_min;
}

bool voltage_dropped_from_peak(float voltage,
                               float peak_voltage,
                               const BatteryChargingPolicy &policy)
{
    return peak_voltage > 0.0f && voltage <= peak_voltage - policy.stop_voltage;
}

bool charging_should_stop(float voltage,
                          const BatteryChargingTracker &tracker,
                          const BatteryChargingPolicy &policy)
{
    return voltage_dropped_from_peak(voltage, tracker.peak_voltage, policy) &&
           tracker.stop_samples >= policy.stop_samples_required;
}

bool charging_animation_idle_elapsed(const BatteryChargingTracker &tracker,
                                     uint32_t now_tick,
                                     const BatteryChargingPolicy &policy)
{
    return tracker.peak_tick_set &&
           app_tick_interval_elapsed(now_tick,
                                     tracker.last_peak_tick,
                                     policy.animation_idle_ticks);
}

bool charging_session_is_meaningful(const BatteryChargingTracker &tracker,
                                    uint32_t now_tick,
                                    const BatteryChargingPolicy &policy)
{
    return battery_charging_session_elapsed(tracker,
                                            now_tick,
                                            policy.full_charge_min_ticks);
}
} // namespace

void reset_battery_charging_tracker(BatteryChargingTracker *tracker)
{
    if (tracker) {
        *tracker = {};
    }
}

bool battery_charging_session_elapsed(const BatteryChargingTracker &tracker,
                                      uint32_t now_tick,
                                      uint32_t minimum_ticks)
{
    return tracker.session_start_tick_set &&
           app_tick_interval_elapsed(now_tick,
                                     tracker.session_start_tick,
                                     minimum_ticks);
}

bool update_battery_charging_state(const BatteryChargingInput &input,
                                   const BatteryChargingPolicy &policy,
                                   BatteryChargingTracker *tracker,
                                   BatteryChargingState *state)
{
    if (!tracker || !state) {
        return false;
    }
    if (!previous_voltage_valid(input.previous_voltage, policy)) {
        reset_battery_charging_tracker(tracker);
        state->animation_complete = false;
        return true;
    }

    float delta = input.current_voltage - input.previous_voltage;
    if (!state->charging) {
        if (tracker->rise_samples > 0 &&
            ((policy.confirmation_timeout_ticks > 0 &&
              app_tick_interval_elapsed(input.now_tick, tracker->candidate_tick,
                                        policy.confirmation_timeout_ticks)) ||
             input.current_voltage < tracker->candidate_voltage - policy.stop_voltage)) {
            tracker->rise_samples = 0;
        }
        if (tracker->rise_samples > 0) {
            if (tracker->rise_samples < policy.rise_samples_required) {
                ++tracker->rise_samples;
            }
        } else if (delta >= policy.rise_voltage) {
            tracker->rise_samples = 1;
            tracker->candidate_voltage = input.current_voltage;
            tracker->candidate_tick = input.now_tick;
        }
    }

    if (state->charging) {
        if (input.current_voltage > tracker->peak_voltage) {
            tracker->peak_voltage = input.current_voltage;
            tracker->last_peak_tick = input.now_tick;
            tracker->peak_tick_set = true;
            tracker->stop_samples = 0;
        } else if (voltage_dropped_from_peak(input.current_voltage,
                                             tracker->peak_voltage,
                                             policy)) {
            if (tracker->stop_samples < policy.stop_samples_required) {
                ++tracker->stop_samples;
            }
        } else {
            tracker->stop_samples = 0;
        }

        if (charging_should_stop(input.current_voltage, *tracker, policy)) {
            state->charging = false;
            state->animation_complete = false;
            reset_battery_charging_tracker(tracker);
        }
    } else if (tracker->rise_samples >= policy.rise_samples_required &&
               app_tick_interval_elapsed(input.now_tick, tracker->candidate_tick,
                                         policy.confirmation_min_ticks)) {
        state->charging = true;
        state->animation_complete = false;
        tracker->stop_samples = 0;
        tracker->peak_voltage = input.current_voltage;
        tracker->last_peak_tick = input.now_tick;
        tracker->peak_tick_set = true;
        tracker->session_start_tick = input.now_tick;
        tracker->session_start_tick_set = true;
        tracker->session_started_below_full_threshold =
            input.percent < policy.animation_stop_percent;
    }

    bool charging_idle = state->charging &&
                         charging_animation_idle_elapsed(*tracker,
                                                         input.now_tick,
                                                         policy);
    bool charging_threshold_reached =
        state->charging &&
        tracker->session_started_below_full_threshold &&
        input.percent >= policy.animation_stop_percent;
    if (state->charging &&
        !state->animation_complete &&
        charging_session_is_meaningful(*tracker, input.now_tick, policy) &&
        (charging_threshold_reached || charging_idle)) {
        state->animation_complete = true;
    }
    return true;
}
