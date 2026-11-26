#pragma once

// 优先使用本地 include/config.h；没有则退回占位示例，保证第一次烧录不用先写密码。
#if __has_include("config.h")
#include "config.h"
#else
#warning "include/config.h 不存在，使用 config.example.h。Wi-Fi 不会连接。"
#include "config.example.h"
#endif

#ifndef WIFI_SSID
#error "config 缺少 WIFI_SSID"
#endif
#ifndef WIFI_PASSWORD
#error "config 缺少 WIFI_PASSWORD"
#endif
#ifndef STATUS_URL
#error "config 缺少 STATUS_URL"
#endif

inline bool app_wifi_configured()
{
    return WIFI_SSID[0] != '\0';
}

inline bool app_status_url_configured()
{
    const char* url = STATUS_URL;
    if (url == nullptr || url[0] == '\0') {
        return false;
    }
    const char* cursor = url;
    const char token[] = "192.168.x.x";
    const int token_len = 11;
    while (*cursor != '\0') {
        int i = 0;
        while (i < token_len && cursor[i] != '\0' && cursor[i] == token[i]) {
            ++i;
        }
        if (i == token_len) {
            return false;
        }
        ++cursor;
    }
    return true;
}
