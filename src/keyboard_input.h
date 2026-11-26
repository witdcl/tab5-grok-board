#pragma once

// Tab5 Keyboard，Ext.Port1，Character 模式。
// 调用顺序来自官方示例：M5.update() 在外面，这里做 Units.update() 并读事件队列。
// https://docs.m5stack.com/zh_CN/arduino/projects/tab5/tab5_keyboard
// https://docs.m5stack.com/zh_CN/tab5/Tab5_Keyboard

struct KeyboardPulse {
    bool dirty;
    // 字符模式里出现 CR 或 LF。官方示例把它们当成换行；本骨架用这一下触发刷新。
    bool refresh;
};

void keyboard_start();
KeyboardPulse keyboard_poll();
bool keyboard_ready();
const char* keyboard_line();
const char* keyboard_echo();
