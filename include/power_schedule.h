#pragma once

#include <cstdint>

// 亮屏时段与显示参数来自 Pages docs/settings.json。
// power：可多段；每段 ≥30 分、空隙 ≥15 分、最多 6 段；失败回退工作日 08–12 / 14–20。
// display.page_seconds：每屏停留秒数（5–120，默认 30）。

void power_schedule_begin();
void power_schedule_mark_network_time();
void power_schedule_poll();
uint32_t board_settings_page_ms();
