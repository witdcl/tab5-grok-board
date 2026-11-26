#pragma once

#include <stddef.h>
#include <stdint.h>

// 应用层状态文档，不是 Grok Bot 的公开 API。
// 由本机旁路观察服务生成；bot 不专门为看板汇报。
//
// {
//   "updated_at": "2026-11-24 10:01:48",  // 北京时间墙钟
//   "items": [
//     {"bot": "主脑", "status": "hold", "task": "等音线开闸", "note": "..."}
//   ]
// }
//
// status: hold | run | done | error
// 第一版屏幕只显示短 task；note 可保留供本机排查。

static constexpr uint8_t kMaxStatusItems = 10;

struct StatusItem {
    char bot[32];
    char status[24];
    char task[96];
    char note[128];
};

struct StatusSnapshot {
    bool ok;
    char updated_at[40];
    char error[160];
    StatusItem items[kMaxStatusItems];
    uint8_t count;
};

// 用已经连通的 TCP/IP 做一次 HTTP GET，并按上面的字段解析。
// 只保留 bots_roster 白名单里的 bot。
bool status_fetch(const char* url, StatusSnapshot& out);
