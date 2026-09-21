// 集中声明联网配置模块直接维护的 NVS key，避免保存与清理路径写散。
#pragma once

namespace network_config_keys {

inline constexpr const char *kWifiSsidKey = "ssid";
inline constexpr const char *kWifiPassKey = "pass";
inline constexpr const char *kWifiBackupSsidKey = "ssid_b";
inline constexpr const char *kWifiBackupPassKey = "pass_b";
inline constexpr const char *kWifiPreferredSlotKey = "wifi_pri_v1";
inline constexpr const char *kWeatherApiKeyKey = "api_key";
inline constexpr const char *kWeatherProviderKey = "weather_src_v1";
inline constexpr const char *kQweatherApiHostKey = "api_host";
inline constexpr const char *kOfflineModeKey = "offline_v1";
inline constexpr const char *kXiaozhiAutoReturnKey = "xz_auto_ret_v1";
inline constexpr const char *kGalleryRotationKey = "gallery_rot_v1";

} // namespace network_config_keys
