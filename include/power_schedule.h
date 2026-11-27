#pragma once

#include <cstdint>

// 亮屏时段与显示参数来自 Pages docs/settings.json。
// power：可多段；每段 ≥30 分、空隙 ≥15 分、最多 6 段；失败回退工作日 08–12 / 14–20。
// display：
//   page_seconds  每屏停留（5–120，默认 30）
//   book_seconds  每本停留后再换书（15–1800，默认 90）
//   brightness    亮度百分比（10–100，默认 100）

void power_schedule_begin();
void power_schedule_mark_network_time();
void power_schedule_poll();
uint32_t board_settings_page_ms();
uint32_t board_settings_book_ms();
uint8_t board_settings_brightness();  // 0–255
void board_settings_apply_brightness();
