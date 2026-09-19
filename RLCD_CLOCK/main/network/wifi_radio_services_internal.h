// 声明仅供应用启动阶段使用的 Wi-Fi 驱动初始化入口。
#pragma once

#include "wifi_radio_services.h"
#include <stdint.h>

// Used only around weather requests on the serialized network task.
class WeatherWifiPerformanceGuard {
public:
    explicit WeatherWifiPerformanceGuard(bool enabled = true);
    ~WeatherWifiPerformanceGuard();
    WeatherWifiPerformanceGuard(const WeatherWifiPerformanceGuard &) = delete;
    WeatherWifiPerformanceGuard &operator=(const WeatherWifiPerformanceGuard &) = delete;
private:
    int previous_mode_ = 0;
    uint32_t generation_ = 0;
    bool changed_ = false;
};

enum class WifiRadioIdleStopResult {
    kNoRequest,
    kDeferred,
    kStopped,
    kRetryRequired,
};

WifiRadioIdleStopResult service_wifi_radio_stop_when_idle();
void init_wifi();
