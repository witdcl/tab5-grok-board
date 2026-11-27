#include "power_schedule.h"

#include "app_config.h"
#include "wifi_link.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFiClientSecure.h>
#include <esp_sntp.h>

#include <cstdio>
#include <cstring>
#include <sys/time.h>
#include <time.h>

namespace {

// —— 与设置页同一套硬限制 ——
constexpr int kMinOnMinutes = 30;
constexpr int kMinGapMinutes = 15;
constexpr int kMaxSegments = 6;

constexpr uint32_t kBootGraceMs = 90 * 1000;
constexpr uint32_t kPreSleepHoldMs = 4000;
constexpr uint32_t kPollGapMs = 5000;
constexpr uint32_t kSettingsReloadMs = 30 * 60 * 1000;

struct Segment {
    int start_min;  // [0, 24*60)
    int end_min;    // (start, 24*60]
};

struct PowerConfig {
    bool enabled = true;
    // ISO 星期：1=周一 … 7=周日；bit0 不用
    bool weekday[8] = {false, true, true, true, true, true, false, false};
    Segment segs[kMaxSegments] = {{8 * 60, 12 * 60}, {14 * 60, 20 * 60}};
    int seg_count = 2;
};

PowerConfig cfg {};
bool ntp_started = false;
bool network_time_ok = false;
bool clock_latched = false;
bool settings_loaded = false;
uint32_t time_ready_ms = 0;
uint32_t last_poll_ms = 0;
uint32_t last_settings_ms = 0;

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
    configTzTime("CST-8", "ntp.aliyun.com", "ntp.tencent.com", "time.windows.com");
    ntp_started = true;
    Serial.println("power schedule: NTP started");
}

bool sync_rtc_from_system()
{
    if (!clock_latched && !trusted_clock_ok()) {
        return false;
    }
    if (!year_plausible() || !M5.Rtc.isEnabled()) {
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

int parse_hhmm(const char* s)
{
    if (s == nullptr || std::strlen(s) < 4) {
        return -1;
    }
    int h = 0;
    int m = 0;
    if (std::sscanf(s, "%d:%d", &h, &m) != 2) {
        return -1;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return -1;
    }
    return h * 60 + m;
}

// ISO 1–7 ← tm_wday 0=Sun
int iso_wday(int tm_wday)
{
    return tm_wday == 0 ? 7 : tm_wday;
}

bool day_enabled(int iso)
{
    if (iso < 1 || iso > 7) {
        return false;
    }
    return cfg.weekday[iso];
}

bool validate_and_set(PowerConfig& out, JsonVariantConst power)
{
    PowerConfig next = out;
    if (power["enabled"].is<bool>()) {
        next.enabled = power["enabled"].as<bool>();
    }

    if (power["weekdays"].is<JsonArray>()) {
        for (int i = 1; i <= 7; ++i) {
            next.weekday[i] = false;
        }
        for (JsonVariantConst v : power["weekdays"].as<JsonArrayConst>()) {
            const int d = v.as<int>();
            if (d >= 1 && d <= 7) {
                next.weekday[d] = true;
            }
        }
        bool any = false;
        for (int i = 1; i <= 7; ++i) {
            any = any || next.weekday[i];
        }
        if (!any) {
            return false;
        }
    }

    if (power["segments"].is<JsonArray>()) {
        JsonArrayConst arr = power["segments"].as<JsonArrayConst>();
        Segment tmp[kMaxSegments] {};
        int n = 0;
        for (JsonVariantConst item : arr) {
            if (n >= kMaxSegments) {
                return false;
            }
            const int a = parse_hhmm(item["start"].as<const char*>());
            const int b = parse_hhmm(item["end"].as<const char*>());
            if (a < 0 || b < 0 || b <= a) {
                return false;
            }
            if ((b - a) < kMinOnMinutes) {
                return false;
            }
            tmp[n++] = {a, b};
        }
        if (n <= 0) {
            return false;
        }
        // 按开始时间排序
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                if (tmp[j].start_min < tmp[i].start_min) {
                    const Segment s = tmp[i];
                    tmp[i] = tmp[j];
                    tmp[j] = s;
                }
            }
        }
        for (int i = 0; i + 1 < n; ++i) {
            if (tmp[i + 1].start_min < tmp[i].end_min) {
                return false;  // 重叠
            }
            if ((tmp[i + 1].start_min - tmp[i].end_min) < kMinGapMinutes) {
                return false;  // 空隙过短
            }
        }
        // 跨午夜：最后一段结束 → 次日第一段开始
        const int wrap = (24 * 60 - tmp[n - 1].end_min) + tmp[0].start_min;
        if (wrap < kMinGapMinutes) {
            return false;
        }
        next.seg_count = n;
        for (int i = 0; i < n; ++i) {
            next.segs[i] = tmp[i];
        }
    }

    out = next;
    return true;
}

void apply_defaults()
{
    cfg = PowerConfig {};
    settings_loaded = false;
}

const char* settings_url()
{
#if defined(WEREAD_SETTINGS_URL)
    return WEREAD_SETTINGS_URL;
#elif defined(WEREAD_DAY_URL)
    // 由片单 URL 推导 settings.json
    static char buf[256];
    const char* day = WEREAD_DAY_URL;
    const char* slash = std::strrchr(day, '/');
    if (slash == nullptr) {
        return "";
    }
    const size_t n = static_cast<size_t>(slash - day + 1);
    if (n + 14 >= sizeof(buf)) {
        return "";
    }
    std::memcpy(buf, day, n);
    std::snprintf(buf + n, sizeof(buf) - n, "settings.json");
    return buf;
#else
    return "";
#endif
}

bool http_get_settings(String& body)
{
    const char* url = settings_url();
    if (url == nullptr || url[0] == '\0') {
        return false;
    }
    Serial.printf("power schedule GET %s\n", url);
    static WiFiClientSecure tls;
    HTTPClient http;
    http.setConnectTimeout(12000);
    http.setTimeout(20000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    tls.setInsecure();
    tls.setTimeout(20000);
    if (!http.begin(tls, url)) {
        tls.stop();
        return false;
    }
    http.addHeader("Accept-Encoding", "identity");
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        tls.stop();
        Serial.printf("power schedule HTTP %d\n", code);
        return false;
    }
    body = http.getString();
    http.end();
    tls.stop();
    return body.length() > 8;
}

void reload_settings_if_due(bool force)
{
    if (!wifi_link_connected()) {
        return;
    }
    const uint32_t now = millis();
    if (!force && settings_loaded && last_settings_ms != 0 &&
        (now - last_settings_ms) < kSettingsReloadMs) {
        return;
    }
    last_settings_ms = now;
    String body;
    if (!http_get_settings(body)) {
        if (!settings_loaded) {
            apply_defaults();
            Serial.println("power schedule: settings fetch fail → defaults");
        }
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.println("power schedule: settings JSON fail");
        return;
    }
    PowerConfig next = cfg;
    if (doc["power"].is<JsonObject>()) {
        if (!validate_and_set(next, doc["power"])) {
            Serial.println("power schedule: settings rejected (limits)");
            if (!settings_loaded) {
                apply_defaults();
            }
            return;
        }
    } else if (!settings_loaded) {
        apply_defaults();
        settings_loaded = true;
        Serial.println("power schedule: no power block → defaults");
        return;
    } else {
        return;
    }
    cfg = next;
    settings_loaded = true;
    Serial.printf("power schedule: loaded segs=%d enabled=%d\n", cfg.seg_count, cfg.enabled ? 1 : 0);
}

bool in_on_window(int iso, int minutes)
{
    if (!cfg.enabled) {
        return true;  // 关闭电源策略 = 常亮
    }
    if (!day_enabled(iso)) {
        return false;
    }
    for (int i = 0; i < cfg.seg_count; ++i) {
        if (minutes >= cfg.segs[i].start_min && minutes < cfg.segs[i].end_min) {
            return true;
        }
    }
    return false;
}

// 计算距下一亮屏起点的秒数（最多 8 天）
bool next_wake_epoch(time_t now, time_t* wake_out)
{
    struct tm local {};
    localtime_r(&now, &local);
    local.tm_sec = 0;
    local.tm_isdst = -1;

    for (int day = 0; day <= 8; ++day) {
        struct tm cand = local;
        cand.tm_mday += day;
        cand.tm_hour = 0;
        cand.tm_min = 0;
        cand.tm_sec = 0;
        const time_t day0 = mktime(&cand);
        if (day0 < 0) {
            continue;
        }
        struct tm day_tm {};
        localtime_r(&day0, &day_tm);
        const int iso = iso_wday(day_tm.tm_wday);
        if (!day_enabled(iso)) {
            continue;
        }
        for (int i = 0; i < cfg.seg_count; ++i) {
            struct tm w = day_tm;
            w.tm_hour = cfg.segs[i].start_min / 60;
            w.tm_min = cfg.segs[i].start_min % 60;
            w.tm_sec = 0;
            w.tm_isdst = -1;
            const time_t wake = mktime(&w);
            if (wake > now + 45) {
                *wake_out = wake;
                return true;
            }
        }
    }
    return false;
}

void show_sleep_splash(const struct tm& wake_tm)
{
    auto& lcd = M5.Display;
    lcd.setBrightness(180);
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setFont(&fonts::efontCN_24);
    char line1[48];
    char line2[48];
    std::snprintf(line1, sizeof(line1), "休眠至 %02d:%02d", wake_tm.tm_hour, wake_tm.tm_min);
    const char* wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    std::snprintf(line2, sizeof(line2), "周%s %02d-%02d", wd[wake_tm.tm_wday], wake_tm.tm_mon + 1,
                  wake_tm.tm_mday);
    lcd.drawString(line1, lcd.width() / 2, lcd.height() / 2 - 20);
    lcd.drawString(line2, lcd.width() / 2, lcd.height() / 2 + 24);
    lcd.setTextDatum(top_left);
}

void enter_sleep_until(time_t wake)
{
    if (!sync_rtc_from_system()) {
        Serial.println("power schedule: skip sleep, RTC sync failed");
        return;
    }
    struct tm wake_tm {};
    localtime_r(&wake, &wake_tm);
    show_sleep_splash(wake_tm);

    const time_t now = time(nullptr);
    int sec = static_cast<int>(wake - now);
    if (sec < 60) {
        sec = 60;
    }
    if (sec > 8 * 24 * 3600) {
        sec = 8 * 24 * 3600;
    }
    Serial.printf("power schedule: timerSleep %ds until %02d:%02d (wday=%d)\n", sec, wake_tm.tm_hour,
                  wake_tm.tm_min, wake_tm.tm_wday);
    delay(kPreSleepHoldMs);
    M5.Power.timerSleep(sec);
    Serial.println("power schedule: timerSleep returned (USB?)");
}

}  // namespace

void power_schedule_begin()
{
    ensure_tz();
    apply_defaults();
    ntp_started = false;
    network_time_ok = false;
    clock_latched = false;
    time_ready_ms = 0;
    last_poll_ms = 0;
    last_settings_ms = 0;
    Serial.println("power schedule: configurable segments (min on 30m / gap 15m)");
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
    reload_settings_if_due(!settings_loaded);

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
    const time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    const int minutes = local.tm_hour * 60 + local.tm_min;
    const int iso = iso_wday(local.tm_wday);

    if (in_on_window(iso, minutes)) {
        return;
    }

    time_t wake = 0;
    if (!next_wake_epoch(now, &wake)) {
        Serial.println("power schedule: no next wake");
        return;
    }
    enter_sleep_until(wake);
}
