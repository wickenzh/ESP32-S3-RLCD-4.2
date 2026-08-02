// 声明各 UI 页面私有绘制缓存的细粒度失效入口。
#pragma once

void invalidate_clock_time_draw_cache();
void invalidate_clock_date_draw_cache();
void invalidate_clock_second_progress_draw_cache();
void invalidate_status_gif_draw_cache();
void invalidate_work_status_draw_cache();
void invalidate_history_draw_cache();
