#include <M5Unified.h>

#include "app_config.h"
#include "keyboard_input.h"
#include "power_schedule.h"
#include "weread_book.h"
#include "weread_ui.h"
#include "wifi_link.h"

#include <cstdio>
#include <cstring>

// loopTask：片单 JSON + 多封面缓冲需要更大栈
SET_LOOP_TASK_STACK_SIZE(64 * 1024);

namespace {

// 片单一天一换；白天偶尔复核即可
constexpr uint32_t kFetchPeriodMs = 30 * 60 * 1000;
constexpr uint32_t kRetryFailMs = 15000;

WereadDay day{};
WereadDay fetch_buf{};
uint32_t last_fetch_ms = 0;
bool ever_fetched = false;
bool force_fetch = false;
char last_date[16] = {};
char last_updated[40] = {};
int16_t touch_x0 = -1;

bool weread_url_ok()
{
#if defined(WEREAD_DAY_URL)
    const char* url = WEREAD_DAY_URL;
#elif defined(WEREAD_BOOK_URL)
    const char* url = WEREAD_BOOK_URL;
#else
    return false;
#endif
    if (url == nullptr || url[0] == '\0') {
        return false;
    }
    const char token[] = "192.168.x.x";
    for (const char* c = url; *c; ++c) {
        int i = 0;
        while (i < 11 && c[i] && c[i] == token[i]) {
            ++i;
        }
        if (i == 11) {
            return false;
        }
    }
    return true;
}

const char* day_url()
{
#if defined(WEREAD_DAY_URL)
    return WEREAD_DAY_URL;
#elif defined(WEREAD_BOOK_URL)
    return WEREAD_BOOK_URL;
#else
    return "";
#endif
}

void do_fetch()
{
    if (!wifi_link_connected()) {
        WereadBook err{};
        err.ok = false;
        std::snprintf(err.error, sizeof(err.error), "%s", wifi_link_line());
        weread_ui_set_book(err);
        return;
    }
    if (!weread_url_ok()) {
        WereadBook err{};
        err.ok = false;
        std::snprintf(err.error, sizeof(err.error), "%s",
                      "WEREAD_DAY_URL 未配置。在 config.h 填写 Pages 片单地址。");
        weread_ui_set_book(err);
        return;
    }

    if (!weread_day_fetch(day_url(), fetch_buf)) {
        if (!day.ok) {
            WereadBook err{};
            err.ok = false;
            std::snprintf(err.error, sizeof(err.error), "%s", fetch_buf.error);
            weread_ui_set_book(err);
        }
        return;
    }

    const bool changed = !day.ok ||
                         std::strcmp(last_date, fetch_buf.date) != 0 ||
                         std::strcmp(last_updated, fetch_buf.updated_at) != 0;
    day = fetch_buf;
    if (changed) {
        std::snprintf(last_date, sizeof(last_date), "%s", day.date);
        std::snprintf(last_updated, sizeof(last_updated), "%s", day.updated_at);
        weread_ui_set_day(day);
    }
}

}  // namespace

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    delay(50);
    M5.Display.setBrightness(255);

    weread_ui_begin();
    Serial.println("Tab5 WeRead board (day playlist)");
    power_schedule_begin();

    keyboard_start();
    wifi_link_start();

    WereadBook boot{};
    boot.ok = false;
    std::snprintf(boot.error, sizeof(boot.error), "%s", "连上 Wi-Fi 后拉取今日片单");
    weread_ui_set_book(boot);
    M5.Display.setBrightness(255);
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

    if (M5.Touch.getCount() > 0) {
        const auto t = M5.Touch.getDetail(0);
        if (t.wasPressed()) {
            touch_x0 = t.x;
        }
        if (t.wasReleased() && touch_x0 >= 0) {
            const int dx = t.x - touch_x0;
            if (dx <= -40) {
                weread_ui_next();
            } else if (dx >= 40) {
                weread_ui_prev();
            } else if (dx > -12 && dx < 12) {
                weread_ui_next();
            }
            touch_x0 = -1;
        }
    }

    (void)wifi_link_poll();

    // 亮屏窗外会 RTC 休眠，本调用可能不返回
    power_schedule_poll();

    const uint32_t period = day.ok ? kFetchPeriodMs : kRetryFailMs;
    const bool due = !ever_fetched || (millis() - last_fetch_ms >= period);
    if (force_fetch || due) {
        do_fetch();
        force_fetch = false;
        ever_fetched = true;
        last_fetch_ms = millis();
    }

    weread_ui_tick();
    delay(10);
}
