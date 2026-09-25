// 声明天气同步结果、请求作用域和高层更新入口。
#pragma once

enum class WeatherUpdateResult {
    kSuccess,
    kFailed,
    kResourceDeferred,
};

enum class WeatherUpdateScope {
    kCurrentAndAlerts,
    kFull,
};

struct WeatherUpdateRequestPolicy {
    int qweather_first_request_timeout_ms = 0;
};

WeatherUpdateResult perform_weather_update(
    WeatherUpdateScope scope,
    WeatherUpdateRequestPolicy request_policy = {});
