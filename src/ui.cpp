#include "ui.h"

#include "bots_roster.h"

#include <M5GFX.h>
#include <M5Unified.h>

#include <cstdio>
#include <cstring>

namespace {

// 配色贴近第一版骨架：黑底 + 亮面板，避免 Tab5 上看成「全黑」。
constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kWall = 0x7BEF;
constexpr uint16_t kFloor = 0x3186;
constexpr uint16_t kHall = 0x4208;
constexpr uint16_t kText = TFT_WHITE;
constexpr uint16_t kMuted = 0xC618;
constexpr uint16_t kAccent = TFT_CYAN;
constexpr uint16_t kWarn = TFT_ORANGE;
constexpr uint16_t kOk = TFT_GREEN;
constexpr uint16_t kError = TFT_RED;
constexpr uint16_t kBubble = 0x528A;

auto& lcd = M5.Display;

struct RoomRect {
    BotRoom id;
    int x;
    int y;
    int w;
    int h;
};

// 1280×720 主区房间布局（去掉顶栏与底栏后的坐标在 draw 时再加偏移）。
constexpr RoomRect kRooms[] = {
    {BotRoom::Dispatch, 16, 8, 300, 150},
    {BotRoom::Books, 328, 8, 220, 150},
    {BotRoom::Drama, 560, 8, 220, 150},
    {BotRoom::Scrolls, 792, 8, 220, 150},
    {BotRoom::Machine, 16, 170, 260, 140},
    {BotRoom::Kitchen, 288, 170, 240, 140},
    {BotRoom::Travel, 540, 170, 300, 140},
    {BotRoom::Errands, 16, 322, 260, 120},
    {BotRoom::Studio, 288, 322, 240, 120},
};

constexpr int kHallX = 852;
constexpr int kHallY = 170;
constexpr int kHallW = 412;
constexpr int kHallH = 272;

const StatusItem* find_item(const StatusSnapshot* board, const char* bot)
{
    if (board == nullptr || !board->ok) {
        return nullptr;
    }
    for (uint8_t i = 0; i < board->count; ++i) {
        if (strcmp(board->items[i].bot, bot) == 0) {
            return &board->items[i];
        }
    }
    return nullptr;
}

const char* effective_status(const StatusItem* item)
{
    if (item == nullptr || item->status[0] == '\0') {
        return "hold";
    }
    return item->status;
}

bool is_outside(const char* status)
{
    return strcmp(status, "run") == 0 || strcmp(status, "error") == 0;
}

uint16_t status_ring(const char* status)
{
    if (strcmp(status, "run") == 0) {
        return kOk;
    }
    if (strcmp(status, "done") == 0) {
        return kAccent;
    }
    if (strcmp(status, "error") == 0) {
        return kError;
    }
    return kMuted;
}

const RoomRect* room_rect(BotRoom room)
{
    for (const auto& r : kRooms) {
        if (r.id == room) {
            return &r;
        }
    }
    return nullptr;
}

void paint_hello()
{
    lcd.fillScreen(kBg);
    lcd.setTextDatum(top_left);
    lcd.setFont(&fonts::FreeMonoBoldOblique24pt7b);
    lcd.setTextColor(kAccent, kBg);
    lcd.drawString("Hello Tab5", 28, 24);
    lcd.setFont(&fonts::efontCN_16);
    lcd.setTextColor(kMuted, kBg);
    lcd.drawString("正在初始化键盘和 Wi-Fi", 32, 78);
}

void draw_room(const RoomRect& room, int ox, int oy)
{
    const int x = ox + room.x;
    const int y = oy + room.y;
    lcd.fillRect(x, y, room.w, room.h, kFloor);
    lcd.drawRect(x, y, room.w, room.h, kWall);
    lcd.setFont(&fonts::efontCN_16);
    lcd.setTextColor(kMuted, kFloor);
    lcd.drawString(bots_room_name(room.id), x + 10, y + 8);
}

void draw_figure(int cx, int cy, uint16_t body, uint16_t ring, const char* label)
{
    lcd.fillCircle(cx, cy - 10, 14, body);
    lcd.drawCircle(cx, cy - 10, 14, ring);
    lcd.fillRoundRect(cx - 10, cy + 4, 20, 22, 4, body);
    lcd.drawRoundRect(cx - 10, cy + 4, 20, 22, 4, ring);
    lcd.setFont(&fonts::efontCN_16);
    lcd.setTextDatum(top_center);
    lcd.setTextColor(kText, kFloor);
    lcd.drawString(label, cx, cy + 30);
    lcd.setTextDatum(top_left);
}

void draw_bubble(int x, int y, const char* text, uint16_t bg)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    lcd.setFont(&fonts::efontCN_16);
    const int pad = 8;
    const int tw = lcd.textWidth(text);
    const int th = lcd.fontHeight();
    const int bw = tw + pad * 2;
    const int bh = th + pad;
    int bx = x;
    if (bx + bw > lcd.width() - 12) {
        bx = lcd.width() - 12 - bw;
    }
    if (bx < 12) {
        bx = 12;
    }
    lcd.fillRoundRect(bx, y, bw, bh, 6, kBubble);
    lcd.drawRoundRect(bx, y, bw, bh, 6, bg);
    lcd.setTextColor(kText, kBubble);
    lcd.drawString(text, bx + pad, y + pad / 2);
}

void slot_in_room(const RoomRect& room, int ox, int oy, int slot, int total, int& cx, int& cy)
{
    const int x = ox + room.x;
    const int y = oy + room.y;
    const int margin = 36;
    const int usable_w = room.w - margin * 2;
    if (total <= 1) {
        cx = x + room.w / 2;
    } else {
        cx = x + margin + (usable_w * slot) / (total - 1);
    }
    cy = y + room.h / 2 + 8;
}

void hall_slot(int index, int& cx, int& cy)
{
    const int col = index % 3;
    const int row = index / 3;
    cx = kHallX + 70 + col * 120;
    cy = kHallY + 70 + row * 90;
}

}  // namespace

void ui_begin()
{
    if (lcd.height() > lcd.width()) {
        lcd.setRotation(3);
    }
    // Tab5 MIPI 屏若背光为 0 会整屏黑，看起来像没画面。
    lcd.setBrightness(255);
    lcd.wakeup();
    delay(30);
    paint_hello();
    Serial.printf("UI begin display %dx%d brightness=%u\n", lcd.width(), lcd.height(),
                  static_cast<unsigned>(lcd.getBrightness()));
}

void ui_draw(const UiState& state)
{
    const int width = lcd.width();
    const int height = lcd.height();
    const int header_h = 96;
    const int footer_h = 72;
    const int ox = 0;
    const int oy = header_h;

    if (lcd.getBrightness() < 128) {
        lcd.setBrightness(255);
    }
    lcd.fillScreen(kBg);
    lcd.setTextDatum(top_left);

    lcd.setFont(&fonts::FreeMonoBoldOblique24pt7b);
    lcd.setTextColor(kAccent, kBg);
    lcd.drawString("Hello Tab5", 24, 12);

    lcd.setFont(&fonts::efontCN_16);
    lcd.setTextColor(kMuted, kBg);
    lcd.drawString("旁路观察看板 · 短 task 气泡 · 5 秒刷新", 28, 58);

    char meta[160];
    std::snprintf(meta, sizeof(meta), "%s   |   %s",
                  state.wifi_line != nullptr ? state.wifi_line : "Wi-Fi 未知",
                  state.keyboard_line != nullptr ? state.keyboard_line : "键盘未知");
    lcd.setTextColor(kText, kBg);
    lcd.drawString(meta, 520, 20);
    if (state.notice != nullptr && state.notice[0] != '\0') {
        lcd.setTextColor(kMuted, kBg);
        lcd.drawString(state.notice, 520, 48);
    }
    ui_draw_updated_at(state);

    // 房间
    for (const auto& room : kRooms) {
        draw_room(room, ox, oy);
    }

    // 走廊 / 出门区
    lcd.fillRect(ox + kHallX, oy + kHallY, kHallW, kHallH, kHall);
    lcd.drawRect(ox + kHallX, oy + kHallY, kHallW, kHallH, kWall);
    lcd.setFont(&fonts::efontCN_16);
    lcd.setTextColor(kMuted, kHall);
    lcd.drawString("走廊 · 出门干活", ox + kHallX + 12, oy + kHallY + 8);

    if (state.board != nullptr && !state.board->ok && state.board->error[0] != '\0') {
        lcd.setTextColor(kWarn, kHall);
        lcd.drawString(state.board->error, ox + kHallX + 12, oy + kHallY + 40);
    }

    // 先统计每间屋里待命人数，方便站位
    uint8_t inside_count[static_cast<uint8_t>(BotRoom::Count)] = {};
    uint8_t inside_index[kBotRosterCount];
    uint8_t outside_count = 0;

    for (size_t i = 0; i < kBotRosterCount; ++i) {
        const auto& entry = kBotRoster[i];
        const StatusItem* item = find_item(state.board, entry.name);
        const char* st = effective_status(item);
        if (is_outside(st)) {
            inside_index[i] = 0xFF;
            ++outside_count;
        } else {
            const auto room_i = static_cast<uint8_t>(entry.room);
            inside_index[i] = inside_count[room_i]++;
        }
    }

    uint8_t outside_slot = 0;
    for (size_t i = 0; i < kBotRosterCount; ++i) {
        const auto& entry = kBotRoster[i];
        const StatusItem* item = find_item(state.board, entry.name);
        const char* st = effective_status(item);
        const uint16_t ring = status_ring(st);
        int cx = 0;
        int cy = 0;

        if (is_outside(st)) {
            hall_slot(outside_slot++, cx, cy);
            cx += ox;
            cy += oy;
            // 走廊底色不同，临时改 label 背景
            lcd.fillCircle(cx, cy - 10, 14, entry.color);
            lcd.drawCircle(cx, cy - 10, 14, ring);
            lcd.fillRoundRect(cx - 10, cy + 4, 20, 22, 4, entry.color);
            lcd.drawRoundRect(cx - 10, cy + 4, 20, 22, 4, ring);
            lcd.setFont(&fonts::efontCN_16);
            lcd.setTextDatum(top_center);
            lcd.setTextColor(kText, kHall);
            lcd.drawString(entry.name, cx, cy + 30);
            lcd.setTextDatum(top_left);
            if (item != nullptr && item->task[0] != '\0') {
                draw_bubble(cx - 40, cy - 48, item->task, ring);
            }
        } else {
            const RoomRect* rr = room_rect(entry.room);
            if (rr == nullptr) {
                continue;
            }
            slot_in_room(*rr, ox, oy, inside_index[i], inside_count[static_cast<uint8_t>(entry.room)], cx, cy);
            draw_figure(cx, cy, entry.color, ring, entry.name);
            if (strcmp(st, "done") == 0 && item != nullptr && item->task[0] != '\0') {
                draw_bubble(cx - 40, cy - 52, item->task, kAccent);
            }
        }
    }

    (void)outside_count;

    // 底栏
    const int foot_y = height - footer_h;
    lcd.fillRect(0, foot_y, width, footer_h, kBg);
    lcd.drawFastHLine(0, foot_y, width, kWall);
    lcd.setFont(&fonts::efontCN_16);
    lcd.setTextColor(kText, kBg);
    char echo[140];
    const char* typed = (state.echo_line != nullptr && state.echo_line[0] != '\0') ? state.echo_line : "（还没有按键）";
    std::snprintf(echo, sizeof(echo), "回显  %s", typed);
    lcd.drawString(echo, 24, foot_y + 12);
    lcd.setTextColor(kMuted, kBg);
    lcd.drawString("Enter 刷新    hold 在房 / run·error 出门    只显示短 task", 24, foot_y + 40);
}

void ui_draw_updated_at(const UiState& state)
{
    constexpr int kX = 520;
    constexpr int kY = 72;
    constexpr int kW = 740;
    constexpr int kH = 22;
    lcd.fillRect(kX, kY, kW, kH, kBg);
    if (state.board == nullptr || state.board->updated_at[0] == '\0') {
        return;
    }
    char when[72];
    std::snprintf(when, sizeof(when), "更新 %s（北京时间）", state.board->updated_at);
    lcd.setFont(&fonts::efontCN_16);
    lcd.setTextDatum(top_left);
    lcd.setTextColor(kMuted, kBg);
    lcd.drawString(when, kX, kY);
}
