// 验证网络同步任务各等待入口使用正确事件位、清位语义和超时。
#include "network_sync_wait.h"

#include "app_event_group.h"
#include "network_sync_requests.h"
#include "network_sync_schedule.h"

#include <cassert>
#include <cstdint>

namespace {

struct WaitCall {
    EventBits_t bits = 0;
    BaseType_t clear_on_exit = pdFALSE;
    BaseType_t wait_for_all = pdFALSE;
    TickType_t timeout = 0;
    int count = 0;
};

WaitCall s_wait_call;
EventBits_t s_wait_result = 0;
int64_t s_now_us = 0;
int64_t s_event_elapsed_us = 0;
bool s_clear_wait_result_after_event = false;
bool s_continuation_allowed = true;
bool s_block_continuation_on_wait = false;
bool s_runtime_changed = false;
bool s_request_canceled = false;
EventBits_t s_current_bits = 0;
int s_clear_bits_count = 0;

constexpr EventBits_t kExpectedSyncWakeBits = kProvisioningSyncBit |
                                               kManualNtpSyncBit |
                                               kManualWeatherSyncBit |
                                               kManualSayingSyncBit |
                                               kVisibleWeatherSyncBit |
                                               kVisibleSayingSyncBit |
                                               kNetworkDiagBit |
                                               kNetworkStateChangedBit |
                                               kSetupPortalStartBit;
constexpr EventBits_t kExpectedStaggerWakeBits = kProvisioningSyncBit |
                                                 kManualNtpSyncBit |
                                                 kManualWeatherSyncBit |
                                                 kManualSayingSyncBit |
                                                 kNetworkDiagBit |
                                                 kNetworkStateChangedBit |
                                                 kSetupPortalStartBit;

void reset_wait_call()
{
    s_wait_call = {};
    s_wait_result = 0;
    s_now_us = 0;
    s_event_elapsed_us = 0;
    s_clear_wait_result_after_event = false;
    s_continuation_allowed = true;
    s_block_continuation_on_wait = false;
    s_runtime_changed = false;
    s_request_canceled = false;
    s_current_bits = 0;
    s_clear_bits_count = 0;
}

void assert_wait(EventBits_t bits,
                 BaseType_t clear_on_exit,
                 TickType_t timeout)
{
    assert(s_wait_call.count == 1);
    assert(s_wait_call.bits == bits);
    assert(s_wait_call.clear_on_exit == clear_on_exit);
    assert(s_wait_call.wait_for_all == pdFALSE);
    assert(s_wait_call.timeout == timeout);
}

} // namespace

EventBits_t app_event_group_wait_bits(EventBits_t bits,
                                      BaseType_t clear_on_exit,
                                      BaseType_t wait_for_all,
                                      TickType_t timeout)
{
    s_wait_call.bits = bits;
    s_wait_call.clear_on_exit = clear_on_exit;
    s_wait_call.wait_for_all = wait_for_all;
    s_wait_call.timeout = timeout;
    ++s_wait_call.count;
    const EventBits_t result = s_wait_result & bits;
    s_current_bits |= result;
    if (result == 0) {
        s_now_us += static_cast<int64_t>(timeout) * 1000;
    } else {
        s_now_us += s_event_elapsed_us;
        if (s_block_continuation_on_wait) {
            s_continuation_allowed = false;
        }
        if (s_clear_wait_result_after_event) {
            s_wait_result = 0;
        }
    }
    return result;
}

EventBits_t app_event_group_get_bits()
{
    return s_current_bits;
}

EventBits_t app_event_group_clear_bits(EventBits_t bits)
{
    const EventBits_t previous_bits = s_current_bits;
    s_current_bits &= ~bits;
    s_wait_result &= ~bits;
    ++s_clear_bits_count;
    return previous_bits;
}

int64_t esp_timer_get_time()
{
    return s_now_us;
}

bool network_sync_continuation_allowed()
{
    return s_continuation_allowed;
}

NetworkSyncAvailability capture_network_runtime_availability()
{
    return {};
}

bool network_sync_start_context_changed(
    const NetworkSyncAvailability &,
    const NetworkSyncAvailability &)
{
    return s_runtime_changed;
}

bool network_sync_request_snapshot_still_current(
    const NetworkSyncRequestSnapshot &)
{
    return !s_request_canceled;
}

int main()
{
    reset_wait_call();
    wait_for_network_sync_event(4321);
    assert_wait(kExpectedSyncWakeBits, pdFALSE, pdMS_TO_TICKS(4321));

    reset_wait_call();
    s_wait_result = kVisibleWeatherSyncBit | kVisibleSayingSyncBit;
    wait_for_network_stagger_interrupt(8000);
    assert_wait(kExpectedStaggerWakeBits, pdFALSE, pdMS_TO_TICKS(8000));
    assert(s_now_us == 8000000);

    reset_wait_call();
    s_wait_result = kManualWeatherSyncBit;
    wait_for_network_stagger_interrupt(8000);
    assert_wait(kExpectedStaggerWakeBits, pdFALSE, pdMS_TO_TICKS(8000));
    assert(s_now_us == 0);

    static_assert(setup_portal_retry_delay_ms(0) == 0);
    static_assert(setup_portal_retry_delay_ms(1) == 1000);
    static_assert(setup_portal_retry_delay_ms(2) == 2000);
    static_assert(setup_portal_retry_delay_ms(3) == 4000);
    static_assert(setup_portal_retry_delay_ms(5) == 16000);
    static_assert(setup_portal_retry_delay_ms(6) == 30000);
    static_assert(setup_portal_retry_delay_ms(32) == 30000);
    static_assert(setup_portal_retry_delay_ms(UINT8_MAX) == 30000);

    reset_wait_call();
    wait_for_setup_portal_retry(1);
    assert_wait(kNetworkStateChangedBit,
                pdTRUE,
                pdMS_TO_TICKS(1000));

    reset_wait_call();
    wait_for_setup_portal_retry(6);
    assert_wait(kNetworkStateChangedBit,
                pdTRUE,
                pdMS_TO_TICKS(30000));

    reset_wait_call();
    s_wait_result = kManualWeatherSyncBit |
                    kVisibleSayingSyncBit |
                    kNetworkDiagBit |
                    kSetupPortalStartBit;
    wait_for_setup_portal_retry(2);
    assert_wait(kNetworkStateChangedBit,
                pdTRUE,
                pdMS_TO_TICKS(2000));
    assert(s_now_us == 2000000);

    reset_wait_call();
    s_wait_result = kNetworkStateChangedBit | kManualWeatherSyncBit;
    s_event_elapsed_us = 25000;
    wait_for_setup_portal_retry(2);
    assert_wait(kNetworkStateChangedBit,
                pdTRUE,
                pdMS_TO_TICKS(2000));
    assert(s_now_us == 25000);

    reset_wait_call();
    wait_for_active_setup_portal_request();
    assert_wait(kProvisioningSyncBit | kNetworkStateChangedBit,
                pdFALSE,
                portMAX_DELAY);

    reset_wait_call();
    wait_for_network_runtime_request();
    assert_wait(kExpectedSyncWakeBits, pdFALSE, portMAX_DELAY);

    reset_wait_call();
    wait_for_ota_network_block_change();
    assert_wait(kNetworkStateChangedBit, pdTRUE, portMAX_DELAY);

    reset_wait_call();
    assert(wait_for_network_sync_settle(250));
    assert_wait(kNetworkStateChangedBit, pdTRUE, pdMS_TO_TICKS(250));

    reset_wait_call();
    s_continuation_allowed = false;
    assert(!wait_for_network_sync_settle(250));
    assert(s_wait_call.count == 0);

    reset_wait_call();
    s_wait_result = kNetworkStateChangedBit;
    s_block_continuation_on_wait = true;
    assert(!wait_for_network_sync_settle(300));
    assert_wait(kNetworkStateChangedBit, pdTRUE, pdMS_TO_TICKS(300));

    reset_wait_call();
    s_wait_result = kNetworkStateChangedBit;
    s_event_elapsed_us = 40000;
    s_clear_wait_result_after_event = true;
    assert(wait_for_network_sync_settle(250));
    assert(s_wait_call.count == 2);
    assert(s_wait_call.bits == kNetworkStateChangedBit);
    assert(s_wait_call.clear_on_exit == pdTRUE);
    assert(s_wait_call.timeout == pdMS_TO_TICKS(210));

    const NetworkSyncAvailability scheduled_runtime = {};
    const NetworkSyncRequestSnapshot scheduled_requests = [] {
        NetworkSyncRequestSnapshot requests;
        requests.manual_weather = true;
        requests.manual_weather_generation = 1;
        return requests;
    }();

    reset_wait_call();
    s_current_bits = kWifiConnectedBit;
    assert(wait_for_valid_network_sync_connection(
               scheduled_runtime, scheduled_requests, 45000) ==
           NetworkSyncConnectionWaitResult::kConnected);
    assert(s_wait_call.count == 0);

    reset_wait_call();
    s_runtime_changed = true;
    assert(wait_for_valid_network_sync_connection(
               scheduled_runtime, scheduled_requests, 45000) ==
           NetworkSyncConnectionWaitResult::kRuntimeChanged);
    assert(s_wait_call.count == 0);

    reset_wait_call();
    s_request_canceled = true;
    assert(wait_for_valid_network_sync_connection(
               scheduled_runtime, scheduled_requests, 45000) ==
           NetworkSyncConnectionWaitResult::kRuntimeChanged);
    assert(s_wait_call.count == 0);

    reset_wait_call();
    assert(wait_for_valid_network_sync_connection(
               scheduled_runtime, scheduled_requests, 1234) ==
           NetworkSyncConnectionWaitResult::kTimedOut);
    assert_wait(kWifiConnectedBit | kNetworkStateChangedBit,
                pdFALSE,
                pdMS_TO_TICKS(1234));

    reset_wait_call();
    s_wait_result = kNetworkStateChangedBit;
    s_event_elapsed_us = 40000;
    s_clear_wait_result_after_event = true;
    assert(wait_for_valid_network_sync_connection(
               scheduled_runtime, scheduled_requests, 1234) ==
           NetworkSyncConnectionWaitResult::kTimedOut);
    assert(s_wait_call.count == 2);
    assert(s_wait_call.bits == (kWifiConnectedBit | kNetworkStateChangedBit));
    assert(s_wait_call.clear_on_exit == pdFALSE);
    assert(s_wait_call.timeout == pdMS_TO_TICKS(1194));
    assert(s_clear_bits_count == 1);

    reset_wait_call();
    s_wait_result = kWifiConnectedBit | kNetworkStateChangedBit;
    assert(wait_for_valid_network_sync_connection(
               scheduled_runtime, scheduled_requests, 45000) ==
           NetworkSyncConnectionWaitResult::kConnected);
    assert_wait(kWifiConnectedBit | kNetworkStateChangedBit,
                pdFALSE,
                pdMS_TO_TICKS(45000));
    assert(s_clear_bits_count == 1);

    reset_wait_call();
    s_wait_result = kNtpSyncCompletedBit;
    assert(wait_for_ntp_sync_completion(30000) ==
           NetworkSyncCompletionWaitResult::kCompleted);
    assert_wait(kNtpSyncCompletedBit | kNetworkStateChangedBit,
                pdTRUE,
                pdMS_TO_TICKS(30000));

    reset_wait_call();
    s_continuation_allowed = false;
    assert(wait_for_ntp_sync_completion(30000) ==
           NetworkSyncCompletionWaitResult::kRuntimeChanged);
    assert(s_wait_call.count == 0);

    reset_wait_call();
    s_wait_result = kNetworkStateChangedBit;
    s_block_continuation_on_wait = true;
    assert(wait_for_ntp_sync_completion(30000) ==
           NetworkSyncCompletionWaitResult::kRuntimeChanged);
    assert_wait(kNtpSyncCompletedBit | kNetworkStateChangedBit,
                pdTRUE,
                pdMS_TO_TICKS(30000));

    reset_wait_call();
    s_wait_result = kNtpSyncCompletedBit | kNetworkStateChangedBit;
    s_block_continuation_on_wait = true;
    assert(wait_for_ntp_sync_completion(30000) ==
           NetworkSyncCompletionWaitResult::kCompleted);

    reset_wait_call();
    assert(wait_for_ntp_sync_completion(30000) ==
           NetworkSyncCompletionWaitResult::kTimedOut);
    assert_wait(kNtpSyncCompletedBit | kNetworkStateChangedBit,
                pdTRUE,
                pdMS_TO_TICKS(30000));

    reset_wait_call();
    s_wait_result = kNetworkStateChangedBit;
    s_event_elapsed_us = 40000;
    s_clear_wait_result_after_event = true;
    assert(wait_for_ntp_sync_completion(30000) ==
           NetworkSyncCompletionWaitResult::kTimedOut);
    assert(s_wait_call.count == 2);
    assert(s_wait_call.bits ==
           (kNtpSyncCompletedBit | kNetworkStateChangedBit));
    assert(s_wait_call.clear_on_exit == pdTRUE);
    assert(s_wait_call.timeout == pdMS_TO_TICKS(29960));

    return 0;
}
