#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// 第一版只监测这些 bot。房间分区按职责固定在固件里；
// JSON 可不带 room，板子按 bot 名落位。

enum class BotRoom : uint8_t {
    Dispatch = 0,  // 总调度厅
    Books,         // 书线间
    Drama,         // 剧影间
    Scrolls,       // 卷宗间
    Machine,       // 机工间
    Kitchen,       // 膳房
    Travel,        // 出行间
    Errands,       // 杂务廊
    Studio,        // 画室
    Count
};

struct BotRosterEntry {
    const char* name;
    BotRoom room;
    uint16_t color;  // RGB565 accent for the geometric figure
};

inline constexpr BotRosterEntry kBotRoster[] = {
    {"主脑", BotRoom::Dispatch, 0xFDA0},  // orange
    {"书枢", BotRoom::Books, 0x7BEF},     // soft blue
    {"剧枢", BotRoom::Drama, 0xF81F},     // magenta
    {"卷枢", BotRoom::Scrolls, 0xFFE0},   // yellow
    {"机枢", BotRoom::Machine, 0x8410},   // gray-blue
    {"膳枢", BotRoom::Kitchen, 0x07E0},   // green
    {"旅伴", BotRoom::Travel, 0x07FF},    // cyan
    {"票匣", BotRoom::Travel, 0x05FF},    // teal-ish
    {"零工", BotRoom::Errands, 0xC618},   // light gray
    {"蜡笔", BotRoom::Studio, 0xF800},    // red
};

inline constexpr size_t kBotRosterCount = sizeof(kBotRoster) / sizeof(kBotRoster[0]);

inline constexpr const char* kRoomNames[] = {
    "总调度厅",
    "书线间",
    "剧影间",
    "卷宗间",
    "机工间",
    "膳房",
    "出行间",
    "杂务廊",
    "画室",
};

inline const BotRosterEntry* bots_find(const char* name)
{
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    for (size_t i = 0; i < kBotRosterCount; ++i) {
        if (strcmp(kBotRoster[i].name, name) == 0) {
            return &kBotRoster[i];
        }
    }
    return nullptr;
}

inline bool bots_is_watched(const char* name)
{
    return bots_find(name) != nullptr;
}

inline const char* bots_room_name(BotRoom room)
{
    const auto idx = static_cast<uint8_t>(room);
    if (idx >= static_cast<uint8_t>(BotRoom::Count)) {
        return "?";
    }
    return kRoomNames[idx];
}
