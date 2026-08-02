// 判断延后的 Wi-Fi 关闭请求何时可以安全执行。
#pragma once

struct WifiIdleStopPolicyInput {
    bool requested = false;
    bool radio_on = false;
    bool setup_portal_active = false;
    bool ota_active = false;
    bool xiaozhi_keepalive_active = false;
    bool network_lock_active = false;
    bool radio_keepalive_active = false;
};

constexpr bool wifi_idle_stop_allowed(const WifiIdleStopPolicyInput &input)
{
    return input.requested &&
           input.radio_on &&
           !input.setup_portal_active &&
           !input.ota_active &&
           !input.xiaozhi_keepalive_active &&
           !input.network_lock_active &&
           !input.radio_keepalive_active;
}
