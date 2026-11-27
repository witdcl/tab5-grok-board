#include "weread_ui.h"

#include "power_schedule.h"
#include "wifi_link.h"

#include <HTTPClient.h>
#include <M5GFX.h>
#include <M5Unified.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint16_t kInkDeep = 0x0841;  // #0c0b0a
constexpr uint16_t kInk = 0x1082;
constexpr uint16_t kPaper = 0xEF5A;
constexpr uint16_t kPaperWarm = 0xF71C;  // 近 #f3ebe0
constexpr uint16_t kPaperSoft = 0xE6F7;
constexpr uint16_t kBrass = 0xC4CA;
constexpr uint16_t kBrassDim = 0x8C40;
constexpr uint16_t kMuted = 0x9492;
constexpr uint16_t kMuted2 = 0x7BEF;
constexpr uint16_t kText = 0x2104;
constexpr uint16_t kBody = 0x4A69;
constexpr uint16_t kBodySoft = 0x632C;
constexpr uint16_t kQuoteDim = 0x8C71;   // 金句非重点
constexpr uint16_t kQuoteHit = 0xF71C;   // 金句重点（亮）

// 先压暗旧页 → 暗中换帧 → 再亮起新页，避免滑入硬边
constexpr uint32_t kFadeOutMs = 480;
constexpr uint32_t kFadeInMs = 820;
constexpr int kDimBri = 22;  // 换帧时足够暗，切页不易察觉
constexpr size_t kCoverCap = 350000;

auto& lcd = M5.Display;
LovyanGFX* g_gfx = &lcd;

WereadBook g_book{};
WereadBook g_books[kWereadDayMax]{};
int g_book_n = 0;
int g_book_i = 0;
uint8_t g_page = 0;
uint32_t g_page_since = 0;
uint32_t g_book_since = 0;

struct CoverSlot {
    uint8_t* jpg = nullptr;
    size_t len = 0;
    int32_t w = 0;
    int32_t h = 0;
    char url[256] = {};
    bool ok = false;
};
CoverSlot g_covers[kWereadDayMax]{};

bool g_has_cover = false;
uint8_t* g_jpg = nullptr;
size_t g_jpg_len = 0;
int32_t g_jpg_w = 0;
int32_t g_jpg_h = 0;

LovyanGFX& gfx()
{
    return *g_gfx;
}

enum class Trans : uint8_t { Idle, FadeOut, FadeIn };
Trans g_trans = Trans::Idle;
uint32_t g_trans_t0 = 0;
uint8_t g_pending_page = 0;
M5Canvas* g_slide = nullptr;

float ease_in_out_cubic(float t)
{
    if (t <= 0.f) {
        return 0.f;
    }
    if (t >= 1.f) {
        return 1.f;
    }
    if (t < 0.5f) {
        return 4.f * t * t * t;
    }
    const float u = -2.f * t + 2.f;
    return 1.f - (u * u * u) / 2.f;
}

void free_slide()
{
    if (g_slide != nullptr) {
        g_slide->deleteSprite();
        delete g_slide;
        g_slide = nullptr;
    }
    g_gfx = &lcd;
}

uint8_t target_bri()
{
    return board_settings_brightness();
}

void end_trans()
{
    if (g_slide != nullptr) {
        g_slide->pushSprite(0, 0);
    }
    free_slide();
    g_trans = Trans::Idle;
    g_page_since = millis();
    lcd.setBrightness(target_bri());
}

// 目标字号（px）。efont 只有 16/24；优先整数倍（16/24/32/48）避免锯齿。
void font_px(int px, bool bold = false)
{
    if (px <= 0) {
        px = 16;
    }
    if (px % 24 == 0) {
        gfx().setFont(bold ? &fonts::efontCN_24_b : &fonts::efontCN_24);
        gfx().setTextSize(px / 24);
        return;
    }
    if (px % 16 == 0) {
        gfx().setFont(bold ? &fonts::efontCN_16_b : &fonts::efontCN_16);
        gfx().setTextSize(px / 16);
        return;
    }
    // 非整数倍：退回最近底图缩放（会有锯齿）
    if (px <= 20) {
        gfx().setFont(bold ? &fonts::efontCN_16_b : &fonts::efontCN_16);
        gfx().setTextSize(static_cast<float>(px) / 16.0f);
    } else {
        gfx().setFont(bold ? &fonts::efontCN_24_b : &fonts::efontCN_24);
        gfx().setTextSize(static_cast<float>(px) / 24.0f);
    }
}

// 48px：24 粗 ×2
void font_x2()
{
    font_px(48, true);
}

// 行首禁止的标点（中英）
bool is_leading_punct(const char* piece, int len)
{
    if (piece == nullptr || len <= 0) {
        return false;
    }
    if (len == 1) {
        const char c = piece[0];
        return c == ',' || c == '.' || c == '!' || c == '?' || c == ';' || c == ':' ||
               c == ')' || c == ']' || c == '}' || c == '"' || c == '\'';
    }
    static const char* const kForbid[] = {
        "。", "，", "、", "！", "？", "；", "：", "》", "」", "』", "）", "】", "…", "”", "’",
        nullptr};
    for (int i = 0; kForbid[i] != nullptr; ++i) {
        size_t n = 0;
        while (kForbid[i][n] != '\0') {
            ++n;
        }
        if (static_cast<int>(n) == len && std::memcmp(piece, kForbid[i], n) == 0) {
            return true;
        }
    }
    return false;
}

void copy_utf8(char* dst, size_t n, const char* src)
{
    if (n == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, n, "%s", src);
}

bool jpeg_dims(const uint8_t* data, size_t len, int32_t* out_w, int32_t* out_h)
{
    if (data == nullptr || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    size_t i = 2;
    while (i + 9 < len) {
        if (data[i] != 0xFF) {
            ++i;
            continue;
        }
        const uint8_t marker = data[i + 1];
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        if (i + 3 >= len) {
            break;
        }
        const uint16_t seglen = static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            *out_h = (data[i + 5] << 8) | data[i + 6];
            *out_w = (data[i + 7] << 8) | data[i + 8];
            return *out_w > 0 && *out_h > 0;
        }
        if (seglen < 2) {
            break;
        }
        i += 2u + seglen;
    }
    return false;
}

int utf8_next(const char* s, size_t& i)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == 0) {
        return 0;
    }
    int len = 1;
    if ((c & 0x80) == 0) {
        len = 1;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
    }
    i += static_cast<size_t>(len);
    return len;
}

void flush_run(char* buf, size_t& blen, int& x_off, int x, int y, int line_h, int line)
{
    if (blen == 0) {
        return;
    }
    buf[blen] = '\0';
    gfx().drawString(buf, x + x_off, y + line * line_h);
    x_off += gfx().textWidth(buf);
    blen = 0;
}

// hit 为 text 内连续子串时换 hit_fg；hit 空则整段 fg。句内换色只多几次 drawString，开销很小。
void draw_wrapped_hit(const char* text, const char* hit, int x, int y, int max_w, int line_h,
                      uint16_t fg, uint16_t hit_fg, uint16_t bg, int max_lines)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    size_t hit_lo = static_cast<size_t>(-1);
    size_t hit_hi = static_cast<size_t>(-1);
    if (hit != nullptr && hit[0] != '\0') {
        const char* p = std::strstr(text, hit);
        if (p != nullptr) {
            hit_lo = static_cast<size_t>(p - text);
            hit_hi = hit_lo + std::strlen(hit);
        }
    }

    gfx().setTextDatum(top_left);
    uint16_t cur_fg = fg;
    gfx().setTextColor(cur_fg, bg);
    size_t i = 0;
    int line = 0;
    char buf[192];
    size_t blen = 0;
    int bw = 0;
    int x_off = 0;
    while (text[i] != '\0' && line < max_lines) {
        const size_t start = i;
        const int len = utf8_next(text, i);
        if (len <= 0) {
            break;
        }
        if (len == 1 && (text[start] == '\n' || text[start] == '\r' || text[start] == ' ' ||
                         text[start] == '\t')) {
            continue;
        }
        if (len == 3 && text[start] == '\xE3' && text[start + 1] == '\x80' &&
            text[start + 2] == '\x80') {
            continue;
        }
        char piece[8] = {};
        for (int k = 0; k < len && k < 7; ++k) {
            piece[k] = text[start + static_cast<size_t>(k)];
        }
        if (blen == 0 && x_off == 0 && is_leading_punct(piece, len)) {
            continue;
        }
        const int pw = gfx().textWidth(piece);
        if (bw > 0 && bw + pw > max_w) {
            if (is_leading_punct(piece, len)) {
                // 标点粘行末
            } else {
                flush_run(buf, blen, x_off, x, y, line_h, line);
                ++line;
                bw = 0;
                x_off = 0;
                if (line >= max_lines) {
                    break;
                }
            }
        }
        const bool in_hit = (start >= hit_lo && start < hit_hi);
        const uint16_t want = in_hit ? hit_fg : fg;
        if (want != cur_fg) {
            flush_run(buf, blen, x_off, x, y, line_h, line);
            cur_fg = want;
            gfx().setTextColor(cur_fg, bg);
        }
        if (blen + static_cast<size_t>(len) >= sizeof(buf) - 1) {
            flush_run(buf, blen, x_off, x, y, line_h, line);
            if (is_leading_punct(piece, len) && x_off == 0) {
                continue;
            }
        }
        for (int k = 0; k < len; ++k) {
            buf[blen++] = piece[k];
        }
        bw += pw;
    }
    if (blen > 0 && line < max_lines) {
        flush_run(buf, blen, x_off, x, y, line_h, line);
    }
}

void draw_wrapped(const char* text, int x, int y, int max_w, int line_h, uint16_t fg, uint16_t bg,
                  int max_lines)
{
    draw_wrapped_hit(text, nullptr, x, y, max_w, line_h, fg, fg, bg, max_lines);
}

bool time_ready()
{
    return time(nullptr) >= 1700000000;
}

void clock_colors_for_page(uint8_t page, uint16_t* fg, uint16_t* bg)
{
    if (page == 2) {
        *fg = kText;
        *bg = kPaperSoft;
    } else {
        *fg = TFT_WHITE;
        *bg = kInkDeep;
    }
}

void paint_clock(uint16_t fg, uint16_t bg)
{
    const time_t now = time(nullptr);
    struct tm tm_local {};
    localtime_r(&now, &tm_local);
    char buf[16];
    if (!time_ready()) {
        std::snprintf(buf, sizeof(buf), "--:--");
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
    }
    // 先清右上角，避免旧数字残留（24px 时钟）
    gfx().fillRect(gfx().width() - 140, 0, 140, 48, bg);
    font_px(24, true);
    gfx().setTextDatum(top_right);
    gfx().setTextColor(fg, bg);
    gfx().drawString(buf, gfx().width() - 26, 14);
    gfx().setTextDatum(top_left);
}

void refresh_clock_if_needed()
{
    // 不用公网 NTP：家用网常拦 UDP/123，且曾对出错误年份；时间以 Mac server_now 为准
    static int last_min = -2;
    if (g_trans != Trans::Idle || g_slide != nullptr) {
        return;
    }
    const time_t now = time(nullptr);
    if (!time_ready()) {
        if (last_min != -1) {
            last_min = -1;
            uint16_t fg = TFT_WHITE;
            uint16_t bg = kInk;
            clock_colors_for_page(g_page, &fg, &bg);
            if (g_book.ok) {
                paint_clock(fg, bg);
            }
        }
        return;
    }
    struct tm tm_local {};
    localtime_r(&now, &tm_local);
    if (tm_local.tm_min == last_min) {
        return;
    }
    last_min = tm_local.tm_min;
    if (!g_book.ok) {
        return;
    }
    uint16_t fg = TFT_WHITE;
    uint16_t bg = kInk;
    clock_colors_for_page(g_page, &fg, &bg);
    paint_clock(fg, bg);
}

void paint_wifi_icon(uint16_t color)
{
    const int x = 28;
    const int y = 14;
    gfx().fillCircle(x + 12, y + 18, 2, color);
    gfx().drawCircle(x + 12, y + 14, 7, color);
    gfx().drawCircle(x + 12, y + 14, 12, color);
}

void paint_dots(uint8_t page, uint16_t on, uint16_t off)
{
    const int cx = gfx().width() / 2;
    const int y = gfx().height() - 26;
    for (int i = 0; i < 3; ++i) {
        const int x = cx + (i - 1) * 22;
        gfx().fillCircle(x, y, i == page ? 5 : 4, i == page ? on : off);
    }
}

void bind_cover_slot(int idx)
{
    g_has_cover = false;
    g_jpg = nullptr;
    g_jpg_len = 0;
    g_jpg_w = 0;
    g_jpg_h = 0;
    if (idx < 0 || idx >= g_book_n) {
        return;
    }
    CoverSlot& slot = g_covers[idx];
    if (!slot.ok || slot.jpg == nullptr || slot.len < 1000) {
        return;
    }
    g_jpg = slot.jpg;
    g_jpg_len = slot.len;
    g_jpg_w = slot.w;
    g_jpg_h = slot.h;
    g_has_cover = true;
}

bool ensure_cover_buf(CoverSlot& slot)
{
    if (slot.jpg != nullptr) {
        return true;
    }
    slot.jpg = static_cast<uint8_t*>(
        heap_caps_malloc(kCoverCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (slot.jpg == nullptr) {
        slot.jpg = static_cast<uint8_t*>(malloc(kCoverCap));
    }
    return slot.jpg != nullptr;
}

bool download_cover_slot_once(int idx, const char* url)
{
    if (idx < 0 || idx >= kWereadDayMax || url == nullptr || url[0] == '\0') {
        return false;
    }
    CoverSlot& slot = g_covers[idx];
    if (slot.ok && std::strcmp(slot.url, url) == 0) {
        return true;
    }
    slot.ok = false;
    slot.len = 0;
    slot.w = 0;
    slot.h = 0;
    slot.url[0] = '\0';
    if (!ensure_cover_buf(slot)) {
        Serial.printf("cover[%d] alloc fail\n", idx);
        return false;
    }

    // 封面 CDN 的 TLS：用静态客户端，避免堆上 new/delete 与 HTTPClient 生命周期打架导致空跳转
    static WiFiClientSecure cover_tls;
    cover_tls.setInsecure();
    cover_tls.setTimeout(20000);
    HTTPClient http;
    http.setConnectTimeout(12000);
    http.setTimeout(20000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(cover_tls, url)) {
        Serial.printf("cover[%d] begin fail\n", idx);
        return false;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("cover[%d] HTTP %d\n", idx, code);
        http.end();
        return false;
    }
    int len = http.getSize();
    if (len > static_cast<int>(kCoverCap)) {
        Serial.printf("cover[%d] bad size %d\n", idx, len);
        http.end();
        return false;
    }
    NetworkClient* stream = http.getStreamPtr();
    size_t got = 0;
    const uint32_t start = millis();
    const size_t cap = (len > 0) ? static_cast<size_t>(len) : kCoverCap;
    while (got < cap) {
        if (!http.connected() && !stream->available()) {
            break;
        }
        const size_t avail = stream->available();
        if (avail) {
            const size_t n = stream->readBytes(slot.jpg + got, avail);
            if (n == 0) {
                break;
            }
            got += n;
        } else {
            if (len > 0 && got >= static_cast<size_t>(len)) {
                break;
            }
            delay(2);
        }
        if (millis() - start > 25000) {
            break;
        }
    }
    http.end();
    if (got < 1000) {
        Serial.printf("cover[%d] short %u\n", idx, static_cast<unsigned>(got));
        return false;
    }
    // 简单校验 JPEG 头，避免半包当成功
    if (!(slot.jpg[0] == 0xFF && slot.jpg[1] == 0xD8)) {
        Serial.printf("cover[%d] not jpeg\n", idx);
        return false;
    }
    slot.len = got;
    if (!jpeg_dims(slot.jpg, slot.len, &slot.w, &slot.h)) {
        slot.w = 0;
        slot.h = 0;
    }
    copy_utf8(slot.url, sizeof(slot.url), url);
    slot.ok = true;
    Serial.printf("cover[%d] ok bytes=%u %dx%d\n", idx, static_cast<unsigned>(slot.len),
                  static_cast<int>(slot.w), static_cast<int>(slot.h));
    return true;
}

bool download_cover_slot(int idx, const char* url)
{
    if (idx < 0 || idx >= kWereadDayMax || url == nullptr || url[0] == '\0') {
        return false;
    }
    CoverSlot& slot = g_covers[idx];
    if (slot.ok && std::strcmp(slot.url, url) == 0) {
        return true;
    }
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (download_cover_slot_once(idx, url)) {
            return true;
        }
        Serial.printf("cover[%d] retry %d/3\n", idx, attempt);
        delay(350 * attempt);
    }
    return false;
}

void preload_all_covers()
{
    for (int i = 0; i < g_book_n; ++i) {
        (void)download_cover_slot(i, g_books[i].cover_url);
        delay(250);  // 错开 TLS，降低连发失败
    }
    // 第二轮专补失败的
    for (int i = 0; i < g_book_n; ++i) {
        if (g_covers[i].ok) {
            continue;
        }
        Serial.printf("cover[%d] second pass\n", i);
        delay(500);
        (void)download_cover_slot(i, g_books[i].cover_url);
    }
}

bool ensure_cover_ready(int idx)
{
    if (idx < 0 || idx >= g_book_n) {
        return false;
    }
    if (g_covers[idx].ok) {
        return true;
    }
    return download_cover_slot(idx, g_books[idx].cover_url);
}

void redraw();  // 背景补封面成功后刷新当前页

void retry_missing_covers_tick()
{
    static uint32_t last_try_ms = 0;
    static int next_i = 0;
    if (g_book_n <= 0) {
        return;
    }
    const uint32_t now = millis();
    if (now - last_try_ms < 8000) {
        return;
    }
    last_try_ms = now;
    for (int n = 0; n < g_book_n; ++n) {
        const int i = (next_i + n) % g_book_n;
        if (g_covers[i].ok) {
            continue;
        }
        next_i = (i + 1) % g_book_n;
        Serial.printf("cover[%d] background retry\n", i);
        if (download_cover_slot(i, g_books[i].cover_url) && i == g_book_i && g_page == 0) {
            bind_cover_slot(i);
            redraw();
        }
        return;  // 每次只补一本，避免卡 UI
    }
}

// object-fit: contain —— 横屏完整显示竖版封面，不裁切
void draw_cover_contain(int x, int y, int w, int h)
{
    gfx().fillRect(x, y, w, h, 0x2104);
    if (!(g_has_cover && g_jpg != nullptr && g_jpg_len > 0)) {
        font_px(28, true);
        gfx().setTextColor(kMuted, 0x2104);
        gfx().drawString("封面加载中", x + w / 2 - 72, y + h / 2);
        return;
    }

    float zoom = 0.0f;
    if (g_jpg_w > 0 && g_jpg_h > 0) {
        const float zx = static_cast<float>(w) / static_cast<float>(g_jpg_w);
        const float zy = static_cast<float>(h) / static_cast<float>(g_jpg_h);
        zoom = zx < zy ? zx : zy;
    }
    if (zoom <= 0.0f) {
        gfx().drawJpg(g_jpg, g_jpg_len, x, y, w, h, 0, 0, 0.0f, 0.0f);
        return;
    }
    gfx().drawJpg(g_jpg, g_jpg_len, x, y, w, h, 0, 0, zoom, zoom, middle_center);
}

void draw_page0()
{
    const int W = gfx().width();
    const int H = gfx().height();
    gfx().fillScreen(kInkDeep);
    gfx().fillRect(0, H * 58 / 100, W, H - H * 58 / 100, kInk);
    paint_wifi_icon(kMuted);
    paint_clock(TFT_WHITE, kInkDeep);

    // 左侧竖框：按 2:3 比例完整容纳封面
    const int cover_h = H * 72 / 100;
    const int cover_w = cover_h * 2 / 3;
    const int cover_x = W * 5 / 100;
    const int cover_y = (H - cover_h) / 2;
    gfx().drawRect(cover_x - 1, cover_y - 1, cover_w + 2, cover_h + 2, 0x6B4D);
    draw_cover_contain(cover_x, cover_y, cover_w, cover_h);

    const int qx = cover_x + cover_w + W * 5 / 100;
    const int qw = W - qx - W * 6 / 100;

    font_px(24, true);
    gfx().setTextColor(kBrass, kInkDeep);
    gfx().drawString("金句", qx, H * 22 / 100);

    font_px(32, false);
    gfx().drawFastVLine(qx, H * 30 / 100, H * 28 / 100, kBrass);
    draw_wrapped_hit(g_book.quote, g_book.quote_hit, qx + 16, H * 30 / 100, qw - 16, 44,
                     kQuoteDim, kQuoteHit, kInkDeep, 5);

    int meta_y = H * 68 / 100;
    if (g_book.why[0] != '\0') {
        font_px(24, true);
        gfx().setTextColor(kBrass, kInk);
        gfx().drawString(g_book.why, qx + 16, meta_y);
        meta_y += 36;
    }
    if (g_book.quote_source[0] != '\0') {
        font_px(16, true);
        gfx().setTextColor(kMuted2, kInk);
        gfx().drawString(g_book.quote_source, qx + 16, meta_y);
    }
    gfx().fillRect(qx + 16, H * 82 / 100, 36, 2, kBrass);
    paint_dots(0, kBrass, 0x4208);
}

void draw_page1()
{
    const int W = gfx().width();
    const int H = gfx().height();
    gfx().fillScreen(kInkDeep);
    paint_wifi_icon(kMuted);
    paint_clock(TFT_WHITE, kInkDeep);

    const int x = W * 5 / 100;
    const int y = H * 12 / 100;
    const int w = W - 2 * x;
    const int h = H - y - H * 10 / 100;
    gfx().fillRect(x, y, w, h, kPaperWarm);
    gfx().fillRect(x, y, w, 3, kBrass);

    const int pad = 48;
    char mast[200];
    std::snprintf(mast, sizeof(mast), "%s  ·  评分 %s  ·  %s",
                  g_book.category[0] ? g_book.category : "图书",
                  g_book.rating[0] ? g_book.rating : "-",
                  g_book.rating_count);
    font_px(16, true);
    gfx().setTextColor(kMuted2, kPaperWarm);
    gfx().drawString(mast, x + pad, y + 28);
    if (g_book.author[0] != '\0') {
        const int aw = gfx().textWidth(g_book.author);
        gfx().setTextColor(kText, kPaperWarm);
        gfx().drawString(g_book.author, x + w - pad - aw, y + 28);
    }
    gfx().drawFastHLine(x + pad, y + 58, w - 2 * pad, 0xC618);

    const char* eye = g_book.eye[0] ? g_book.eye : nullptr;
    int body_y = y + 80;
    if (eye != nullptr) {
        font_px(32, true);
        gfx().setTextColor(kText, kPaperWarm);
        gfx().drawString(eye, x + pad, body_y);
        const int ew = gfx().textWidth(eye);
        gfx().fillRect(x + pad, body_y + 38, ew, 3, kBrass);
        body_y += 54;
    }

    if (g_book.lede[0] != '\0') {
        font_px(24, true);
        draw_wrapped(g_book.lede, x + pad, body_y, w - 2 * pad, 34, kBrassDim, kPaperWarm, 2);
        body_y += 60;
    }

    font_px(24, false);
    draw_wrapped_hit(g_book.intro, g_book.intro_hit, x + pad, body_y, w - 2 * pad, 36,
                     kBodySoft, kBrassDim, kPaperWarm, 6);

    if (g_book.title[0] != '\0') {
        font_px(16, true);
        gfx().setTextColor(kMuted, kPaperWarm);
        char short_title[96];
        std::snprintf(short_title, sizeof(short_title), "%s", g_book.title);
        gfx().drawString(short_title, x + pad, y + h - 40);
    }
    paint_dots(1, kBrass, 0xC618);
}

void draw_page2()
{
    const int W = gfx().width();
    const int H = gfx().height();
    gfx().fillScreen(kPaperSoft);
    paint_wifi_icon(kMuted2);
    paint_clock(kText, kPaperSoft);

    // 角落水印「荐」——淡色大字，不抢正文
    font_px(48, true);
    gfx().setTextColor(0xD6B5, kPaperSoft);
    gfx().drawString("荐", W - 140, H * 12 / 100);

    const int x = W * 11 / 100;
    const int text_w = W - 2 * x;

    font_px(24, true);
    gfx().setTextColor(kBrassDim, kPaperSoft);
    gfx().drawString(g_book.voice[0] ? g_book.voice : "语气 · 一问", x, H * 22 / 100);

    int y = H * 30 / 100;
    if (g_book.recommend_lead[0] != '\0') {
        font_px(32, true);
        draw_wrapped_hit(g_book.recommend_lead, nullptr, x, y, text_w, 44, kText, kText,
                         kPaperSoft, 2);
        y += 100;
    }
    font_px(24, false);
    draw_wrapped_hit(g_book.recommend, g_book.recommend_hit, x, y, text_w, 36, kBodySoft, kText,
                     kPaperSoft, 5);

    gfx().fillRect(x, H * 78 / 100, 40, 2, kBrass);
    font_px(16, true);
    int foot_y = H * 82 / 100;
    if (g_book.why[0] != '\0') {
        gfx().setTextColor(kBrassDim, kPaperSoft);
        gfx().drawString(g_book.why, x, foot_y);
        foot_y += 28;
    }
    if (g_book.author[0] != '\0') {
        gfx().setTextColor(kMuted2, kPaperSoft);
        char foot[160];
        std::snprintf(foot, sizeof(foot), "%s · %s", g_book.author, g_book.title);
        gfx().drawString(foot, x, foot_y);
    }
    paint_dots(2, kBrass, 0xC618);
}

void draw_error()
{
    gfx().fillScreen(kInk);
    paint_wifi_icon(kMuted);
    paint_clock(TFT_WHITE, kInk);
    font_px(28, true);
    gfx().setTextColor(TFT_ORANGE, kInk);
    gfx().drawString(g_book.error[0] ? g_book.error : "还没有荐书数据", 80, 300);
    font_px(16, true);
    gfx().setTextColor(kMuted, kInk);
    gfx().drawString("请确认 Mac mini 上 weread_board.py 已启动", 80, 360);
}

void redraw()
{
    if (!g_book.ok) {
        draw_error();
        return;
    }
    if (g_page == 0) {
        draw_page0();
    } else if (g_page == 1) {
        draw_page1();
    } else {
        draw_page2();
    }
}

void swap_to_pending()
{
    g_page = g_pending_page;
    if (g_slide != nullptr) {
        // 暗场中一次落到最终位置，淡入只调亮度，不再二次偏移
        g_slide->pushSprite(0, 0);
    } else {
        g_gfx = &lcd;
        redraw();
    }
}

void tick_trans()
{
    if (g_trans == Trans::Idle) {
        return;
    }
    const uint32_t now = millis();
    if (g_trans == Trans::FadeOut) {
        const float raw = static_cast<float>(now - g_trans_t0) / static_cast<float>(kFadeOutMs);
        if (raw >= 1.f) {
            lcd.setBrightness(kDimBri);
            swap_to_pending();
            g_trans = Trans::FadeIn;
            g_trans_t0 = now;
            return;
        }
        const float t = ease_in_out_cubic(raw);
        const int hi = static_cast<int>(target_bri());
        const int bri = hi - static_cast<int>(t * static_cast<float>(hi - kDimBri));
        lcd.setBrightness(bri < kDimBri ? kDimBri : bri);
        return;
    }

    // FadeIn：只缓出亮度，画面位置已固定
    const float raw = static_cast<float>(now - g_trans_t0) / static_cast<float>(kFadeInMs);
    if (raw >= 1.f) {
        end_trans();
        return;
    }
    const float t = ease_in_out_cubic(raw);
    const int hi = static_cast<int>(target_bri());
    const int bri = kDimBri + static_cast<int>(t * static_cast<float>(hi - kDimBri));
    lcd.setBrightness(bri > hi ? hi : bri);
}

bool start_trans(uint8_t new_page, int /*dir*/)
{
    if (g_trans != Trans::Idle) {
        end_trans();
    }
    g_pending_page = new_page;

    // 离屏先画好新页；旧页仍留在屏上，先只做压暗
    auto* canvas = new M5Canvas(&lcd);
    canvas->setColorDepth(16);
    canvas->setPsram(true);
    if (!canvas->createSprite(lcd.width(), lcd.height())) {
        Serial.println("fade sprite fail → dim hard cut");
        delete canvas;
        g_slide = nullptr;
        g_trans = Trans::FadeOut;
        g_trans_t0 = millis();
        return false;
    }

    const uint8_t keep = g_page;
    g_page = new_page;
    g_slide = canvas;
    g_gfx = g_slide;
    redraw();
    g_gfx = &lcd;
    g_page = keep;  // 屏上仍是旧页，直到暗场 swap

    g_trans = Trans::FadeOut;
    g_trans_t0 = millis();
    return true;
}

void select_book(int idx, bool with_fade_to_page0)
{
    if (g_book_n <= 0) {
        return;
    }
    g_book_i = ((idx % g_book_n) + g_book_n) % g_book_n;
    g_book = g_books[g_book_i];
    (void)ensure_cover_ready(g_book_i);
    bind_cover_slot(g_book_i);
    g_page = 0;
    g_page_since = millis();
    g_book_since = millis();
    if (with_fade_to_page0 && g_book.ok) {
        (void)start_trans(0, 1);
    }
}

void advance_auto()
{
    // 到换书时间：切下一本并从第 0 屏开始；否则只在本内 0→1→2→0 循环
    if (millis() - g_book_since >= board_settings_book_ms()) {
        const int next = (g_book_i + 1) % g_book_n;
        g_book_i = next;
        g_book = g_books[g_book_i];
        (void)ensure_cover_ready(g_book_i);
        bind_cover_slot(g_book_i);
        g_book_since = millis();
        Serial.printf("rotate book %d/%d %s\n", g_book_i + 1, g_book_n, g_book.title);
        (void)start_trans(0, 1);
        return;
    }
    const uint8_t next_page = static_cast<uint8_t>((g_page + 1) % 3);
    (void)start_trans(next_page, 1);
}

void change_page(int delta)
{
    if (!g_book.ok || g_book_n <= 0) {
        return;
    }
    int page = static_cast<int>(g_page) + delta;
    int book = g_book_i;
    if (page > 2) {
        page = 0;
        book = (book + 1) % g_book_n;
    } else if (page < 0) {
        page = 2;
        book = (book - 1 + g_book_n) % g_book_n;
    }
    if (book != g_book_i) {
        g_book_i = book;
        g_book = g_books[g_book_i];
        (void)ensure_cover_ready(g_book_i);
        bind_cover_slot(g_book_i);
        g_book_since = millis();
        Serial.printf("rotate book %d/%d %s\n", g_book_i + 1, g_book_n, g_book.title);
    }
    (void)start_trans(static_cast<uint8_t>(page), delta > 0 ? 1 : -1);
}

}  // namespace

void weread_ui_begin()
{
    if (lcd.height() > lcd.width()) {
        lcd.setRotation(3);
    }
    board_settings_apply_brightness();
    g_gfx = &lcd;
    gfx().fillScreen(kInk);
    font_px(28, true);
    gfx().setTextColor(kBrass, kInk);
    gfx().drawString("荐书板启动中…", 80, 320);
    g_page = 0;
    g_book_i = 0;
    g_book_n = 0;
    g_page_since = millis();
    g_book_since = millis();
    g_trans = Trans::Idle;
    Serial.printf("weread ui %dx%d\n", lcd.width(), lcd.height());
}

void weread_ui_set_book(const WereadBook& book)
{
    if (g_trans != Trans::Idle) {
        end_trans();
    }
    g_books[0] = book;
    g_book_n = book.ok ? 1 : 0;
    g_book_i = 0;
    g_book = book;
    g_page = 0;
    g_page_since = millis();
    g_book_since = millis();
    if (g_book.ok) {
        (void)download_cover_slot(0, g_book.cover_url);
        bind_cover_slot(0);
    } else {
        bind_cover_slot(-1);
    }
    g_gfx = &lcd;
    if (g_book.ok) {
        (void)start_trans(0, 1);
    } else {
        redraw();
        board_settings_apply_brightness();
    }
}

void weread_ui_set_day(const WereadDay& day)
{
    if (g_trans != Trans::Idle) {
        end_trans();
    }
    g_book_n = 0;
    g_book_i = 0;
    auto show_err = [&](const char* msg) {
        g_book.ok = false;
        std::snprintf(g_book.error, sizeof(g_book.error), "%s", msg);
        bind_cover_slot(-1);
        redraw();
        board_settings_apply_brightness();
    };
    if (!day.ok || day.count <= 0) {
        show_err(day.error[0] ? day.error : "今日片单为空");
        return;
    }
    const int n = day.count > kWereadDayMax ? kWereadDayMax : day.count;
    for (int i = 0; i < n; ++i) {
        if (!day.books[i].ok) {
            continue;
        }
        g_books[g_book_n++] = day.books[i];
    }
    if (g_book_n <= 0) {
        show_err("今日片单无有效书");
        return;
    }
    Serial.printf("ui set day books=%d date=%s\n", g_book_n, day.date);
    preload_all_covers();
    select_book(0, true);
}

void weread_ui_tick()
{
    if (g_trans != Trans::Idle) {
        tick_trans();
        return;
    }
    refresh_clock_if_needed();
    if (!g_book.ok) {
        return;
    }
    retry_missing_covers_tick();
    if (millis() - g_page_since >= board_settings_page_ms()) {
        advance_auto();
    }
}

void weread_ui_next()
{
    change_page(1);
}

void weread_ui_prev()
{
    change_page(-1);
}

uint8_t weread_ui_page()
{
    return g_page;
}

int weread_ui_book_index()
{
    return g_book_i;
}
