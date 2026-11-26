#include "wifi_link.h"

#include "app_config.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

namespace {

// 引脚与官方 Tab5 Wi-Fi STA 示例一致，对应产品管脚图里的 SDIO2：
//   CLK = G12, CMD = G13, D0 = G11, D1 = G10, D2 = G9, D3 = G8, RST = G15
// esp32-p4-evboard 自带的 BOARD_SDIO_ESP_HOSTED_* 是乐鑫 EV 板
// （CLK 18 / CMD 19 / D0-D3 14-17 / RST 54），不能用在 Tab5 上。
// 官方示例也写了：没选 M5Tab5 板型时，调用 setPins 传入这组 GPIO。
constexpr int8_t kSdioClk = 12;
constexpr int8_t kSdioCmd = 13;
constexpr int8_t kSdioD0 = 11;
constexpr int8_t kSdioD1 = 10;
constexpr int8_t kSdioD2 = 9;
constexpr int8_t kSdioD3 = 8;
constexpr int8_t kSdioRst = 15;

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kRetryGapMs = 15000;
// ESP-Hosted 上 WiFi.status() 会短暂抖一下；立刻标 Failed 会整屏重画，看起来像蓝屏重启。
constexpr uint32_t kDisconnectGraceMs = 4000;

WifiPhase phase = WifiPhase::NotConfigured;
char line[128] = "Wi-Fi 未初始化";
uint32_t mark_ms = 0;
uint32_t disconnect_since_ms = 0;
bool pins_ready = false;

void set_line(const char* text)
{
    std::snprintf(line, sizeof(line), "%s", text);
}

#if CONFIG_ESP_WIFI_REMOTE_ENABLED

const char* status_name(int status)
{
    switch (status) {
        case WL_NO_SSID_AVAIL:
            return "找不到 SSID";
        case WL_CONNECT_FAILED:
            return "认证失败";
        case WL_CONNECTION_LOST:
            return "连接中断";
        case WL_DISCONNECTED:
            return "未连接";
        case WL_IDLE_STATUS:
            return "空闲";
        default:
            return "未连接";
    }
}

bool start_join()
{
    if (!pins_ready) {
        // 必须在 WiFi.mode / WiFi.begin 之前。官方示例同顺序。
        const bool pins_ok = WiFi.setPins(kSdioClk, kSdioCmd, kSdioD0, kSdioD1, kSdioD2, kSdioD3, kSdioRst);
        if (!pins_ok) {
            phase = WifiPhase::Failed;
            mark_ms = millis();
            set_line("Wi-Fi setPins 失败");
            Serial.println("WiFi.setPins returned false");
            return false;
        }
        pins_ready = true;
        WiFi.mode(WIFI_STA);
        Serial.println("WiFi STA pins set for Tab5 SDIO2");
    }

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    phase = WifiPhase::Starting;
    mark_ms = millis();
    char ssid_short[25];
    std::snprintf(ssid_short, sizeof(ssid_short), "%s", WIFI_SSID);
    std::snprintf(line, sizeof(line), "Wi-Fi 连接中：%s", ssid_short);
    Serial.printf("WiFi.begin ssid=%s\n", ssid_short);
    return true;
}

void remember_connected()
{
    phase = WifiPhase::Connected;
    String ip = WiFi.localIP().toString();
    std::snprintf(line, sizeof(line), "Wi-Fi 已连接 %s", ip.c_str());
}

#else

bool start_join()
{
    phase = WifiPhase::Unavailable;
    set_line("Wi-Fi 不可用：核心未启用 ESP-Hosted");
    Serial.println("CONFIG_ESP_WIFI_REMOTE_ENABLED is off.");
    Serial.println("Official example: https://docs.m5stack.com/en/arduino/m5tab5/wifi");
    return false;
}

#endif

}  // namespace

void wifi_link_start()
{
    if (!app_wifi_configured()) {
        phase = WifiPhase::NotConfigured;
        set_line("Wi-Fi 未配置（WIFI_SSID 为空）");
        Serial.println("Wi-Fi skipped: WIFI_SSID is empty");
        return;
    }
    start_join();
}

void wifi_link_retry()
{
    if (!app_wifi_configured()) {
        phase = WifiPhase::NotConfigured;
        set_line("Wi-Fi 未配置（WIFI_SSID 为空）");
        return;
    }
    if (phase == WifiPhase::Unavailable) {
        return;
    }
    if (phase == WifiPhase::Starting && millis() - mark_ms < 3000) {
        return;
    }
    start_join();
}

bool wifi_link_poll()
{
    const WifiPhase before = phase;
    char previous[sizeof(line)];
    std::snprintf(previous, sizeof(previous), "%s", line);

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    if (!pins_ready) {
        return phase != before || std::strcmp(previous, line) != 0;
    }
    if (phase == WifiPhase::Starting || phase == WifiPhase::Connected || phase == WifiPhase::Failed) {
        const int status = WiFi.status();
        if (status == WL_CONNECTED) {
            disconnect_since_ms = 0;
            if (phase != WifiPhase::Connected) {
                remember_connected();
                Serial.printf("WiFi connected %s\n", WiFi.localIP().toString().c_str());
            }
        } else if (phase == WifiPhase::Starting && millis() - mark_ms >= kConnectTimeoutMs) {
            phase = WifiPhase::Failed;
            mark_ms = millis();
            disconnect_since_ms = 0;
            std::snprintf(line, sizeof(line), "Wi-Fi 超时：%s", status_name(status));
            Serial.println(line);
        } else if (phase == WifiPhase::Connected) {
            if (disconnect_since_ms == 0) {
                disconnect_since_ms = millis();
            } else if (millis() - disconnect_since_ms >= kDisconnectGraceMs) {
                phase = WifiPhase::Failed;
                mark_ms = millis();
                disconnect_since_ms = 0;
                std::snprintf(line, sizeof(line), "Wi-Fi 中断：%s", status_name(status));
                Serial.println(line);
            }
        } else if (phase == WifiPhase::Failed && millis() - mark_ms >= kRetryGapMs) {
            start_join();
        }
        // Starting 阶段不改秒数文案；Connected 短抖动也不改 phase，避免整屏闪。
    }
#else
    (void)kConnectTimeoutMs;
    (void)kRetryGapMs;
#endif

    return phase != before || std::strcmp(previous, line) != 0;
}

WifiPhase wifi_link_phase()
{
    return phase;
}

bool wifi_link_connected()
{
    return phase == WifiPhase::Connected;
}

const char* wifi_link_line()
{
    return line;
}
