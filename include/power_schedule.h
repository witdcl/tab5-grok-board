#pragma once

#include <cstdint>

// 每日亮屏：08:00–12:00、14:00–20:00；其余时段 RTC timerSleep 至下一开机点。
// 开机后有短暂宽限，方便烧录后看串口；校时成功后才会真正休眠。

void power_schedule_begin();
// HTTP Date / 其它网络校时成功时调用，作为 NTP 备份可信源
void power_schedule_mark_network_time();
// 在 loop 里周期调用。若进入休眠，本函数不返回。
void power_schedule_poll();
