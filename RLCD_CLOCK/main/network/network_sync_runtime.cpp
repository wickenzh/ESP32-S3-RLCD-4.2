// 汇总离线、低电量、OTA 和配网请求状态，供多段联网流程统一复核。
#include "network_sync_runtime.h"

#include "battery_runtime_state.h"
#include "network_credentials_state.h"
#include "weather_provider.h"
#include "network_sync_schedule.h"
#include "offline_mode_state.h"
#include "ota_runtime_state.h"
#include "setup_portal_control.h"

NetworkSyncAvailability capture_network_runtime_availability()
{
    const NetworkCredentialsAvailability credentials =
        network_credentials_availability();
    return {
        credentials.wifi_configured,
        weather_provider_load() == WeatherProvider::kOpenMeteo ||
            (credentials.weather_api_key_configured && credentials.weather_api_host_configured),
        offline_mode_enabled_load(),
        battery_low_mode_load(),
    };
}

bool network_sync_start_context_changed(
    const NetworkSyncAvailability &scheduled,
    const NetworkSyncAvailability &current)
{
    return network_sync_availability_changed(scheduled, current) ||
           ota_blocks_background_network_sync(ota_runtime_state_load()) ||
           setup_portal_start_requested();
}

bool network_sync_continuation_allowed()
{
    NetworkSyncContinuationState state = {};
    state.offline_mode = offline_mode_enabled_load();
    state.low_battery_mode = battery_low_mode_load();
    state.ota_blocks_background_sync =
        ota_blocks_background_network_sync(ota_runtime_state_load());
    state.setup_portal_start_requested = setup_portal_start_requested();
    return network_sync_continuation_allowed(state);
}
