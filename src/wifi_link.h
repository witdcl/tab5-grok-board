#pragma once

// Tab5 的 Wi-Fi 在 ESP32-C6 上，不在 ESP32-P4 里。
// 初始化顺序来自官方示例（先 setPins，再 STA，再 begin）：
// https://docs.m5stack.com/en/arduino/m5tab5/wifi
// https://docs.m5stack.com/zh_CN/arduino/m5tab5/wifi

enum class WifiPhase {
    NotConfigured,
    Unavailable,
    Starting,
    Connected,
    Failed,
};

void wifi_link_start();
// 返回 true 表示屏幕上的那一行状态变了。
bool wifi_link_poll();
void wifi_link_retry();
WifiPhase wifi_link_phase();
bool wifi_link_connected();
const char* wifi_link_line();
