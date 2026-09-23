// 实现常驻网络同步任务的普通、退避和状态变化等待语义。
#include "network_sync_wait.h"

#include "app_event_group.h"
#include "app_tick_time.h"
#include "network_sync_requests.h"
#include "network_sync_runtime.h"
#include "network_sync_schedule.h"

#include <esp_timer.h>

namespace {

constexpr EventBits_t kNetworkSyncWakeBits = kProvisioningSyncBit |
                                             kManualNtpSyncBit |
                                             kManualWeatherSyncBit |
                                             kManualSayingSyncBit |
                                             kVisibleWeatherSyncBit |
                                             kVisibleSayingSyncBit |
                                             kNetworkDiagBit |
                                             kNetworkStateChangedBit |
                                             kSetupPortalStartBit;
constexpr EventBits_t kSetupPortalRetryWakeBits = kNetworkStateChangedBit;
constexpr EventBits_t kNetworkStaggerWakeBits = kProvisioningSyncBit |
                                                kManualNtpSyncBit |
                                                kManualWeatherSyncBit |
                                                kManualSayingSyncBit |
                                                kNetworkDiagBit |
                                                kNetworkStateChangedBit |
                                                kSetupPortalStartBit;
constexpr EventBits_t kActiveSetupPortalWakeBits =
    kProvisioningSyncBit | kNetworkStateChangedBit;
constexpr EventBits_t kNetworkConnectionWakeBits =
    kWifiConnectedBit | kNetworkStateChangedBit;
constexpr EventBits_t kNtpCompletionWakeBits =
    kNtpSyncCompletedBit | kNetworkStateChangedBit;
static_assert((kNetworkSyncWakeBits & kNetworkStateChangedBit) != 0,
              "network sync wait must wake on runtime state changes");
static_assert((kSetupPortalRetryWakeBits & kSetupPortalStartBit) == 0,
              "setup portal retry wait must ignore its pending level bit");
static_assert((kNetworkStaggerWakeBits &
               (kVisibleWeatherSyncBit | kVisibleSayingSyncBit)) == 0,
              "automatic sync bits must not interrupt their own stagger");
static_assert((kSetupPortalRetryWakeBits &
               (kProvisioningSyncBit |
                kManualNtpSyncBit |
                kManualWeatherSyncBit |
                kManualSayingSyncBit |
                kVisibleWeatherSyncBit |
                kVisibleSayingSyncBit |
                kNetworkDiagBit)) == 0,
              "setup portal retry wait must ignore queued level requests");
static_assert((kActiveSetupPortalWakeBits & kNetworkDiagBit) == 0 &&
                  (kActiveSetupPortalWakeBits & kVisibleWeatherSyncBit) == 0,
              "active setup portal wait must ignore ordinary sync requests");
static_assert((kNetworkConnectionWakeBits & kWifiConnectedBit) != 0 &&
                  (kNetworkConnectionWakeBits & kNetworkStateChangedBit) != 0,
              "connection wait must observe success and runtime cancellation");
static_assert((kNtpCompletionWakeBits & kNtpSyncCompletedBit) != 0 &&
                  (kNtpCompletionWakeBits & kNetworkStateChangedBit) != 0,
              "NTP wait must observe completion and runtime cancellation");

bool wait_for_network_runtime_change(uint32_t timeout_ms)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0) {
        timeout_ticks = app_tick_nonzero_delay(timeout_ticks);
    }
    const EventBits_t bits = app_event_group_wait_bits(
        kNetworkStateChangedBit,
        pdTRUE,
        pdFALSE,
        timeout_ticks);
    return (bits & kNetworkStateChangedBit) != 0;
}

} // namespace

void wait_for_network_sync_event(uint32_t timeout_ms)
{
    app_event_group_wait_bits(kNetworkSyncWakeBits,
                              pdFALSE,
                              pdFALSE,
                              pdMS_TO_TICKS(timeout_ms));
}

void wait_for_network_stagger_interrupt(uint32_t timeout_ms)
{
    app_event_group_wait_bits(kNetworkStaggerWakeBits,
                              pdFALSE,
                              pdFALSE,
                              pdMS_TO_TICKS(timeout_ms));
}

void wait_for_setup_portal_retry(uint8_t failure_count)
{
    // All sync requests are level-triggered and may remain queued while portal
    // start/stop is failing. Only a real runtime-state edge may interrupt the
    // bounded retry delay; otherwise pending work would create a busy loop.
    app_event_group_wait_bits(kSetupPortalRetryWakeBits,
                              pdTRUE,
                              pdFALSE,
                              pdMS_TO_TICKS(
                                  setup_portal_retry_delay_ms(failure_count)));
}

void wait_for_active_setup_portal_request()
{
    // Ordinary sync bits remain level-triggered while the AP is serving the
    // setup page. Ignore them here so they cannot spin the task or switch the
    // radio back to STA before provisioning completes.
    app_event_group_wait_bits(kActiveSetupPortalWakeBits,
                              pdFALSE,
                              pdFALSE,
                              portMAX_DELAY);
}

void wait_for_network_runtime_request()
{
    app_event_group_wait_bits(kNetworkSyncWakeBits,
                              pdFALSE,
                              pdFALSE,
                              portMAX_DELAY);
}

void wait_for_ota_network_block_change()
{
    // Pending level-triggered sync bits remain queued while OTA owns HTTPS and
    // Wi-Fi. Wait only for the edge-like runtime-state bit so those requests do
    // not turn the protection branch into a busy loop.
    app_event_group_wait_bits(kNetworkStateChangedBit,
                              pdTRUE,
                              pdFALSE,
                              portMAX_DELAY);
}

bool wait_for_network_sync_settle(uint32_t timeout_ms)
{
    const int64_t settle_deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (network_sync_continuation_allowed()) {
        const int64_t remaining_us = settle_deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return true;
        }
        const uint32_t remaining_ms =
            static_cast<uint32_t>((remaining_us + 999) / 1000);
        if (!wait_for_network_runtime_change(remaining_ms)) {
            return network_sync_continuation_allowed();
        }
    }
    return false;
}

static NetworkSyncConnectionWaitResult wait_for_network_sync_connection(
    uint32_t timeout_ms)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0) {
        timeout_ticks = app_tick_nonzero_delay(timeout_ticks);
    }
    const EventBits_t bits = app_event_group_wait_bits(
        kNetworkConnectionWakeBits,
        pdFALSE,
        pdFALSE,
        timeout_ticks);
    if ((bits & kNetworkStateChangedBit) != 0) {
        return NetworkSyncConnectionWaitResult::kRuntimeChanged;
    }
    if ((bits & kWifiConnectedBit) != 0) {
        return NetworkSyncConnectionWaitResult::kConnected;
    }
    return NetworkSyncConnectionWaitResult::kTimedOut;
}

NetworkSyncConnectionWaitResult wait_for_valid_network_sync_connection(
    const NetworkSyncAvailability &scheduled_runtime,
    const NetworkSyncRequestSnapshot &scheduled_requests,
    uint32_t timeout_ms)
{
    constexpr int64_t kUsPerMs = 1000;
    const int64_t connection_deadline_us =
        esp_timer_get_time() +
        static_cast<int64_t>(timeout_ms) * kUsPerMs;
    for (;;) {
        const NetworkSyncAvailability connected_runtime =
            capture_network_runtime_availability();
        const EventBits_t current_bits = app_event_group_get_bits();
        if (network_sync_start_context_changed(scheduled_runtime,
                                               connected_runtime) ||
            !network_sync_request_snapshot_still_current(
                scheduled_requests)) {
            return NetworkSyncConnectionWaitResult::kRuntimeChanged;
        }
        if ((current_bits & kWifiConnectedBit) != 0) {
            return NetworkSyncConnectionWaitResult::kConnected;
        }

        const int64_t remaining_us =
            connection_deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return NetworkSyncConnectionWaitResult::kTimedOut;
        }
        const uint32_t remaining_ms = static_cast<uint32_t>(
            (remaining_us + kUsPerMs - 1) / kUsPerMs);
        const NetworkSyncConnectionWaitResult connection_wait =
            wait_for_network_sync_connection(remaining_ms);
        if (connection_wait == NetworkSyncConnectionWaitResult::kRuntimeChanged) {
            // The state bit is edge-like and shared by several producers.
            // Consume it, then trust the live context instead of restarting a
            // Wi-Fi session for a stale or no-op notification.
            app_event_group_clear_bits(kNetworkStateChangedBit);
        }

        const NetworkSyncAvailability refreshed_runtime =
            capture_network_runtime_availability();
        const EventBits_t refreshed_bits = app_event_group_get_bits();
        if (network_sync_start_context_changed(scheduled_runtime,
                                               refreshed_runtime) ||
            !network_sync_request_snapshot_still_current(
                scheduled_requests)) {
            return NetworkSyncConnectionWaitResult::kRuntimeChanged;
        }
        if ((refreshed_bits & kWifiConnectedBit) != 0) {
            return NetworkSyncConnectionWaitResult::kConnected;
        }
        if (connection_wait == NetworkSyncConnectionWaitResult::kTimedOut) {
            return NetworkSyncConnectionWaitResult::kTimedOut;
        }
    }
}

NetworkSyncCompletionWaitResult wait_for_ntp_sync_completion(uint32_t timeout_ms)
{
    const int64_t deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    for (;;) {
        if (!network_sync_continuation_allowed()) {
            return NetworkSyncCompletionWaitResult::kRuntimeChanged;
        }
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return NetworkSyncCompletionWaitResult::kTimedOut;
        }
        const uint32_t remaining_ms =
            static_cast<uint32_t>((remaining_us + 999) / 1000);
        const TickType_t remaining_ticks =
            app_tick_nonzero_delay(pdMS_TO_TICKS(remaining_ms));
        const EventBits_t bits = app_event_group_wait_bits(
            kNtpCompletionWakeBits,
            pdTRUE,
            pdFALSE,
            remaining_ticks);
        if ((bits & kNtpSyncCompletedBit) != 0) {
            return NetworkSyncCompletionWaitResult::kCompleted;
        }
        if ((bits & kNetworkStateChangedBit) != 0 &&
            !network_sync_continuation_allowed()) {
            return NetworkSyncCompletionWaitResult::kRuntimeChanged;
        }
        if (bits == 0) {
            return NetworkSyncCompletionWaitResult::kTimedOut;
        }
    }
}
