// 集中维护配网页活跃状态、断线原因和本地 IP 完整快照。
#include "wifi_portal_state.h"

#include "freertos/FreeRTOS.h"

#include <atomic>
#include <string.h>

namespace {
std::atomic<bool> s_setup_portal_active{false};
std::atomic<int> s_last_wifi_disconnect_reason{0};
portMUX_TYPE s_station_ip_mux = portMUX_INITIALIZER_UNLOCKED;
char s_station_ip[kWifiStationIpTextLen] = {};
} // namespace

bool setup_portal_active_load()
{
    return s_setup_portal_active.load(std::memory_order_acquire);
}

void setup_portal_active_store(bool active)
{
    s_setup_portal_active.store(active, std::memory_order_release);
}

int wifi_last_disconnect_reason()
{
    return s_last_wifi_disconnect_reason.load(std::memory_order_acquire);
}

void record_wifi_disconnect_reason(int reason)
{
    s_last_wifi_disconnect_reason.store(reason, std::memory_order_release);
}

void clear_wifi_last_disconnect_reason()
{
    record_wifi_disconnect_reason(0);
}

bool wifi_station_ip_snapshot(char *out, size_t out_len)
{
    if (!out || out_len < sizeof(s_station_ip)) {
        if (out && out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }
    portENTER_CRITICAL(&s_station_ip_mux);
    memcpy(out, s_station_ip, sizeof(s_station_ip));
    const bool available = s_station_ip[0] != '\0';
    portEXIT_CRITICAL(&s_station_ip_mux);
    return available;
}

void wifi_station_ip_store(const char *ip_text)
{
    char replacement[kWifiStationIpTextLen] = {};
    strlcpy(replacement, ip_text ? ip_text : "", sizeof(replacement));
    portENTER_CRITICAL(&s_station_ip_mux);
    memcpy(s_station_ip, replacement, sizeof(s_station_ip));
    portEXIT_CRITICAL(&s_station_ip_mux);
}

void clear_wifi_station_ip()
{
    wifi_station_ip_store("");
}
