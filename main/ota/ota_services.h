// 声明 OTA manifest、下载状态和 OTA 任务入口。
#pragma once
#include "app_state.h"

bool ota_flow_active();
void ota_reset_status_if_idle();
void ota_handle_info_key();
void ota_mark_running_app_valid();
void ota_task(void *);
