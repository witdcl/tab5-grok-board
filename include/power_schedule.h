#pragma once

#include <cstdint>

// 亮屏时段由 Pages docs/settings.json 的 power 字段配置（可多段、可增删）。
// 限制（与设置页一致）：
//   - 每段至少 30 分钟
//   - 相邻段空隙至少 15 分钟（RTC 仅到分钟；开机+校时约 1–2 分钟，过短会刚醒又睡）
//   - 最多 6 段
// 拉取失败时回退默认：工作日 08–12、14–20。

void power_schedule_begin();
void power_schedule_mark_network_time();
void power_schedule_poll();
