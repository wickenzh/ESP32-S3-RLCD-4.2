// 验证联网同步错峰、手动请求、低电量和 NTP 退避的纯调度规则。
#include "network_sync_schedule.h"

#include <assert.h>
#include <limits.h>

namespace {
constexpr time_t kNow = 1000;

NetworkSyncScheduleInput base_input()
{
    NetworkSyncScheduleInput input = {};
    input.now = kNow;
    input.have_weather_config = true;
    input.boot_weather_due_at = kNow + 8;
    input.boot_saying_due_at = kNow + 16;
    return input;
}
} // namespace

int main()
{
    const NetworkSyncAvailability available = {true, true, false, false};
    assert(!network_sync_availability_changed(available, available));
    assert(network_sync_availability_changed(
        available, NetworkSyncAvailability{false, true, false, false}));
    assert(network_sync_availability_changed(
        available, NetworkSyncAvailability{true, false, false, false}));
    assert(network_sync_availability_changed(
        available, NetworkSyncAvailability{true, true, true, false}));
    assert(network_sync_availability_changed(
        available, NetworkSyncAvailability{true, true, false, true}));

    assert(network_boot_https_memory_sufficient(48 * 1024, 24 * 1024, 16 * 1024));
    assert(network_boot_https_memory_sufficient(96 * 1024, 48 * 1024, 32 * 1024));
    assert(!network_boot_https_memory_sufficient(48 * 1024 - 1, 24 * 1024, 16 * 1024));
    assert(!network_boot_https_memory_sufficient(48 * 1024, 24 * 1024 - 1, 16 * 1024));
    assert(!network_boot_https_memory_sufficient(48 * 1024, 24 * 1024, 16 * 1024 - 1));
    assert(!network_startup_pressure_window_active(true, 120LL * 1000 * 1000));
    assert(network_startup_pressure_window_active(false, 0));
    assert(network_startup_pressure_window_active(false, 59LL * 1000 * 1000));
    assert(!network_startup_pressure_window_active(false, 60LL * 1000 * 1000));
    assert(!network_startup_pressure_window_active(false, -1));
    assert(network_startup_pressure_window_active(true, -1));
    assert(network_weather_request_settle_delay_ms(true) == 300);
    assert(network_weather_request_settle_delay_ms(false) == 120);
    assert(network_inter_operation_settle_delay_ms(true) == 1000);
    assert(network_inter_operation_settle_delay_ms(false) == 250);
    assert(network_sync_connection_timeout_ms(false) == 30000);
    assert(network_sync_connection_timeout_ms(true) == 45000);
    assert(network_sync_connection_timeout_ms(false) <
           network_sync_connection_timeout_ms(true));
    assert(network_hourly_weather_stagger_delay_ms(true, true, 0, 0) == 8000);
    assert(network_hourly_weather_stagger_delay_ms(true, true, 0, 7) == 1000);
    assert(network_hourly_weather_stagger_delay_ms(true, true, 0, 8) == 0);
    assert(network_hourly_weather_stagger_delay_ms(true, true, 1, 0) == 0);
    assert(network_hourly_weather_stagger_delay_ms(false, true, 0, 0) == 0);
    assert(network_hourly_weather_stagger_delay_ms(true, false, 0, 0) == 0);
    assert(network_hourly_weather_stagger_delay_ms(true, true, -1, 0) == 0);
    assert(network_hourly_weather_stagger_delay_ms(true, true, 0, -1) == 0);
    assert(network_ntp_retry_delay_seconds(false, 0) == 15);
    assert(network_ntp_retry_delay_seconds(false, 1) == 15);
    assert(network_ntp_retry_delay_seconds(false, 2) == 30);
    assert(network_ntp_retry_delay_seconds(false, 3) == 60);
    assert(network_ntp_retry_delay_seconds(false, 4) == 120);
    assert(network_ntp_retry_delay_seconds(false, 5) == 240);
    assert(network_ntp_retry_delay_seconds(false, 6) == 5 * 60);
    assert(network_ntp_retry_delay_seconds(false, 1000) == 5 * 60);
    assert(network_ntp_retry_delay_seconds(true, 0) == 5 * 60);
    assert(network_ntp_retry_delay_seconds(true, 1) == 5 * 60);
    assert(network_ntp_retry_delay_seconds(true, 2) == 10 * 60);
    assert(network_ntp_retry_delay_seconds(true, 3) == 20 * 60);
    assert(network_ntp_retry_delay_seconds(true, 4) == 40 * 60);
    assert(network_ntp_retry_delay_seconds(true, 5) == 60 * 60);
    assert(network_ntp_retry_delay_seconds(true, 1000) == 60 * 60);
    assert(network_boot_https_memory_retry_delay_seconds(0) == 10);
    assert(network_boot_https_memory_retry_delay_seconds(1) == 10);
    assert(network_boot_https_memory_retry_delay_seconds(2) == 20);
    assert(network_boot_https_memory_retry_delay_seconds(3) == 40);
    assert(network_boot_https_memory_retry_delay_seconds(4) == 60);
    assert(network_boot_https_memory_retry_delay_seconds(1000) == 60);
    assert(!network_visible_auto_sync_allowed(0));
    assert(!network_visible_auto_sync_allowed(29LL * 1000 * 1000));
    assert(network_visible_auto_sync_allowed(30LL * 1000 * 1000));
    assert(network_visible_auto_sync_allowed(-1));
    assert(network_startup_followup_https_allowed(true,
                                                  48 * 1024,
                                                  24 * 1024,
                                                  16 * 1024));
    assert(!network_startup_followup_https_allowed(true,
                                                   48 * 1024 - 1,
                                                   24 * 1024,
                                                   16 * 1024));
    assert(network_startup_followup_https_allowed(false, 0, 0, 0));
    assert(!network_automatic_boot_https_allowed(false,
                                                 59LL * 1000 * 1000,
                                                 48 * 1024 - 1,
                                                 24 * 1024,
                                                 16 * 1024));
    assert(network_automatic_boot_https_allowed(false,
                                                60LL * 1000 * 1000,
                                                0,
                                                0,
                                                0));
    assert(network_automatic_boot_https_allowed(true,
                                                120LL * 1000 * 1000,
                                                0,
                                                0,
                                                0));
    NetworkSyncSchedule boot_https_schedule = {};
    boot_https_schedule.boot_weather_ready = true;
    boot_https_schedule.boot_saying_ready = true;
    boot_https_schedule.stagger_boot_saying_after_weather = true;
    boot_https_schedule.weather_due = true;
    boot_https_schedule.saying_due = true;
    NetworkBootHttpsDeferralInput boot_https_input = {};
    boot_https_input.now = kNow;
    boot_https_input.retry_delay_seconds = 10;
    assert(network_automatic_boot_https_pending(boot_https_schedule,
                                                boot_https_input));
    NetworkBootHttpsDeferralResult deferral =
        calculate_network_boot_https_deferral(boot_https_schedule,
                                              boot_https_input);
    assert(deferral.deferred);
    assert(deferral.weather_deferred);
    assert(deferral.saying_deferred);
    assert(deferral.retry_at == kNow + 10);
    assert(deferral.schedule.next_boot_due_at == kNow + 10);
    assert(network_idle_wait_ms(kNow,
                                deferral.schedule.next_boot_due_at,
                                kNow + 2,
                                0) == 2000);
    assert(!deferral.schedule.boot_weather_ready);
    assert(!deferral.schedule.boot_saying_ready);
    assert(!deferral.schedule.stagger_boot_saying_after_weather);
    assert(!deferral.schedule.weather_due);
    assert(!deferral.schedule.saying_due);

    boot_https_schedule.next_boot_due_at = kNow + 4;
    deferral = calculate_network_boot_https_deferral(boot_https_schedule,
                                                     boot_https_input);
    assert(deferral.deferred);
    assert(deferral.schedule.next_boot_due_at == kNow + 4);
    boot_https_schedule.next_boot_due_at = 0;

    boot_https_input.memory_allowed = true;
    deferral = calculate_network_boot_https_deferral(boot_https_schedule,
                                                     boot_https_input);
    assert(!deferral.deferred);
    assert(deferral.schedule.weather_due);
    assert(deferral.schedule.saying_due);

    boot_https_input.memory_allowed = false;
    boot_https_input.manual_weather_due = true;
    deferral = calculate_network_boot_https_deferral(boot_https_schedule,
                                                     boot_https_input);
    assert(deferral.deferred);
    assert(!deferral.weather_deferred);
    assert(deferral.saying_deferred);
    assert(deferral.schedule.weather_due);
    assert(!deferral.schedule.saying_due);

    boot_https_input.manual_weather_due = false;
    boot_https_input.manual_saying_due = true;
    deferral = calculate_network_boot_https_deferral(boot_https_schedule,
                                                     boot_https_input);
    assert(deferral.deferred);
    assert(deferral.weather_deferred);
    assert(!deferral.saying_deferred);
    assert(!deferral.schedule.weather_due);
    assert(deferral.schedule.saying_due);

    boot_https_input.provisioning_sync_due = true;
    assert(!network_automatic_boot_https_pending(boot_https_schedule,
                                                 boot_https_input));
    deferral = calculate_network_boot_https_deferral(boot_https_schedule,
                                                     boot_https_input);
    assert(!deferral.deferred);
    assert(deferral.schedule.weather_due);
    assert(deferral.schedule.saying_due);

    NetworkAutomaticBootPageInput page_input = {};
    assert(network_automatic_boot_refresh_page_disabled(boot_https_schedule,
                                                        page_input));
    page_input.weather_page_enabled = true;
    assert(network_automatic_boot_refresh_page_disabled(boot_https_schedule,
                                                        page_input));
    page_input.saying_page_enabled = true;
    assert(!network_automatic_boot_refresh_page_disabled(boot_https_schedule,
                                                         page_input));
    page_input.weather_page_enabled = false;
    page_input.explicit_weather_due = true;
    assert(!network_automatic_boot_refresh_page_disabled(boot_https_schedule,
                                                         page_input));
    page_input.explicit_weather_due = false;
    page_input.saying_page_enabled = false;
    page_input.explicit_saying_due = true;
    assert(network_automatic_boot_refresh_page_disabled(boot_https_schedule,
                                                        page_input));
    page_input.weather_page_enabled = true;
    assert(!network_automatic_boot_refresh_page_disabled(boot_https_schedule,
                                                         page_input));
    page_input.explicit_saying_due = false;
    page_input.provisioning_sync_due = true;
    page_input.weather_page_enabled = false;
    assert(!network_automatic_boot_refresh_page_disabled(boot_https_schedule,
                                                         page_input));

    assert(network_boot_budget_remaining_ms(0, 1000) == INT32_MAX);
    assert(network_boot_budget_remaining_ms(-1, 1000) == INT32_MAX);
    assert(network_boot_budget_remaining_ms(1000, 1000) == 0);
    assert(network_boot_budget_remaining_ms(999, 1000) == 0);
    assert(network_boot_budget_remaining_ms(1999, 1000) == 0);
    assert(network_boot_budget_remaining_ms(2000, 1000) == 1);
    assert(network_boot_budget_remaining_ms(6500000, 500000) == 6000);
    assert(network_boot_budget_remaining_ms(static_cast<int64_t>(INT32_MAX) * 1000 + 2000,
                                             0) == INT32_MAX);

    assert(network_idle_wait_ms(kNow, 0, 0, 0) == 3600000);
    assert(network_idle_wait_ms(kNow, kNow + 10, 0, 0) == 10000);
    assert(network_idle_wait_ms(kNow, kNow + 10, kNow + 2, 0) == 2000);
    assert(network_idle_wait_ms(kNow, kNow, kNow - 1, 0) == 3600000);
    assert(network_idle_wait_ms(kNow, kNow + 600, kNow + 700, 0) == 600000);
    assert(network_idle_wait_ms(kNow, 0, 0, kNow + 12 * 60 * 60) == 43200000);
    assert(network_idle_wait_ms(kNow, 0, 0, kNow + 25 * 60 * 60) == 86400000);
    assert(network_idle_wait_ms(kNow, kNow + 5, 0, kNow + 12 * 60 * 60) == 5000);

    NetworkSyncScheduleInput input = base_input();
    input.boot_weather_due = true;
    input.boot_saying_due = true;
    NetworkSyncSchedule schedule = calculate_network_sync_schedule(input);
    assert(!schedule.boot_weather_ready);
    assert(!schedule.boot_saying_ready);
    assert(!schedule.weather_due);
    assert(!schedule.saying_due);
    assert(schedule.next_boot_due_at == kNow + 8);

    input.now = kNow + 8;
    schedule = calculate_network_sync_schedule(input);
    assert(schedule.boot_weather_ready);
    assert(!schedule.boot_saying_ready);
    assert(schedule.stagger_boot_saying_after_weather);
    assert(schedule.weather_due);
    assert(!schedule.saying_due);
    assert(schedule.next_boot_due_at == kNow + 16);

    input.now = kNow + 16;
    schedule = calculate_network_sync_schedule(input);
    assert(schedule.boot_weather_ready);
    assert(!schedule.boot_saying_ready);
    assert(schedule.stagger_boot_saying_after_weather);
    assert(schedule.weather_due);
    assert(!schedule.saying_due);
    assert(schedule.next_boot_due_at == 0);

    input.boot_weather_due = false;
    schedule = calculate_network_sync_schedule(input);
    assert(!schedule.boot_weather_ready);
    assert(schedule.boot_saying_ready);
    assert(!schedule.stagger_boot_saying_after_weather);
    assert(!schedule.weather_due);
    assert(schedule.saying_due);

    input = base_input();
    input.provisioning_sync_due = true;
    schedule = calculate_network_sync_schedule(input);
    assert(schedule.ntp_due);
    assert(!schedule.ntp_retry_required);
    assert(schedule.weather_due);
    assert(schedule.saying_due);
    assert(!schedule.stagger_boot_saying_after_weather);

    input = base_input();
    input.now = kNow + 16;
    input.boot_weather_due = true;
    input.boot_saying_due = true;
    input.manual_saying_due = true;
    schedule = calculate_network_sync_schedule(input);
    assert(schedule.boot_weather_ready);
    assert(schedule.boot_saying_ready);
    assert(!schedule.stagger_boot_saying_after_weather);
    assert(schedule.weather_due);
    assert(schedule.saying_due);

    input.next_ntp_retry_at = kNow + 1;
    schedule = calculate_network_sync_schedule(input);
    assert(!schedule.ntp_due);
    assert(schedule.weather_due);
    assert(schedule.saying_due);

    input = base_input();
    input.manual_ntp_due = true;
    input.manual_weather_due = true;
    input.manual_saying_due = true;
    input.low_battery_mode = true;
    schedule = calculate_network_sync_schedule(input);
    assert(schedule.ntp_due);
    assert(!schedule.ntp_retry_required);
    assert(!schedule.weather_due);
    assert(!schedule.saying_due);

    input = base_input();
    input.manual_weather_due = true;
    input.manual_saying_due = true;
    input.have_weather_config = false;
    schedule = calculate_network_sync_schedule(input);
    assert(!schedule.weather_due);
    assert(schedule.saying_due);

    input = base_input();
    input.boot_weather_due = true;
    input.boot_saying_due = true;
    input.boot_weather_due_at = kNow + 20;
    input.boot_saying_due_at = kNow + 5;
    schedule = calculate_network_sync_schedule(input);
    assert(schedule.next_boot_due_at == kNow + 5);

    input = base_input();
    input.boot_ntp_due = true;
    input.next_ntp_retry_at = kNow + 1;
    schedule = calculate_network_sync_schedule(input);
    assert(!schedule.ntp_due);
    assert(schedule.ntp_retry_required);

    input.manual_ntp_due = true;
    schedule = calculate_network_sync_schedule(input);
    assert(schedule.ntp_due);
    assert(schedule.ntp_retry_required);

    input = base_input();
    input.provisioning_sync_due = true;
    input.next_ntp_retry_at = kNow + 1;
    schedule = calculate_network_sync_schedule(input);
    assert(schedule.ntp_due);
    assert(!schedule.ntp_retry_required);

    input = base_input();
    input.daily_ntp_due = true;
    input.next_ntp_retry_at = kNow - 1;
    schedule = calculate_network_sync_schedule(input);
    assert(schedule.ntp_due);
    assert(schedule.ntp_retry_required);

    return 0;
}
