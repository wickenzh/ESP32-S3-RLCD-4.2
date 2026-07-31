// 验证配网页活跃状态、Wi-Fi 断线原因和本地 IP 的跨任务快照。
#include "wifi_portal_state.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <thread>

int main()
{
    assert(!setup_portal_active_load());
    setup_portal_active_store(true);
    assert(setup_portal_active_load());
    setup_portal_active_store(false);
    assert(!setup_portal_active_load());

    assert(wifi_last_disconnect_reason() == 0);
    record_wifi_disconnect_reason(8);
    assert(wifi_last_disconnect_reason() == 8);
    record_wifi_disconnect_reason(-1);
    assert(wifi_last_disconnect_reason() == -1);
    clear_wifi_last_disconnect_reason();
    assert(wifi_last_disconnect_reason() == 0);

    char station_ip[kWifiStationIpTextLen] = {};
    assert(!wifi_station_ip_snapshot(station_ip, sizeof(station_ip)));
    assert(station_ip[0] == '\0');
    assert(!wifi_station_ip_snapshot(station_ip, sizeof(station_ip) - 1));

    wifi_station_ip_store("192.168.4.20");
    assert(wifi_station_ip_snapshot(station_ip, sizeof(station_ip)));
    assert(std::strcmp(station_ip, "192.168.4.20") == 0);

    std::atomic<bool> writer_done{false};
    std::thread writer([&]() {
        for (int i = 0; i < 10000; ++i) {
            wifi_station_ip_store((i & 1) ? "10.10.10.155" : "192.168.100.200");
        }
        writer_done.store(true, std::memory_order_release);
    });
    while (!writer_done.load(std::memory_order_acquire)) {
        assert(wifi_station_ip_snapshot(station_ip, sizeof(station_ip)));
        assert(std::strcmp(station_ip, "10.10.10.155") == 0 ||
               std::strcmp(station_ip, "192.168.100.200") == 0 ||
               std::strcmp(station_ip, "192.168.4.20") == 0);
    }
    writer.join();

    clear_wifi_station_ip();
    assert(!wifi_station_ip_snapshot(station_ip, sizeof(station_ip)));
    assert(station_ip[0] == '\0');

    std::puts("Wi-Fi portal state host tests passed");
    return 0;
}
