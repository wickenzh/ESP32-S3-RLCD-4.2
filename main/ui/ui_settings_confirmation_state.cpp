// 私有保存设置页二次确认状态，避免确认标志泄漏到全局应用状态。
#include "ui_settings_confirmation_state.h"

#include <atomic>

namespace {
std::atomic<bool> s_factory_reset_pending{false};
std::atomic<bool> s_offline_disable_pending{false};
std::atomic<bool> s_weather_city_clear_pending{false};

std::atomic<bool> *confirmation_slot(SettingsConfirmation confirmation)
{
    switch (confirmation) {
    case SettingsConfirmation::kFactoryReset:
        return &s_factory_reset_pending;
    case SettingsConfirmation::kOfflineDisable:
        return &s_offline_disable_pending;
    case SettingsConfirmation::kWeatherCityClear:
        return &s_weather_city_clear_pending;
    }
    return nullptr;
}
} // namespace

bool settings_confirmation_pending(SettingsConfirmation confirmation)
{
    const std::atomic<bool> *pending = confirmation_slot(confirmation);
    return pending && pending->load(std::memory_order_acquire);
}

void settings_confirmation_request(SettingsConfirmation confirmation)
{
    std::atomic<bool> *pending = confirmation_slot(confirmation);
    if (pending) {
        pending->store(true, std::memory_order_release);
    }
}

void settings_confirmation_clear(SettingsConfirmation confirmation)
{
    std::atomic<bool> *pending = confirmation_slot(confirmation);
    if (pending) {
        pending->store(false, std::memory_order_release);
    }
}

void settings_confirmation_clear_all()
{
    s_factory_reset_pending.store(false, std::memory_order_release);
    s_offline_disable_pending.store(false, std::memory_order_release);
    s_weather_city_clear_pending.store(false, std::memory_order_release);
}
