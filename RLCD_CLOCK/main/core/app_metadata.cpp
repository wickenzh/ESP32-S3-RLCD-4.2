// 定义应用日志标签、版本号和构建日期等只读元数据。
#include "app_metadata.h"

const char *const TAG = "WeatherClock";
const char *const APP_VERSION = "v1.6.4";
#ifndef WEATHER_CLOCK_BUILD_DATE
#define WEATHER_CLOCK_BUILD_DATE "unknown"
#endif
const char *const APP_BUILD_DATE = WEATHER_CLOCK_BUILD_DATE;
