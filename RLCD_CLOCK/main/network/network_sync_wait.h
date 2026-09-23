// 声明常驻网络同步任务的事件等待策略。
#pragma once

#include <stdint.h>

struct NetworkSyncAvailability;
struct NetworkSyncRequestSnapshot;

enum class NetworkSyncConnectionWaitResult {
    kConnected,
    kRuntimeChanged,
    kTimedOut,
};

enum class NetworkSyncCompletionWaitResult {
    kCompleted,
    kRuntimeChanged,
    kTimedOut,
};

constexpr uint32_t setup_portal_retry_delay_ms(uint8_t failure_count)
{
    if (failure_count == 0) {
        return 0;
    }
    constexpr uint32_t kInitialDelayMs = 1000;
    constexpr uint32_t kMaximumDelayMs = 30000;
    uint32_t delay_ms = kInitialDelayMs;
    for (uint8_t failure = 1;
         failure < failure_count && delay_ms < kMaximumDelayMs;
         ++failure) {
        delay_ms = delay_ms > kMaximumDelayMs / 2
                       ? kMaximumDelayMs
                       : delay_ms * 2;
    }
    return delay_ms;
}

void wait_for_network_sync_event(uint32_t timeout_ms);
void wait_for_network_stagger_interrupt(uint32_t timeout_ms);
void wait_for_setup_portal_retry(uint8_t failure_count);
void wait_for_active_setup_portal_request();
void wait_for_network_runtime_request();
void wait_for_ota_network_block_change();
bool wait_for_network_sync_settle(uint32_t timeout_ms);
NetworkSyncConnectionWaitResult wait_for_valid_network_sync_connection(
    const NetworkSyncAvailability &scheduled_runtime,
    const NetworkSyncRequestSnapshot &scheduled_requests,
    uint32_t timeout_ms);
NetworkSyncCompletionWaitResult wait_for_ntp_sync_completion(uint32_t timeout_ms);
