// 将 LVGL 像素写入 RLCD，并选择局部或全屏刷新及记录诊断统计。
#include "ui_display_flush.h"

#include "active_work_page_state.h"
#include "app_display_config.h"
#include "app_hardware.h"
#include "app_metadata.h"
#include "ota_runtime_state.h"
#include "runtime_health.h"
#include "ui_display_diag_policy.h"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <inttypes.h>

namespace {
constexpr uint32_t kDisplayFullReasonSingleWide = 1U << 0;
constexpr uint32_t kDisplayFullReasonTooManyRanges = 1U << 1;
constexpr uint32_t kDisplayFullReasonCoveredWide = 1U << 2;
constexpr uint16_t kRlcdBlackThreshold = 0xC618;
#define DISPLAY_FLUSH_DIAG_LOG_FORMAT "display flush diag: page=%d partial=%lu ranges=%lu partial_us=%" PRIu64 " full=%lu full_us=%" PRIu64 " reason_single=%lu reason_covered=%lu reason_ranges=%lu"
#define DISPLAY_FULL_REASON_OVERLAP_ASSERT "display full-refresh reason bits must not overlap"

struct FlushRange {
    int x1;
    int x2;
};

struct DisplayFlushRuntimeState {
    FlushRange ranges[kMaxFlushRanges];
    int range_count;
    bool force_full_refresh;
    uint32_t full_reason_mask;
    uint32_t partial_cycles;
    uint32_t partial_ranges;
    uint32_t full_cycles;
    uint32_t full_single_wide;
    uint32_t full_covered_wide;
    uint32_t full_too_many_ranges;
    uint64_t partial_transfer_us;
    uint64_t full_transfer_us;
    TickType_t last_diag_tick;
    int last_diag_page;
    uint32_t initialized_magic;
};

constexpr uint32_t kDisplayFlushRuntimeInitializedMagic = 0x44535046U;
EXT_RAM_BSS_ATTR DisplayFlushRuntimeState s_display_flush_runtime;

DisplayFlushRuntimeState &display_flush_runtime()
{
    if (s_display_flush_runtime.initialized_magic !=
        kDisplayFlushRuntimeInitializedMagic) {
        s_display_flush_runtime = {};
        s_display_flush_runtime.last_diag_page = -1;
        s_display_flush_runtime.initialized_magic =
            kDisplayFlushRuntimeInitializedMagic;
    }
    return s_display_flush_runtime;
}

constexpr bool flush_ranges_touch(const FlushRange &range, int x1, int x2)
{
    return x1 <= range.x2 + kFlushRangeMergeGap &&
           x2 >= range.x1 - kFlushRangeMergeGap;
}

constexpr bool add_merged_flush_range(FlushRange *ranges, int *range_count, int x1, int x2)
{
    if (!ranges || !range_count || *range_count < 0 || *range_count > kMaxFlushRanges || x1 > x2) {
        return false;
    }
    int merged_x1 = x1;
    int merged_x2 = x2;
    for (int i = 0; i < *range_count;) {
        if (!flush_ranges_touch(ranges[i], merged_x1, merged_x2)) {
            ++i;
            continue;
        }
        if (ranges[i].x1 < merged_x1) {
            merged_x1 = ranges[i].x1;
        }
        if (ranges[i].x2 > merged_x2) {
            merged_x2 = ranges[i].x2;
        }
        --(*range_count);
        ranges[i] = ranges[*range_count];
        i = 0;
    }
    if (*range_count >= kMaxFlushRanges) {
        return false;
    }
    ranges[(*range_count)++] = {merged_x1, merged_x2};
    return true;
}

constexpr bool bridged_flush_ranges_merge_once()
{
    FlushRange ranges[kMaxFlushRanges] = {{0, 10}, {20, 30}};
    int range_count = 2;
    return add_merged_flush_range(ranges, &range_count, 8, 22) &&
           range_count == 1 &&
           ranges[0].x1 == 0 &&
           ranges[0].x2 == 30;
}

static_assert(kDisplayFullReasonSingleWide != 0, "display full-refresh single-wide reason bit must be nonzero");
static_assert(kDisplayFullReasonTooManyRanges != 0, "display full-refresh range-count reason bit must be nonzero");
static_assert(kDisplayFullReasonCoveredWide != 0, "display full-refresh covered-width reason bit must be nonzero");
static_assert((kDisplayFullReasonSingleWide & kDisplayFullReasonTooManyRanges) == 0,
              DISPLAY_FULL_REASON_OVERLAP_ASSERT);
static_assert((kDisplayFullReasonSingleWide & kDisplayFullReasonCoveredWide) == 0,
              DISPLAY_FULL_REASON_OVERLAP_ASSERT);
static_assert((kDisplayFullReasonTooManyRanges & kDisplayFullReasonCoveredWide) == 0,
              DISPLAY_FULL_REASON_OVERLAP_ASSERT);
static_assert(kRlcdBlackThreshold > 0, "RLCD black threshold must be nonzero");
static_assert(kMaxFlushRanges > 0, "display flush range capacity must be positive");
static_assert(kFlushRangeMergeGap >= 0, "display flush range merge gap must be non-negative");
static_assert(sizeof(DisplayFlushRuntimeState) == 136,
              "display flush runtime state must remain compact");
static_assert(bridged_flush_ranges_merge_once(),
              "bridging flush ranges must collapse into one covered interval");
} // namespace

void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    DisplayPort &display = app_display();
    DisplayFlushRuntimeState &runtime = display_flush_runtime();

    const OtaRuntimeFlagsSnapshot ota_at_flush_start =
        ota_runtime_flags_load();
    if (ota_at_flush_start.reboot_pending) {
        runtime.range_count = 0;
        runtime.force_full_refresh = false;
        runtime.full_reason_mask = 0;
        lv_disp_flush_ready(drv);
        return;
    }

    int clipped_x1 = area->x1 < 0 ? 0 : area->x1;
    int clipped_x2 = area->x2 >= kDisplayWidth ? kDisplayWidth - 1 : area->x2;
    int clipped_y1 = area->y1 < 0 ? 0 : area->y1;
    int clipped_y2 = area->y2 >= kDisplayHeight ? kDisplayHeight - 1 : area->y2;
    bool touches_visible_area = clipped_x1 <= clipped_x2 && clipped_y1 <= clipped_y2;
    if (touches_visible_area) {
        int area_x1 = clipped_x1;
        int area_x2 = clipped_x2;
        area_x1 &= ~1;
        area_x2 |= 1;
        if (area_x2 >= kDisplayWidth) {
            area_x2 = kDisplayWidth - 1;
        }
        if (area_x2 - area_x1 + 1 >= kDisplayPartialMaxWidth) {
            runtime.force_full_refresh = true;
            runtime.full_reason_mask |= kDisplayFullReasonSingleWide;
        } else if (!add_merged_flush_range(runtime.ranges,
                                           &runtime.range_count,
                                           area_x1,
                                           area_x2)) {
            runtime.force_full_refresh = true;
            runtime.full_reason_mask |= kDisplayFullReasonTooManyRanges;
        }
    }

    if (touches_visible_area) {
        int area_width = area->x2 - area->x1 + 1;
        uint16_t *buffer = (uint16_t *)color_map;
        for (int y = clipped_y1; y <= clipped_y2; ++y) {
            uint16_t *row = buffer + (y - area->y1) * area_width + (clipped_x1 - area->x1);
            for (int x = clipped_x1; x <= clipped_x2; ++x) {
                uint8_t color = (*row < kRlcdBlackThreshold) ? ColorBlack : ColorWhite;
                display.RLCD_SetPixel(x, y, color);
                ++row;
            }
        }
    }
    if (lv_disp_flush_is_last(drv)) {
        int covered_width = 0;
        for (int i = 0; i < runtime.range_count; ++i) {
            covered_width += runtime.ranges[i].x2 -
                             runtime.ranges[i].x1 + 1;
        }
        bool covered_wide = covered_width >= kDisplayPartialMaxWidth;
        if (covered_wide) {
            runtime.full_reason_mask |= kDisplayFullReasonCoveredWide;
        }
        if (runtime.force_full_refresh || covered_wide) {
            ++runtime.full_cycles;
            if (runtime.full_reason_mask & kDisplayFullReasonSingleWide) {
                ++runtime.full_single_wide;
            }
            if (runtime.full_reason_mask &
                kDisplayFullReasonTooManyRanges) {
                ++runtime.full_too_many_ranges;
            }
            if (runtime.full_reason_mask & kDisplayFullReasonCoveredWide) {
                ++runtime.full_covered_wide;
            }
            const int64_t transfer_started_us = esp_timer_get_time();
            display.RLCD_Display();
            const uint32_t transfer_us = static_cast<uint32_t>(
                esp_timer_get_time() - transfer_started_us);
            runtime.full_transfer_us += transfer_us;
            runtime_health_record_display_flush(true, transfer_us);
        } else if (runtime.range_count > 0) {
            ++runtime.partial_cycles;
            runtime.partial_ranges += runtime.range_count;
            const int64_t transfer_started_us = esp_timer_get_time();
            for (int i = 0; i < runtime.range_count; ++i) {
                display.RLCD_DisplayXRange(runtime.ranges[i].x1,
                                           runtime.ranges[i].x2);
            }
            const uint32_t transfer_us = static_cast<uint32_t>(
                esp_timer_get_time() - transfer_started_us);
            runtime.partial_transfer_us += transfer_us;
            runtime_health_record_display_flush(false, transfer_us);
        }
        TickType_t now_tick = xTaskGetTickCount();
        int active_page = active_work_page_load();
        const bool page_changed = runtime.last_diag_page != active_page;
        const bool diag_window_due =
            runtime.last_diag_tick == 0 ||
            now_tick - runtime.last_diag_tick >=
                pdMS_TO_TICKS(kDisplayFlushDiagIntervalMs) ||
            page_changed;
        OtaRuntimeFlagsSnapshot ota_for_diag = {};
        if (diag_window_due && runtime.full_cycles > 0) {
            ota_for_diag = ota_runtime_flags_load();
        }
        const DisplayFlushDiagDecision diag = display_flush_diag_decision(
            runtime.last_diag_tick == 0,
            now_tick - runtime.last_diag_tick >=
                pdMS_TO_TICKS(kDisplayFlushDiagIntervalMs),
            page_changed,
            runtime.full_cycles,
            ota_for_diag.state == kOtaUpdating,
            ota_for_diag.reboot_pending);
        if (diag.emit_log) {
            ESP_LOGI(TAG,
                     DISPLAY_FLUSH_DIAG_LOG_FORMAT,
                     active_page,
                     (unsigned long)runtime.partial_cycles,
                     (unsigned long)runtime.partial_ranges,
                     runtime.partial_transfer_us,
                     (unsigned long)runtime.full_cycles,
                     runtime.full_transfer_us,
                     (unsigned long)runtime.full_single_wide,
                     (unsigned long)runtime.full_covered_wide,
                     (unsigned long)runtime.full_too_many_ranges);
        }
        if (diag.close_window) {
            runtime.partial_cycles = 0;
            runtime.partial_ranges = 0;
            runtime.full_cycles = 0;
            runtime.full_single_wide = 0;
            runtime.full_covered_wide = 0;
            runtime.full_too_many_ranges = 0;
            runtime.partial_transfer_us = 0;
            runtime.full_transfer_us = 0;
            runtime.last_diag_tick = now_tick;
            runtime.last_diag_page = active_page;
        }
        runtime.range_count = 0;
        runtime.force_full_refresh = false;
        runtime.full_reason_mask = 0;
    }
    lv_disp_flush_ready(drv);
}
