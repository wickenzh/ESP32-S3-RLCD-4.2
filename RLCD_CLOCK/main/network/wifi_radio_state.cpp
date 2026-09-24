// 集中维护 Wi-Fi 射频运行状态，供网络与业务任务安全共享。
#include "wifi_radio_state_internal.h"
#include "runtime_health.h"

#include <atomic>

namespace {
std::atomic<bool> s_wifi_radio_on{false};
} // namespace

bool wifi_radio_on_load()
{
    return s_wifi_radio_on.load(std::memory_order_acquire);
}

void wifi_radio_on_store(bool radio_on)
{
    s_wifi_radio_on.store(radio_on, std::memory_order_release);
    runtime_health_record_wifi_radio_state(radio_on);
}
