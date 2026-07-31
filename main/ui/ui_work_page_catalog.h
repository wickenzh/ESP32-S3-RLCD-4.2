// 声明工作页名称、启用状态和自定义顺序的共享接口。
#pragma once

#include <stddef.h>
#include <stdint.h>

bool is_work_page_enabled(int page);
uint8_t work_page_enabled_mask_load();
void work_page_enabled_mask_store(uint8_t page_mask);
bool work_page_requires_network(int page);
bool work_page_uses_low_refresh_idle(int page);
uint8_t normalize_work_page_enabled_mask(uint8_t page_mask);
uint8_t work_page_mask_for_offline_mode(uint8_t page_mask);
const char *work_page_name(int page);
int display_settings_item_work_page(int item);
int first_enabled_work_page();
int next_enabled_work_page(int current_page);
int first_enabled_work_page_order_index();
int next_enabled_work_page_order_index(int current_order_index);
int valid_enabled_work_page_order_index(int current_order_index);
bool work_page_mask_has_valid_home(uint8_t page_mask);
bool work_page_order_has_valid_home();
bool work_page_order_copy(uint8_t *order, size_t order_size);
void work_page_order_replace(const uint8_t *order, size_t order_size);
bool swap_work_page_order_entries_preserving_home(int first_index, int second_index);
void ensure_active_work_page_enabled();
void reset_work_page_order();
void normalize_work_page_order();
