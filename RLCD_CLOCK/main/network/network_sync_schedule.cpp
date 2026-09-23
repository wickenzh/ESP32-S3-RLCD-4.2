// 计算联网同步任务本轮应执行的项目，不访问 Wi-Fi、事件组或全局状态。
#include "network_sync_schedule.h"
#include "wifi_failover_policy.h"

#include <limits.h>

namespace {
constexpr int64_t kMicrosecondsPerMillisecond = 1000;
constexpr uint32_t kMillisecondsPerSecond = 1000;
constexpr time_t kSecondsPerMinute = 60;
constexpr time_t kInvalidTimeNtpRetryBaseDelaySeconds = 15;
constexpr time_t kInvalidTimeNtpRetryMaximumDelaySeconds = 5 * kSecondsPerMinute;
constexpr time_t kValidTimeNtpRetryBaseDelaySeconds = 5 * kSecondsPerMinute;
constexpr time_t kValidTimeNtpRetryMaximumDelaySeconds = 60 * kSecondsPerMinute;
constexpr time_t kBootHttpsMemoryRetryBaseDelaySeconds = 10;
constexpr time_t kBootHttpsMemoryRetryMaximumDelaySeconds = kSecondsPerMinute;
constexpr uint32_t kIdleFallbackWaitMs = 60 * kSecondsPerMinute * kMillisecondsPerSecond;
constexpr uint32_t kIdleMaximumWaitMs = 24 * 60 * kSecondsPerMinute * kMillisecondsPerSecond;
constexpr uint32_t kIdleMinimumWaitMs = 1000;
constexpr size_t kBootHttpsMinimumInternalFree = 48 * 1024;
constexpr size_t kBootHttpsMinimumInternalLargest = 24 * 1024;
constexpr size_t kBootHttpsMinimumDmaLargest = 16 * 1024;
constexpr int64_t kStartupPressureWindowUs = 60LL * 1000 * 1000;
constexpr int64_t kStartupVisibleAutoSyncDelayUs = 30LL * 1000 * 1000;
constexpr uint32_t kWeatherRequestSettleDelayMs = 120;
constexpr uint32_t kStartupWeatherRequestSettleDelayMs = 300;
constexpr uint32_t kNetworkOperationSettleDelayMs = 250;
constexpr uint32_t kStartupNetworkOperationSettleDelayMs = 1000;
constexpr uint32_t kAutomaticWifiConnectTimeoutMs = 30000;
constexpr uint32_t kInteractiveWifiConnectTimeoutMs = 45000;
constexpr uint32_t kHourlyWeatherStaggerSeconds = 8;
constexpr uint8_t kAutomaticBootHttpsWeatherBit = 1u << 0;
constexpr uint8_t kAutomaticBootHttpsSayingBit = 1u << 1;
static_assert(kIdleMinimumWaitMs > 0, "network idle minimum wait must be positive");
static_assert(kInvalidTimeNtpRetryBaseDelaySeconds > 0,
              "invalid-time NTP retry delay must be positive");
static_assert(kInvalidTimeNtpRetryMaximumDelaySeconds >=
                  kInvalidTimeNtpRetryBaseDelaySeconds,
              "invalid-time NTP retry cap must cover the initial recovery retry");
static_assert(kValidTimeNtpRetryBaseDelaySeconds ==
                  kInvalidTimeNtpRetryMaximumDelaySeconds,
              "valid-time NTP retry must begin at the invalid-time recovery ceiling");
static_assert(kValidTimeNtpRetryMaximumDelaySeconds >
                  kValidTimeNtpRetryBaseDelaySeconds,
              "valid-time NTP retry cap must reduce repeated Wi-Fi wakeups");
static_assert(kBootHttpsMemoryRetryMaximumDelaySeconds >=
                  kBootHttpsMemoryRetryBaseDelaySeconds,
              "boot HTTPS memory retry cap must cover its initial retry");
static_assert(kIdleFallbackWaitMs >= kIdleMinimumWaitMs,
              "network idle fallback wait must cover the minimum wait");
static_assert(kIdleMaximumWaitMs >= kIdleFallbackWaitMs,
              "network idle maximum wait must cover the fallback wait");
static_assert(kBootHttpsMinimumInternalFree >= kBootHttpsMinimumInternalLargest,
              "boot HTTPS free-memory threshold must cover the largest-block threshold");
static_assert(kBootHttpsMinimumInternalLargest >= kBootHttpsMinimumDmaLargest,
              "boot HTTPS internal block threshold must cover the DMA block threshold");
static_assert(kStartupPressureWindowUs > 0,
              "startup pressure window must be positive");
static_assert(kStartupVisibleAutoSyncDelayUs > 0 &&
                  kStartupVisibleAutoSyncDelayUs < kStartupPressureWindowUs,
              "visible auto sync delay must fit the startup pressure window");
static_assert(kStartupWeatherRequestSettleDelayMs > kWeatherRequestSettleDelayMs,
              "startup HTTPS requests must use the longer settle delay");
static_assert(kStartupNetworkOperationSettleDelayMs > kNetworkOperationSettleDelayMs,
              "startup network operations must use the longer settle delay");
static_assert(kAutomaticWifiConnectTimeoutMs > kWifiPrimaryAttemptWindowMs,
              "automatic Wi-Fi timeout must leave time for the backup profile");
static_assert(kInteractiveWifiConnectTimeoutMs > kAutomaticWifiConnectTimeoutMs,
              "interactive Wi-Fi timeout must retain the longest recovery window");
static_assert(kHourlyWeatherStaggerSeconds > 2 &&
                  kHourlyWeatherStaggerSeconds < kSecondsPerMinute,
              "hourly weather stagger must follow the chime window and fit one minute");

time_t earliest_pending_boot_sync(const NetworkSyncScheduleInput &input)
{
    time_t next = 0;
    if (input.boot_weather_due && input.boot_weather_due_at > input.now) {
        next = input.boot_weather_due_at;
    }
    if (input.boot_saying_due &&
        input.boot_saying_due_at > input.now &&
        (next == 0 || input.boot_saying_due_at < next)) {
        next = input.boot_saying_due_at;
    }
    return next;
}

uint32_t future_deadline_wait_ms(time_t now,
                                 time_t deadline,
                                 uint32_t fallback_ms)
{
    if (deadline <= now) {
        return fallback_ms;
    }
    int64_t seconds = static_cast<int64_t>(deadline) - static_cast<int64_t>(now);
    if (seconds >= static_cast<int64_t>(kIdleMaximumWaitMs) /
                       kMillisecondsPerSecond) {
        return kIdleMaximumWaitMs;
    }
    return static_cast<uint32_t>(seconds * kMillisecondsPerSecond);
}

uint8_t automatic_boot_https_mask(
    const NetworkSyncSchedule &schedule,
    const NetworkBootHttpsDeferralInput &input)
{
    uint8_t mask = 0;
    if (schedule.boot_weather_ready &&
        !input.provisioning_sync_due &&
        !input.manual_weather_due) {
        mask |= kAutomaticBootHttpsWeatherBit;
    }
    if (schedule.boot_saying_ready &&
        !input.provisioning_sync_due &&
        !input.manual_saying_due) {
        mask |= kAutomaticBootHttpsSayingBit;
    }
    return mask;
}

time_t capped_exponential_backoff_seconds(time_t base_delay,
                                          time_t maximum_delay,
                                          uint32_t consecutive_events)
{
    time_t delay = base_delay;
    uint32_t remaining_backoff_steps =
        consecutive_events > 0 ? consecutive_events - 1 : 0;
    while (remaining_backoff_steps > 0 && delay < maximum_delay) {
        if (delay > maximum_delay / 2) {
            delay = maximum_delay;
            break;
        }
        delay *= 2;
        --remaining_backoff_steps;
    }
    return delay < maximum_delay ? delay : maximum_delay;
}
} // namespace

NetworkSyncSchedule calculate_network_sync_schedule(const NetworkSyncScheduleInput &input)
{
    NetworkSyncSchedule schedule = {};
    schedule.boot_weather_ready = input.boot_weather_due && input.now >= input.boot_weather_due_at;
    bool boot_saying_time_ready = input.boot_saying_due && input.now >= input.boot_saying_due_at;
    schedule.stagger_boot_saying_after_weather =
        schedule.boot_weather_ready && input.boot_saying_due &&
        !input.provisioning_sync_due && !input.manual_saying_due;
    schedule.boot_saying_ready = boot_saying_time_ready &&
                                 !schedule.stagger_boot_saying_after_weather;
    schedule.weather_due = input.have_weather_config &&
                           !input.low_battery_mode &&
                           (input.manual_weather_due ||
                            input.provisioning_sync_due ||
                            schedule.boot_weather_ready);
    const bool explicit_ntp_due = input.manual_ntp_due ||
                                  input.provisioning_sync_due;
    const bool automatic_ntp_due = input.boot_ntp_due ||
                                   input.daily_ntp_due;
    schedule.ntp_due = explicit_ntp_due ||
                       (automatic_ntp_due && input.now >= input.next_ntp_retry_at);
    schedule.ntp_retry_required = input.boot_ntp_due || input.daily_ntp_due;
    schedule.saying_due = !input.low_battery_mode &&
                          (input.manual_saying_due ||
                           input.provisioning_sync_due ||
                           schedule.boot_saying_ready);
    schedule.next_boot_due_at = earliest_pending_boot_sync(input);
    return schedule;
}

bool network_sync_availability_changed(const NetworkSyncAvailability &scheduled,
                                       const NetworkSyncAvailability &current)
{
    return scheduled.have_wifi_creds != current.have_wifi_creds ||
           scheduled.have_weather_config != current.have_weather_config ||
           scheduled.offline_mode != current.offline_mode ||
           scheduled.low_battery_mode != current.low_battery_mode;
}

time_t network_ntp_retry_delay_seconds(bool time_plausible,
                                       uint32_t consecutive_failures)
{
    if (time_plausible) {
        return capped_exponential_backoff_seconds(
            kValidTimeNtpRetryBaseDelaySeconds,
            kValidTimeNtpRetryMaximumDelaySeconds,
            consecutive_failures);
    }
    return capped_exponential_backoff_seconds(
        kInvalidTimeNtpRetryBaseDelaySeconds,
        kInvalidTimeNtpRetryMaximumDelaySeconds,
        consecutive_failures);
}

time_t network_boot_https_memory_retry_delay_seconds(
    uint32_t consecutive_deferrals)
{
    return capped_exponential_backoff_seconds(
        kBootHttpsMemoryRetryBaseDelaySeconds,
        kBootHttpsMemoryRetryMaximumDelaySeconds,
        consecutive_deferrals);
}

NetworkBootHttpsDeferralResult calculate_network_boot_https_deferral(
    const NetworkSyncSchedule &schedule,
    const NetworkBootHttpsDeferralInput &input)
{
    NetworkBootHttpsDeferralResult result = {};
    result.schedule = schedule;
    uint8_t automatic_mask = automatic_boot_https_mask(schedule, input);
    if (automatic_mask == 0 || input.memory_allowed) {
        return result;
    }

    result.deferred = true;
    result.retry_at = input.now + input.retry_delay_seconds;
    if (result.schedule.next_boot_due_at <= input.now ||
        result.retry_at < result.schedule.next_boot_due_at) {
        result.schedule.next_boot_due_at = result.retry_at;
    }
    if ((automatic_mask & kAutomaticBootHttpsWeatherBit) != 0) {
        result.weather_deferred = true;
        result.schedule.weather_due = false;
        result.schedule.boot_weather_ready = false;
        result.schedule.stagger_boot_saying_after_weather = false;
    }
    if ((automatic_mask & kAutomaticBootHttpsSayingBit) != 0) {
        result.saying_deferred = true;
        result.schedule.saying_due = false;
        result.schedule.boot_saying_ready = false;
    }
    return result;
}

bool network_automatic_boot_https_pending(
    const NetworkSyncSchedule &schedule,
    const NetworkBootHttpsDeferralInput &input)
{
    return automatic_boot_https_mask(schedule, input) != 0;
}

bool network_automatic_boot_refresh_page_disabled(
    const NetworkSyncSchedule &schedule,
    const NetworkAutomaticBootPageInput &input)
{
    if (input.provisioning_sync_due) {
        return false;
    }
    const bool automatic_weather_lost_page =
        schedule.boot_weather_ready &&
        !input.explicit_weather_due &&
        !input.weather_page_enabled;
    const bool automatic_saying_lost_page =
        schedule.boot_saying_ready &&
        !input.explicit_saying_due &&
        !input.saying_page_enabled;
    return automatic_weather_lost_page || automatic_saying_lost_page;
}

int network_boot_budget_remaining_ms(int64_t deadline_us, int64_t now_us)
{
    if (deadline_us <= 0) {
        return INT32_MAX;
    }
    int64_t remaining_us = deadline_us - now_us;
    if (remaining_us <= 0) {
        return 0;
    }
    int64_t remaining_ms = remaining_us / kMicrosecondsPerMillisecond;
    return remaining_ms > INT32_MAX ? INT32_MAX : static_cast<int>(remaining_ms);
}

uint32_t network_idle_wait_ms(time_t now,
                              time_t next_boot_due_at,
                              time_t next_ntp_retry_at,
                              time_t next_daily_ntp_at)
{
    uint32_t wait_ms = next_daily_ntp_at > now
                           ? future_deadline_wait_ms(now,
                                                     next_daily_ntp_at,
                                                     kIdleFallbackWaitMs)
                           : kIdleFallbackWaitMs;
    if (next_boot_due_at > now) {
        uint32_t boot_wait = future_deadline_wait_ms(now,
                                                     next_boot_due_at,
                                                     kIdleFallbackWaitMs);
        if (boot_wait < wait_ms) {
            wait_ms = boot_wait;
        }
    }
    if (next_ntp_retry_at > now) {
        uint32_t ntp_wait = future_deadline_wait_ms(now,
                                                    next_ntp_retry_at,
                                                    kIdleFallbackWaitMs);
        if (ntp_wait < wait_ms) {
            wait_ms = ntp_wait;
        }
    }
    if (wait_ms < kIdleMinimumWaitMs) {
        wait_ms = kIdleMinimumWaitMs;
    } else if (wait_ms > kIdleMaximumWaitMs) {
        wait_ms = kIdleMaximumWaitMs;
    }
    return wait_ms;
}

bool network_boot_https_memory_sufficient(size_t internal_free,
                                          size_t internal_largest,
                                          size_t dma_largest)
{
    return internal_free >= kBootHttpsMinimumInternalFree &&
           internal_largest >= kBootHttpsMinimumInternalLargest &&
           dma_largest >= kBootHttpsMinimumDmaLargest;
}

bool network_startup_pressure_window_active(bool startup_screen_active,
                                            int64_t uptime_us)
{
    if (uptime_us >= 0) {
        // The boot-screen flag is an additional early-start signal, not an
        // unbounded HTTPS gate. A stale lifecycle flag must not postpone page
        // data forever after the fixed startup pressure window has elapsed.
        return uptime_us < kStartupPressureWindowUs;
    }
    return startup_screen_active;
}

uint32_t network_weather_request_settle_delay_ms(bool startup_pressure_active)
{
    return startup_pressure_active
               ? kStartupWeatherRequestSettleDelayMs
               : kWeatherRequestSettleDelayMs;
}

uint32_t network_inter_operation_settle_delay_ms(bool startup_pressure_active)
{
    return startup_pressure_active
               ? kStartupNetworkOperationSettleDelayMs
               : kNetworkOperationSettleDelayMs;
}

uint32_t network_sync_connection_timeout_ms(bool interactive_request)
{
    return interactive_request
               ? kInteractiveWifiConnectTimeoutMs
               : kAutomaticWifiConnectTimeoutMs;
}

uint32_t network_hourly_weather_stagger_delay_ms(bool automatic_weather_due,
                                                 bool hourly_chime_enabled,
                                                 int minute,
                                                 int second)
{
    if (!automatic_weather_due || !hourly_chime_enabled || minute != 0 ||
        second < 0 || second >= static_cast<int>(kHourlyWeatherStaggerSeconds)) {
        return 0;
    }
    return (kHourlyWeatherStaggerSeconds - static_cast<uint32_t>(second)) *
           kMillisecondsPerSecond;
}

bool network_visible_auto_sync_allowed(int64_t uptime_us)
{
    return uptime_us < 0 || uptime_us >= kStartupVisibleAutoSyncDelayUs;
}

bool network_startup_followup_https_allowed(bool startup_pressure_active,
                                            size_t internal_free,
                                            size_t internal_largest,
                                            size_t dma_largest)
{
    return !startup_pressure_active ||
           network_boot_https_memory_sufficient(internal_free,
                                                internal_largest,
                                                dma_largest);
}

bool network_automatic_boot_https_allowed(bool startup_screen_active,
                                          int64_t uptime_us,
                                          size_t internal_free,
                                          size_t internal_largest,
                                          size_t dma_largest)
{
    return network_startup_followup_https_allowed(
        network_startup_pressure_window_active(startup_screen_active, uptime_us),
        internal_free,
        internal_largest,
        dma_largest);
}
