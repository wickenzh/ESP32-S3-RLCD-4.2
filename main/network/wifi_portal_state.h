// 声明 Wi-Fi 门户、断线原因和本地 IP 的跨任务安全访问接口。
#pragma once

#include <stddef.h>

inline constexpr size_t kWifiStationIpTextLen = 16;

bool setup_portal_active_load();
void setup_portal_active_store(bool active);
int wifi_last_disconnect_reason();
void record_wifi_disconnect_reason(int reason);
void clear_wifi_last_disconnect_reason();
bool wifi_station_ip_snapshot(char *out, size_t out_len);
void wifi_station_ip_store(const char *ip_text);
void clear_wifi_station_ip();
