// 验证主备 Wi-Fi 每槽有限重试、单次切换和双槽失败终止规则。
#include "wifi_failover_policy.h"

#include <assert.h>

int main()
{
    static_assert(kWifiPrimaryAttemptWindowMs == 12000);
    assert(wifi_credential_slot_valid(WifiCredentialSlot::kSlotA));
    assert(wifi_credential_slot_valid(WifiCredentialSlot::kSlotB));
    assert(!wifi_credential_slot_valid(
        static_cast<WifiCredentialSlot>(2)));

    WifiFailoverState state =
        wifi_failover_begin(WifiCredentialSlot::kSlotA, true);
    assert(state.current_slot == WifiCredentialSlot::kSlotA);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kRetryCurrent);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kRetryCurrent);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kSwitchAlternate);
    assert(state.current_slot == WifiCredentialSlot::kSlotB);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kRetryCurrent);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kRetryCurrent);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kExhausted);
    assert(state.current_slot == WifiCredentialSlot::kSlotB);

    state = wifi_failover_begin(WifiCredentialSlot::kSlotB, false);
    assert(wifi_failover_record_failure(&state, true) ==
           WifiFailoverAction::kIgnore);
    assert(state.current_failures == 0);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kRetryCurrent);
    wifi_failover_record_connected(&state);
    assert(state.current_failures == 0);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kRetryCurrent);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kRetryCurrent);
    assert(wifi_failover_record_failure(&state, false) ==
           WifiFailoverAction::kExhausted);

    assert(wifi_failover_record_failure(nullptr, false) ==
           WifiFailoverAction::kIgnore);

    state = wifi_failover_begin(WifiCredentialSlot::kSlotA, true);
    assert(wifi_failover_force_switch(&state) ==
           WifiFailoverAction::kSwitchAlternate);
    assert(state.current_slot == WifiCredentialSlot::kSlotB);
    assert(wifi_failover_force_switch(&state) ==
           WifiFailoverAction::kExhausted);
    state = wifi_failover_begin(WifiCredentialSlot::kSlotA, false);
    assert(wifi_failover_force_switch(&state) ==
           WifiFailoverAction::kExhausted);
    assert(wifi_failover_force_switch(nullptr) ==
           WifiFailoverAction::kIgnore);
    return 0;
}
