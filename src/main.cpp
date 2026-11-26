#include <M5Unified.h>

#include "app_config.h"
#include "keyboard_input.h"
#include "status_client.h"
#include "ui.h"
#include "wifi_link.h"

#include <cstdio>
#include <cstring>

// loopTask 默认约 8KB；取数 + 比对会爆栈（Stack protection fault → 蓝屏重启）。
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

namespace {

constexpr uint32_t kFetchPeriodMs = 5000;

StatusSnapshot board{};
StatusSnapshot fetch_buf{};
StatusItem items_before[kMaxStatusItems];
char notice[160] = "骨架已启动。没有键盘或 Wi-Fi 时，屏幕仍然保留。";
char notice_before[sizeof(notice)];
char error_before[sizeof(board.error)];
char updated_before[sizeof(board.updated_at)];
uint32_t last_fetch_ms = 0;
bool ever_fetched = false;
bool force_fetch = false;

void remember_notice(const char* text)
{
    std::snprintf(notice, sizeof(notice), "%s", text == nullptr ? "" : text);
}

void show_blocking_error(const char* message)
{
    remember_notice(message);
    if (board.ok) {
        return;
    }
    board.count = 0;
    board.updated_at[0] = '\0';
    std::snprintf(board.error, sizeof(board.error), "%s", message);
}

UiState current_view()
{
    UiState view{};
    view.wifi_line = wifi_link_line();
    view.keyboard_line = keyboard_line();
    view.echo_line = keyboard_echo();
    view.notice = notice;
    view.board = &board;
    return view;
}

void fetch_now()
{
    if (!wifi_link_connected()) {
        show_blocking_error(wifi_link_line());
        return;
    }
    if (!app_status_url_configured()) {
        show_blocking_error("STATUS_URL 仍是占位符。复制 config.example.h 为 config.h 再填写。");
        return;
    }

    if (!status_fetch(STATUS_URL, fetch_buf)) {
        remember_notice(fetch_buf.error[0] != '\0' ? fetch_buf.error : "拉取失败");
        if (!board.ok) {
            board = fetch_buf;
        }
        return;
    }
    board = fetch_buf;
    // 成功拉取不反复改 notice，避免无谓整屏重画。
    if (std::strcmp(notice, "已拉取 status.json") != 0) {
        remember_notice("已拉取 status.json");
    }
}

bool service_network()
{
    const WifiPhase phase = wifi_link_phase();
    const bool connected = phase == WifiPhase::Connected;

    if (force_fetch && !connected) {
        if (phase == WifiPhase::Starting) {
            remember_notice("Wi-Fi 连上后会拉取 JSON");
            return false;
        }
        fetch_now();
        force_fetch = false;
        return true;
    }

    if (!connected) {
        return false;
    }
    if (!app_status_url_configured()) {
        if (!force_fetch) {
            return false;
        }
        fetch_now();
        force_fetch = false;
        return true;
    }

    const bool due = !ever_fetched || (millis() - last_fetch_ms >= kFetchPeriodMs);
    if (!force_fetch && !due) {
        return false;
    }

    fetch_now();
    force_fetch = false;
    ever_fetched = true;
    last_fetch_ms = millis();
    return true;
}

}  // namespace

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    delay(50);
    M5.Display.setBrightness(255);

    // 先把字画上去。后面的键盘 I2C 或 Wi-Fi 失败，也不要把屏幕停在黑的。
    ui_begin();
    Serial.println("Hello Tab5");
    Serial.println("Status board skeleton. JSON contract is local, not a Grok Bot API.");

    keyboard_start();
    wifi_link_start();

    board.ok = false;
    std::snprintf(board.error, sizeof(board.error), "%s", "还没有拉取任务");
    // 键盘改线后背光偶发掉到 0，再拉一次并重画。
    M5.Display.setBrightness(255);
    ui_draw(current_view());
}

void loop()
{
    M5.update();

    const KeyboardPulse keys = keyboard_poll();
    if (keys.refresh) {
        force_fetch = true;
        if (!wifi_link_connected()) {
            wifi_link_retry();
        }
    }

    const bool wifi_dirty = wifi_link_poll();

    std::snprintf(notice_before, sizeof(notice_before), "%s", notice);
    std::snprintf(error_before, sizeof(error_before), "%s", board.error);
    std::snprintf(updated_before, sizeof(updated_before), "%s", board.updated_at);
    const bool ok_before = board.ok;
    const uint8_t count_before = board.count;
    for (uint8_t i = 0; i < count_before; ++i) {
        items_before[i] = board.items[i];
    }

    (void)service_network();

    bool items_dirty = count_before != board.count;
    if (!items_dirty) {
        for (uint8_t i = 0; i < board.count; ++i) {
            if (std::strcmp(items_before[i].bot, board.items[i].bot) != 0 ||
                std::strcmp(items_before[i].status, board.items[i].status) != 0 ||
                std::strcmp(items_before[i].task, board.items[i].task) != 0) {
                items_dirty = true;
                break;
            }
        }
    }
    const bool board_dirty = ok_before != board.ok || items_dirty ||
                             std::strcmp(notice_before, notice) != 0 ||
                             std::strcmp(error_before, board.error) != 0;
    const bool clock_dirty = std::strcmp(updated_before, board.updated_at) != 0;

    if (keys.dirty || keys.refresh || wifi_dirty || board_dirty) {
        ui_draw(current_view());
    } else if (clock_dirty) {
        // 仅时间变化：不 fillScreen，避免闪屏。
        ui_draw_updated_at(current_view());
    }

    delay(10);
}
