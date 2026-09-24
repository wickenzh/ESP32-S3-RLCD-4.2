// 以静态 RAM 状态保存低频健康统计，不增加任务、定时器或持久化写入。
#include "runtime_health.h"

#include "app_metadata.h"
#include "power_services.h"
#include "runtime_health_policy.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <inttypes.h>
#include <stddef.h>

namespace {
constexpr int64_t kRuntimeHealthPeriodicIntervalUs =
    6LL * 60LL * 60LL * 1000000LL;
constexpr TickType_t kTaskStackSampleInterval =
    pdMS_TO_TICKS(60U * 60U * 1000U);

struct RuntimeHealthState {
    uint32_t internal_largest_low = 0;
    uint32_t dma_largest_low = 0;
    uint32_t task_stack_low[static_cast<uint8_t>(RegularAppTaskId::kCount)] = {};
    TickType_t task_stack_sample_tick[static_cast<uint8_t>(RegularAppTaskId::kCount)] = {};
    uint32_t event_count[static_cast<uint8_t>(RuntimeHealthEvent::kCount)] = {};
    uint64_t wifi_completed_us = 0;
    int64_t wifi_started_us = -1;
    uint64_t display_partial_us = 0;
    uint64_t display_full_us = 0;
    uint32_t display_partial_count = 0;
    uint32_t display_full_count = 0;
    int64_t next_periodic_log_us = 0;
    bool wifi_radio_on = false;
};

portMUX_TYPE s_runtime_health_mux = portMUX_INITIALIZER_UNLOCKED;

RuntimeHealthState initial_runtime_health_state()
{
    RuntimeHealthState state = {};
    state.internal_largest_low = UINT32_MAX;
    state.dma_largest_low = UINT32_MAX;
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(RegularAppTaskId::kCount);
         ++i) {
        state.task_stack_low[i] = UINT32_MAX;
    }
    return state;
}

RuntimeHealthState s_runtime_health = initial_runtime_health_state();

static_assert(kRuntimeHealthPeriodicIntervalUs > 0,
              "runtime health interval must be positive");
static_assert(kTaskStackSampleInterval > 0,
              "task stack sample interval must be positive");

uint8_t task_index(RegularAppTaskId id)
{
    return static_cast<uint8_t>(id);
}

uint8_t event_index(RuntimeHealthEvent event)
{
    return static_cast<uint8_t>(event);
}

uint32_t stack_value_or_zero(uint32_t value)
{
    return value == UINT32_MAX ? 0 : value;
}
} // namespace

void runtime_health_record_current_task_stack(RegularAppTaskId id)
{
    const uint8_t index = task_index(id);
    if (index >= static_cast<uint8_t>(RegularAppTaskId::kCount)) {
        return;
    }
    const TickType_t now = xTaskGetTickCount();
    bool sample_due = false;
    portENTER_CRITICAL(&s_runtime_health_mux);
    const TickType_t previous = s_runtime_health.task_stack_sample_tick[index];
    sample_due = previous == 0 || now - previous >= kTaskStackSampleInterval;
    if (sample_due) {
        s_runtime_health.task_stack_sample_tick[index] = now;
    }
    portEXIT_CRITICAL(&s_runtime_health_mux);
    if (!sample_due) {
        return;
    }
    const uint32_t current =
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    portENTER_CRITICAL(&s_runtime_health_mux);
    s_runtime_health.task_stack_low[index] = runtime_health_lower_value(
        s_runtime_health.task_stack_low[index], current);
    portEXIT_CRITICAL(&s_runtime_health_mux);
}

void runtime_health_record_wifi_radio_state(bool radio_on)
{
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_runtime_health_mux);
    if (radio_on != s_runtime_health.wifi_radio_on) {
        if (radio_on) {
            s_runtime_health.wifi_started_us = now_us;
        } else if (s_runtime_health.wifi_started_us >= 0 &&
                   now_us > s_runtime_health.wifi_started_us) {
            s_runtime_health.wifi_completed_us +=
                static_cast<uint64_t>(now_us - s_runtime_health.wifi_started_us);
            s_runtime_health.wifi_started_us = -1;
        }
        s_runtime_health.wifi_radio_on = radio_on;
    }
    portEXIT_CRITICAL(&s_runtime_health_mux);
}

void runtime_health_record_display_flush(bool full_refresh,
                                         uint32_t duration_us)
{
    portENTER_CRITICAL(&s_runtime_health_mux);
    if (full_refresh) {
        ++s_runtime_health.display_full_count;
        s_runtime_health.display_full_us += duration_us;
    } else {
        ++s_runtime_health.display_partial_count;
        s_runtime_health.display_partial_us += duration_us;
    }
    portEXIT_CRITICAL(&s_runtime_health_mux);
}

void runtime_health_note_event(RuntimeHealthEvent event)
{
    const uint8_t index = event_index(event);
    if (index >= static_cast<uint8_t>(RuntimeHealthEvent::kCount)) {
        return;
    }
    portENTER_CRITICAL(&s_runtime_health_mux);
    if (s_runtime_health.event_count[index] != UINT32_MAX) {
        ++s_runtime_health.event_count[index];
    }
    portEXIT_CRITICAL(&s_runtime_health_mux);
}

bool runtime_health_snapshot(RuntimeHealthSnapshot *out)
{
    if (!out) {
        return false;
    }
    RuntimeHealthSnapshot snapshot = {};
    snapshot.internal_free = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    snapshot.internal_min_free = static_cast<uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    snapshot.internal_largest = static_cast<uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    snapshot.dma_free = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_DMA));
    snapshot.dma_min_free = static_cast<uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
    snapshot.dma_largest = static_cast<uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    snapshot.psram_free = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    snapshot.psram_min_free = static_cast<uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_runtime_health_mux);
    s_runtime_health.internal_largest_low = runtime_health_lower_value(
        s_runtime_health.internal_largest_low, snapshot.internal_largest);
    s_runtime_health.dma_largest_low = runtime_health_lower_value(
        s_runtime_health.dma_largest_low, snapshot.dma_largest);
    snapshot.internal_largest_low = s_runtime_health.internal_largest_low;
    snapshot.dma_largest_low = s_runtime_health.dma_largest_low;
    snapshot.wifi_radio_on = s_runtime_health.wifi_radio_on;
    snapshot.wifi_radio_on_ms = runtime_health_wifi_total_us(
                                    s_runtime_health.wifi_radio_on,
                                    s_runtime_health.wifi_started_us,
                                    now_us,
                                    s_runtime_health.wifi_completed_us) /
                                1000U;
    snapshot.display_partial_us = s_runtime_health.display_partial_us;
    snapshot.display_full_us = s_runtime_health.display_full_us;
    snapshot.display_partial_count = s_runtime_health.display_partial_count;
    snapshot.display_full_count = s_runtime_health.display_full_count;
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(RegularAppTaskId::kCount);
         ++i) {
        snapshot.task_stack_low[i] = s_runtime_health.task_stack_low[i];
    }
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(RuntimeHealthEvent::kCount);
         ++i) {
        snapshot.event_count[i] = s_runtime_health.event_count[i];
    }
    portEXIT_CRITICAL(&s_runtime_health_mux);

    PowerLockDepthSnapshot power = {};
    if (get_power_lock_depth_snapshot(&power)) {
        snapshot.pm_network = power.network;
        snapshot.pm_audio = power.audio;
        snapshot.pm_audio_wake = power.audio_wake;
        snapshot.pm_audio_cpu = power.audio_cpu;
    }
    *out = snapshot;
    return true;
}

void runtime_health_log_snapshot(const char *reason)
{
    RuntimeHealthSnapshot snapshot = {};
    if (!runtime_health_snapshot(&snapshot)) {
        return;
    }
    ESP_LOGI(
        TAG,
        "runtime health %s: internal=%u/%u largest=%u/%u dma=%u/%u largest=%u/%u psram=%u/%u wifi_ms=%" PRIu64 " on=%d pm=%d/%d/%d/%d display=%lu/%" PRIu64 "/%lu/%" PRIu64 " stack=%u,%u,%u,%u,%u,%u,%u events=%lu,%lu,%lu,%lu,%lu",
        reason ? reason : "snapshot",
        snapshot.internal_free,
        snapshot.internal_min_free,
        snapshot.internal_largest,
        snapshot.internal_largest_low,
        snapshot.dma_free,
        snapshot.dma_min_free,
        snapshot.dma_largest,
        snapshot.dma_largest_low,
        snapshot.psram_free,
        snapshot.psram_min_free,
        snapshot.wifi_radio_on_ms,
        snapshot.wifi_radio_on,
        snapshot.pm_network,
        snapshot.pm_audio,
        snapshot.pm_audio_wake,
        snapshot.pm_audio_cpu,
        static_cast<unsigned long>(snapshot.display_partial_count),
        snapshot.display_partial_us,
        static_cast<unsigned long>(snapshot.display_full_count),
        snapshot.display_full_us,
        stack_value_or_zero(snapshot.task_stack_low[0]),
        stack_value_or_zero(snapshot.task_stack_low[1]),
        stack_value_or_zero(snapshot.task_stack_low[2]),
        stack_value_or_zero(snapshot.task_stack_low[3]),
        stack_value_or_zero(snapshot.task_stack_low[4]),
        stack_value_or_zero(snapshot.task_stack_low[5]),
        stack_value_or_zero(snapshot.task_stack_low[6]),
        static_cast<unsigned long>(snapshot.event_count[0]),
        static_cast<unsigned long>(snapshot.event_count[1]),
        static_cast<unsigned long>(snapshot.event_count[2]),
        static_cast<unsigned long>(snapshot.event_count[3]),
        static_cast<unsigned long>(snapshot.event_count[4]));
}

void runtime_health_service_periodic()
{
    const int64_t now_us = esp_timer_get_time();
    bool emit = false;
    portENTER_CRITICAL(&s_runtime_health_mux);
    if (s_runtime_health.next_periodic_log_us == 0) {
        s_runtime_health.next_periodic_log_us =
            now_us + kRuntimeHealthPeriodicIntervalUs;
    } else if (runtime_health_periodic_due(
                   now_us, s_runtime_health.next_periodic_log_us)) {
        s_runtime_health.next_periodic_log_us =
            now_us + kRuntimeHealthPeriodicIntervalUs;
        emit = true;
    }
    portEXIT_CRITICAL(&s_runtime_health_mux);
    if (emit) {
        runtime_health_log_snapshot("periodic");
    }
}
