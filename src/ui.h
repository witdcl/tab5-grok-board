#pragma once

#include "status_client.h"

struct UiState {
    const char* wifi_line;
    const char* keyboard_line;
    const char* echo_line;
    const char* notice;
    const StatusSnapshot* board;
};

void ui_begin();
void ui_draw(const UiState& state);
// 只重画顶栏「更新时间」一行，避免整屏闪。
void ui_draw_updated_at(const UiState& state);
