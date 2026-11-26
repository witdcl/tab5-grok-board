#include "keyboard_input.h"

#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <Wire.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// 官方字符模式示例里的 Ext.Port1 引脚。默认 I2C 地址 0x6D 由库构造函数写入，这里不改地址。
constexpr int8_t kSda = 0;
constexpr int8_t kScl = 1;
constexpr int8_t kIrq = 50;
constexpr size_t kEchoMax = 80;

m5::unit::UnitUnified units;
m5::unit::UnitTab5Keyboard unit;

bool ready = false;
char line[120] = "键盘尚未初始化";
std::string echo;

void pop_utf8(std::string& text)
{
    if (text.empty()) {
        return;
    }
    text.pop_back();
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
        text.pop_back();
    }
}

void trim_front(std::string& text)
{
    while (text.size() > kEchoMax) {
        const unsigned char lead = static_cast<unsigned char>(text[0]);
        size_t len = 1;
        if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        }
        if (len > text.size()) {
            len = 1;
        }
        text.erase(0, len);
    }
}

bool consume_character(const m5::unit::tab5_keyboard::Event& event)
{
    bool changed = false;
    bool refresh = false;
    for (uint8_t i = 0; i < event.chr.length && event.chr.chars[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(event.chr.chars[i]);
        if (c == '\r' || c == '\n') {
            refresh = true;
            continue;
        }
        if (c == '\b' || c == 0x7F) {
            pop_utf8(echo);
            changed = true;
            continue;
        }
        if (c == '\t') {
            echo.push_back(' ');
            changed = true;
            continue;
        }
        echo.push_back(static_cast<char>(c));
        changed = true;
    }
    trim_front(echo);
    Serial.printf("[keyboard] chars=\"%s\" refresh=%d\n", event.chr.chars, refresh ? 1 : 0);
    return refresh;
}

}  // namespace

void keyboard_start()
{
    auto cfg = unit.config();
    cfg.mode = m5::unit::tab5_keyboard::Mode::Character;
    cfg.start_periodic = true;
    cfg.irq_pin = kIrq;
    unit.config(cfg);

    // 官方示例在 M5.begin() 之后把 Wire 改到 Ext.Port1。
    // 键盘必须在 Tab5 关机时插入，不能热插这排针。
    Serial.printf("Tab5 keyboard I2C SDA=%d SCL=%d INT=%d\n", kSda, kScl, kIrq);
    Wire.end();
    Wire.begin(kSda, kScl, unit.component_config().clock);
    if (!units.add(unit, Wire) || !units.begin()) {
        ready = false;
        std::snprintf(line, sizeof(line), "未检测到键盘。关机后装到 Ext.Port1");
        Serial.println("UnitTab5Keyboard begin failed; continuing without it");
        return;
    }

    ready = true;
    std::snprintf(line, sizeof(line), "键盘已连接 Character 模式 0x6D fw 0x%02X", unit.firmwareVersion());
    Serial.printf("UnitTab5Keyboard firmware 0x%02X\n", unit.firmwareVersion());
}

KeyboardPulse keyboard_poll()
{
    KeyboardPulse pulse{};
    if (!ready) {
        return pulse;
    }

    units.update();
    while (!unit.empty()) {
        const auto event = unit.oldest();
        if (event.type == m5::unit::tab5_keyboard::EventType::Character) {
            if (consume_character(event)) {
                pulse.refresh = true;
            }
            pulse.dirty = true;
        }
        unit.discard();
    }
    return pulse;
}

bool keyboard_ready()
{
    return ready;
}

const char* keyboard_line()
{
    return line;
}

const char* keyboard_echo()
{
    return echo.c_str();
}
