// 声明设置页菜单数量、确认状态和 KEY 导航接口。
#pragma once

#include "app_state.h"

int settings_secondary_count(int primary);
int clamp_settings_primary(int primary);
int clamp_settings_secondary(int primary, int selected);
int clamp_settings_selection_for_mode(int primary, int selected, bool page_toggle_mode);
void reset_settings_confirmation();
void reset_settings_navigation_state();
void enter_settings_primary_navigation();
void enter_settings_system_item_navigation(int selection);
void handle_settings_key_short();
void handle_settings_key_long();
