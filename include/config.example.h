#pragma once

// 复制本文件为 include/config.h，再在本机填写。
// include/config.h 已在 .gitignore 中，不要把真实密码提交进仓库。
//
// WIFI_SSID 留空表示不启动 Wi-Fi，屏幕仍会显示 Hello Tab5。
// STATUS_URL 里的 192.168.x.x 是占位符，固件会把它当成“尚未配置”。

#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// 应用层状态文档。这不是 Grok Bot 的官方接口。
#define STATUS_URL "http://192.168.x.x:8787/status.json"

// 微信读书荐书板：每日片单（GitHub Pages 或局域网旁路）
// Pages 示例：https://<user>.github.io/tab5-grok-board/weread_day.json
#define WEREAD_DAY_URL "https://YOUR_USER.github.io/tab5-grok-board/weread_day.json"
#define WEREAD_BOOK_URL "https://YOUR_USER.github.io/tab5-grok-board/weread_book.json"
