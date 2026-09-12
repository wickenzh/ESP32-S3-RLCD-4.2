// 提供设置页各模式的 SDL 静态预览构建入口。
#pragma once

void build_settings_preview_page(const char *mode);

struct WebDemoState;
void build_web_settings_page(const WebDemoState &state, const char *version);
