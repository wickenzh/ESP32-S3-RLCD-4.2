// 汇总固件长期运行的内存、任务栈、PM 锁、Wi-Fi 和显示健康指标。
#pragma once

#include "app_task_startup_policy.h"

#include <stdint.h>

enum class RuntimeHealthEvent : uint8_t {
    kTaskCreateFailure = 0,
    kHttpFailure,
    kHttpsResourceDeferred,
    kWifiStopFailure,
    kPmLockFailure,
    kCount,
};

struct RuntimeHealthSnapshot {
    uint32_t internal_free = 0;
    uint32_t internal_min_free = 0;
    uint32_t internal_largest = 0;
    uint32_t internal_largest_low = 0;
    uint32_t dma_free = 0;
    uint32_t dma_min_free = 0;
    uint32_t dma_largest = 0;
    uint32_t dma_largest_low = 0;
    uint32_t psram_free = 0;
    uint32_t psram_min_free = 0;
    uint64_t wifi_radio_on_ms = 0;
    uint64_t display_partial_us = 0;
    uint64_t display_full_us = 0;
    uint32_t display_partial_count = 0;
    uint32_t display_full_count = 0;
    uint32_t task_stack_low[static_cast<uint8_t>(RegularAppTaskId::kCount)] = {};
    uint32_t event_count[static_cast<uint8_t>(RuntimeHealthEvent::kCount)] = {};
    int pm_network = 0;
    int pm_audio = 0;
    int pm_audio_wake = 0;
    int pm_audio_cpu = 0;
    bool wifi_radio_on = false;
};

void runtime_health_record_current_task_stack(RegularAppTaskId id);
void runtime_health_record_wifi_radio_state(bool radio_on);
void runtime_health_record_display_flush(bool full_refresh,
                                         uint32_t duration_us);
void runtime_health_note_event(RuntimeHealthEvent event);
bool runtime_health_snapshot(RuntimeHealthSnapshot *out);
void runtime_health_log_snapshot(const char *reason);
void runtime_health_service_periodic();
