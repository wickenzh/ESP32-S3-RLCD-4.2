// 统一定义屏幕尺寸和 RLCD 局部刷新策略常量，避免显示叶子模块依赖完整应用状态。
#pragma once

inline constexpr int kDisplayWidth = 400;
inline constexpr int kDisplayHeight = 300;
inline constexpr int kDisplayPartialMaxWidth = (kDisplayWidth * 7) / 10;
inline constexpr int kMaxFlushRanges = 8;
inline constexpr int kFlushRangeMergeGap = 8;
inline constexpr int kDisplayFlushDiagIntervalMs = 60 * 60 * 1000;

static_assert(kDisplayWidth > 0 && kDisplayHeight > 0,
              "display dimensions must be positive");
static_assert(kDisplayPartialMaxWidth > 0 &&
                  kDisplayPartialMaxWidth <= kDisplayWidth,
              "partial refresh threshold must fit the display width");
static_assert(kMaxFlushRanges > 0,
              "display flush range capacity must be positive");
static_assert(kFlushRangeMergeGap >= 0,
              "display flush range merge gap must be non-negative");
static_assert(kDisplayFlushDiagIntervalMs > 0,
              "display flush diagnostic interval must be positive");
