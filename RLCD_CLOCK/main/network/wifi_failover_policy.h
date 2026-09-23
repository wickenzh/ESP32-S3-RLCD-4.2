// 定义单次 Wi-Fi 射频会话内主备槽位的有界重试与切换策略。
#pragma once

#include <stdint.h>

enum class WifiCredentialSlot : uint8_t {
    kSlotA = 0,
    kSlotB = 1,
};

enum class WifiFailoverAction : uint8_t {
    kIgnore,
    kRetryCurrent,
    kSwitchAlternate,
    kExhausted,
};

struct WifiFailoverState {
    WifiCredentialSlot current_slot = WifiCredentialSlot::kSlotA;
    uint8_t attempted_slot_mask = 0;
    uint8_t current_failures = 0;
    bool alternate_configured = false;
};

inline constexpr uint8_t kWifiFailoverFailuresPerSlot = 3;
inline constexpr uint32_t kWifiPrimaryAttemptWindowMs = 12000;

constexpr bool wifi_credential_slot_valid(WifiCredentialSlot slot)
{
    return slot == WifiCredentialSlot::kSlotA ||
           slot == WifiCredentialSlot::kSlotB;
}

constexpr uint8_t wifi_credential_slot_bit(WifiCredentialSlot slot)
{
    return static_cast<uint8_t>(1U << static_cast<uint8_t>(slot));
}

constexpr WifiCredentialSlot wifi_alternate_credential_slot(
    WifiCredentialSlot slot)
{
    return slot == WifiCredentialSlot::kSlotA
               ? WifiCredentialSlot::kSlotB
               : WifiCredentialSlot::kSlotA;
}

constexpr WifiFailoverState wifi_failover_begin(
    WifiCredentialSlot preferred_slot,
    bool alternate_configured)
{
    return {
        preferred_slot,
        wifi_credential_slot_bit(preferred_slot),
        0,
        alternate_configured,
    };
}

constexpr WifiFailoverAction wifi_failover_record_failure(
    WifiFailoverState *state,
    bool deliberate_disconnect)
{
    if (!state || deliberate_disconnect) {
        return WifiFailoverAction::kIgnore;
    }
    if (state->current_failures < UINT8_MAX) {
        ++state->current_failures;
    }
    if (state->current_failures < kWifiFailoverFailuresPerSlot) {
        return WifiFailoverAction::kRetryCurrent;
    }
    const WifiCredentialSlot alternate =
        wifi_alternate_credential_slot(state->current_slot);
    const uint8_t alternate_bit = wifi_credential_slot_bit(alternate);
    if (!state->alternate_configured ||
        (state->attempted_slot_mask & alternate_bit) != 0) {
        return WifiFailoverAction::kExhausted;
    }
    state->current_slot = alternate;
    state->attempted_slot_mask = static_cast<uint8_t>(
        state->attempted_slot_mask | alternate_bit);
    state->current_failures = 0;
    state->alternate_configured = true;
    return WifiFailoverAction::kSwitchAlternate;
}

constexpr WifiFailoverAction wifi_failover_force_switch(
    WifiFailoverState *state)
{
    if (!state) {
        return WifiFailoverAction::kIgnore;
    }
    const WifiCredentialSlot alternate =
        wifi_alternate_credential_slot(state->current_slot);
    const uint8_t alternate_bit = wifi_credential_slot_bit(alternate);
    if (!state->alternate_configured ||
        (state->attempted_slot_mask & alternate_bit) != 0) {
        return WifiFailoverAction::kExhausted;
    }
    state->current_slot = alternate;
    state->attempted_slot_mask = static_cast<uint8_t>(
        state->attempted_slot_mask | alternate_bit);
    state->current_failures = 0;
    state->alternate_configured = true;
    return WifiFailoverAction::kSwitchAlternate;
}

constexpr void wifi_failover_record_connected(WifiFailoverState *state)
{
    if (state) {
        state->current_failures = 0;
    }
}
