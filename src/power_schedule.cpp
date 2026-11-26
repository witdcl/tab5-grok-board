#include "power_schedule.h"

#include "wifi_link.h"

#include <M5Unified.h>
#include <esp_sntp.h>

#include <cstdio>
#include <cstring>
#include <sys/time.h>
#include <time.h>

namespace {

// 亮屏窗： [8:00, 12:00) ∪ [14:00, 20:00)
constexpr int kOn1Begin = 8 * 60;
constexpr int kOn1End = 12 * 60;
constexpr int kOn2Begin = 14 * 60;
constexpr int kOn2End = 20 * 60;

// 开机宽限：可信校时成功后至少再亮这么久，避免一烧录就睡过去
constexpr uint32_t kBootGraceMs = 90 * 1000;
constexpr uint32_t kPreSleepHoldMs = 4000;
constexpr uint32_t kPollGapMs = 5000;

bool ntp_started = false;
bool network_time_ok = false;
bool clock_latched = false;  // 一旦可信校时成功就锁存，避免 SNTP 状态抖动导致永不休眠
uint32_t time_ready_ms = 0;
uint32_t last_poll_ms = 0;

void ensure_tz()
{
    setenv("TZ", "CST-8", 1);
    tzset();
}

bool year_plausible()
{
    const time_t now = time(nullptr);
    if (now < 1700000000LL) {
        return false;
    }
    struct tm local {};
    localtime_r(&now, &local);
    const int year = local.tm_year + 1900;
    return year >= 2025 && year <= 2035;
}

// 不能信上电时 RTC 里的脏时间（曾出现 2028 年），必须等 NTP 或 HTTP Date
bool trusted_clock_ok()
{
    if (!year_plausible()) {
        return false;
    }
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        return true;
    }
    return network_time_ok;
}

void start_ntp_if_needed()
{
    if (ntp_started || !wifi_link_connected()) {
        return;
    }
    ensure_tz();
    // 家用网偶发拦 pool.ntp.org；优先国内服务器
    configTzTime("CST-8", "ntp.aliyun.com", "ntp.tencent.com", "time.windows.com");
    ntp_started = true;
    Serial.println("power schedule: NTP started");
}

bool sync_rtc_from_system()
{
    if (!clock_latched && !trusted_clock_ok()) {
        return false;
    }
    if (!year_plausible()) {
        Serial.println("power schedule: skip RTC sync, year implausible");
        return false;
    }
    if (!M5.Rtc.isEnabled()) {
        Serial.println("power schedule: RTC not enabled");
        return false;
    }
    ensure_tz();
    const time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);

    m5::rtc_datetime_t dt {};
    dt.date.year = local.tm_year + 1900;
    dt.date.month = local.tm_mon + 1;
    dt.date.date = local.tm_mday;
    dt.date.weekDay = local.tm_wday;
    dt.time.hours = local.tm_hour;
    dt.time.minutes = local.tm_min;
    dt.time.seconds = local.tm_sec;
    M5.Rtc.setDateTime(&dt);
    Serial.printf("power schedule: RTC synced %04d-%02d-%02d %02d:%02d:%02d\n",
                  dt.date.year, dt.date.month, dt.date.date, dt.time.hours, dt.time.minutes,
                  dt.time.seconds);
    return true;
}

bool in_on_window(int minutes)
{
    return (minutes >= kOn1Begin && minutes < kOn1End) ||
           (minutes >= kOn2Begin && minutes < kOn2End);
}

// 关机窗内：返回下一开机的时、分
void next_wake(int minutes, int* wake_h, int* wake_m)
{
    if (minutes >= kOn1End && minutes < kOn2Begin) {
        *wake_h = 14;
        *wake_m = 0;
        return;
    }
    // [20:00, 24:00) ∪ [0:00, 8:00)
    *wake_h = 8;
    *wake_m = 0;
}

void show_sleep_splash(int wake_h, int wake_m)
{
    auto& lcd = M5.Display;
    lcd.setBrightness(180);
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setFont(&fonts::efontCN_24);
    char line1[48];
    char line2[48];
    std::snprintf(line1, sizeof(line1), "休眠至 %02d:%02d", wake_h, wake_m);
    std::snprintf(line2, sizeof(line2), "亮屏 08–12 · 14–20");
    lcd.drawString(line1, lcd.width() / 2, lcd.height() / 2 - 20);
    lcd.drawString(line2, lcd.width() / 2, lcd.height() / 2 + 24);
    lcd.setTextDatum(top_left);
}

void enter_timer_sleep(int wake_h, int wake_m)
{
    if (!sync_rtc_from_system()) {
        Serial.println("power schedule: skip sleep, RTC sync failed");
        return;
    }

    show_sleep_splash(wake_h, wake_m);
    Serial.printf("power schedule: timerSleep until %02d:%02d\n", wake_h, wake_m);
    delay(kPreSleepHoldMs);

    m5::rtc_time_t wake {};
    wake.hours = wake_h;
    wake.minutes = wake_m;
    wake.seconds = -1;  // RX8130 闹钟忽略秒
    M5.Power.timerSleep(wake);
    // 正常不会返回；若返回说明未真正断电
    Serial.println("power schedule: timerSleep returned (USB?)");
}

}  // namespace

void power_schedule_begin()
{
    ensure_tz();
    ntp_started = false;
    network_time_ok = false;
    clock_latched = false;
    time_ready_ms = 0;
    last_poll_ms = 0;
    Serial.println("power schedule: 08-12 & 14-20 on; else RTC sleep");
}

void power_schedule_mark_network_time()
{
    if (!year_plausible()) {
        return;
    }
    network_time_ok = true;
    Serial.println("power schedule: network time accepted");
}

void power_schedule_poll()
{
    const uint32_t now_ms = millis();
    if (last_poll_ms != 0 && (now_ms - last_poll_ms) < kPollGapMs) {
        return;
    }
    last_poll_ms = now_ms;

    start_ntp_if_needed();

    if (!clock_latched) {
        if (!trusted_clock_ok()) {
            return;
        }
        clock_latched = true;
        time_ready_ms = now_ms;
        sync_rtc_from_system();
        struct tm local {};
        const time_t t = time(nullptr);
        localtime_r(&t, &local);
        Serial.printf("power schedule: clock ready %02d:%02d (grace %us)\n", local.tm_hour,
                      local.tm_min, static_cast<unsigned>(kBootGraceMs / 1000));
    }

    if ((now_ms - time_ready_ms) < kBootGraceMs) {
        return;
    }

    ensure_tz();
    struct tm local {};
    const time_t t = time(nullptr);
    localtime_r(&t, &local);
    const int minutes = local.tm_hour * 60 + local.tm_min;

    if (in_on_window(minutes)) {
        return;
    }

    int wake_h = 8;
    int wake_m = 0;
    next_wake(minutes, &wake_h, &wake_m);
    enter_timer_sleep(wake_h, wake_m);
}
