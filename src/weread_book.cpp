#include "weread_book.h"

#include "power_schedule.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <cstring>
#include <new>
#include <sys/time.h>
#include <time.h>

namespace {

void clear_book(WereadBook& out)
{
    out.ok = false;
    out.updated_at[0] = '\0';
    out.slot[0] = '\0';
    out.why[0] = '\0';
    out.book_id[0] = '\0';
    out.title[0] = '\0';
    out.author[0] = '\0';
    out.cover_url[0] = '\0';
    out.category[0] = '\0';
    out.rating[0] = '\0';
    out.rating_count[0] = '\0';
    out.words[0] = '\0';
    out.quote[0] = '\0';
    out.quote_hit[0] = '\0';
    out.quote_source[0] = '\0';
    out.eye[0] = '\0';
    out.lede[0] = '\0';
    out.intro[0] = '\0';
    out.intro_hit[0] = '\0';
    out.voice[0] = '\0';
    out.recommend_lead[0] = '\0';
    out.recommend[0] = '\0';
    out.recommend_hit[0] = '\0';
    out.error[0] = '\0';
}

void clear_day(WereadDay& out)
{
    out.ok = false;
    out.date[0] = '\0';
    out.updated_at[0] = '\0';
    out.error[0] = '\0';
    out.count = 0;
    for (int i = 0; i < kWereadDayMax; ++i) {
        clear_book(out.books[i]);
    }
}

void copy_field(char* dst, size_t n, JsonVariantConst v)
{
    if (n == 0) {
        return;
    }
    if (!v.is<const char*>()) {
        dst[0] = '\0';
        return;
    }
    const char* s = v.as<const char*>();
    if (s == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, s, n - 1);
    dst[n - 1] = '\0';
}

bool wall_clock_ok()
{
    return time(nullptr) >= 1700000000LL;
}

void apply_epoch(time_t epoch, const char* source)
{
    if (epoch < 1700000000LL) {
        return;
    }
    // 已有可信时钟时不覆盖（避免 Pages 静态 JSON 里陈旧 server_now 把时间打回去）
    if (wall_clock_ok() && source != nullptr && std::strcmp(source, "server_now") == 0) {
        return;
    }
    setenv("TZ", "CST-8", 1);
    tzset();
    timeval tv{};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    struct tm local {};
    localtime_r(&epoch, &local);
    Serial.printf("time from %s %02d:%02d:%02d\n", source ? source : "?", local.tm_hour, local.tm_min,
                  local.tm_sec);
}

void apply_server_now(JsonVariantConst doc)
{
    if (!(doc["server_now"].is<long long>() || doc["server_now"].is<long>() ||
          doc["server_now"].is<unsigned long>() || doc["server_now"].is<int>())) {
        return;
    }
    apply_epoch(static_cast<time_t>(doc["server_now"].as<long long>()), "server_now");
}

// RFC 1123：Date: Wed, 26 Nov 2026 16:40:00 GMT
bool apply_http_date(const char* date_hdr)
{
    if (date_hdr == nullptr || date_hdr[0] == '\0' || wall_clock_ok()) {
        return false;
    }
    struct tm gmt {};
    // %z 在部分 newlib 上不稳；按 GMT 解析后再当 UTC 秒
    if (strptime(date_hdr, "%a, %d %b %Y %H:%M:%S", &gmt) == nullptr) {
        return false;
    }
    gmt.tm_isdst = 0;
    // newlib 未必有 timegm：临时切 UTC 用 mktime
    setenv("TZ", "GMT0", 1);
    tzset();
    const time_t epoch = mktime(&gmt);
    setenv("TZ", "CST-8", 1);
    tzset();
    if (epoch < 1700000000LL) {
        return false;
    }
    apply_epoch(epoch, "HTTP Date");
    power_schedule_mark_network_time();
    return true;
}

void fill_book_from_json(WereadBook& out, JsonVariantConst doc)
{
    clear_book(out);
    copy_field(out.updated_at, sizeof(out.updated_at), doc["updated_at"]);
    copy_field(out.slot, sizeof(out.slot), doc["slot"]);
    copy_field(out.why, sizeof(out.why), doc["why"]);
    copy_field(out.book_id, sizeof(out.book_id), doc["bookId"]);
    copy_field(out.title, sizeof(out.title), doc["title"]);
    copy_field(out.author, sizeof(out.author), doc["author"]);
    copy_field(out.cover_url, sizeof(out.cover_url), doc["cover"]);
    copy_field(out.category, sizeof(out.category), doc["category"]);
    copy_field(out.rating, sizeof(out.rating), doc["rating"]);
    copy_field(out.rating_count, sizeof(out.rating_count), doc["rating_count"]);
    copy_field(out.words, sizeof(out.words), doc["words"]);
    copy_field(out.quote, sizeof(out.quote), doc["quote"]);
    copy_field(out.quote_hit, sizeof(out.quote_hit), doc["quote_hit"]);
    copy_field(out.quote_source, sizeof(out.quote_source), doc["quote_source"]);
    copy_field(out.eye, sizeof(out.eye), doc["eye"]);
    copy_field(out.lede, sizeof(out.lede), doc["lede"]);
    copy_field(out.intro, sizeof(out.intro), doc["intro"]);
    copy_field(out.intro_hit, sizeof(out.intro_hit), doc["intro_hit"]);
    copy_field(out.voice, sizeof(out.voice), doc["voice"]);
    copy_field(out.recommend_lead, sizeof(out.recommend_lead), doc["recommend_lead"]);
    copy_field(out.recommend, sizeof(out.recommend), doc["recommend"]);
    copy_field(out.recommend_hit, sizeof(out.recommend_hit), doc["recommend_hit"]);
    if (out.why[0] == '\0' && out.slot[0] != '\0') {
        copy_field(out.why, sizeof(out.why), doc["slot"]);
    }
    if (out.title[0] == '\0' || out.cover_url[0] == '\0') {
        std::snprintf(out.error, sizeof(out.error), "%s", "缺少 title/cover");
        out.ok = false;
        return;
    }
    out.ok = true;
}

bool url_is_https(const char* url)
{
    return url != nullptr && strncmp(url, "https://", 8) == 0;
}

bool http_get_body(const char* url, String& body, char* err, size_t err_n)
{
    if (url == nullptr || url[0] == '\0') {
        std::snprintf(err, err_n, "%s", "URL 为空");
        return false;
    }
    Serial.printf("weread GET %s\n", url);
    body = "";

    // 与封面下载同一套路：静态 WiFiClientSecure，避免堆/栈生命周期崩溃
    static WiFiClientSecure tls;
    HTTPClient http;
    http.setConnectTimeout(15000);
    http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);

    bool began = false;
    if (url_is_https(url)) {
        tls.setInsecure();
        tls.setTimeout(30000);
        began = http.begin(tls, url);
    } else {
        began = http.begin(url);
    }
    if (!began) {
        std::snprintf(err, err_n, "%s", "打不开荐书 URL");
        tls.stop();
        return false;
    }
    // CDN 常下发 gzip；设备端不解压会导致「有正文但 JSON 解析失败」
    http.addHeader("Accept-Encoding", "identity");
    static const char* kHdrs[] = {"Date"};
    http.collectHeaders(kHdrs, 1);

    const int code = http.GET();
    Serial.printf("weread HTTP %d\n", code);
    if (code != HTTP_CODE_OK) {
        std::snprintf(err, err_n, "HTTP %d", code);
        http.end();
        tls.stop();
        return false;
    }

    // Pages 静态 JSON 无实时 server_now；用响应 Date 头作 NTP 备份校时
    apply_http_date(http.header("Date").c_str());

    // 必须用 getString()：CDN 常是 chunked，裸读流会把 "27ec\\r\\n" 这种块头读进正文
    body = http.getString();
    http.end();
    tls.stop();

    if (body.length() < 8) {
        std::snprintf(err, err_n, "%s", "片单正文过短");
        return false;
    }
    Serial.printf("weread body=%u\n", static_cast<unsigned>(body.length()));
    return true;
}

}  // namespace

bool weread_book_fetch(const char* url, WereadBook& out)
{
    clear_book(out);
    String body;
    if (!http_get_body(url, body, out.error, sizeof(out.error))) {
        return false;
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, body);
    if (err) {
        std::snprintf(out.error, sizeof(out.error), "%s", "JSON 解析失败");
        return false;
    }

    // 兼容片单：若带 books[]，取第一本
    if (doc["books"].is<JsonArray>()) {
        JsonArray arr = doc["books"].as<JsonArray>();
        if (arr.size() == 0) {
            std::snprintf(out.error, sizeof(out.error), "%s", "片单为空");
            return false;
        }
        fill_book_from_json(out, arr[0]);
    } else {
        fill_book_from_json(out, doc);
    }
    apply_server_now(doc);
    if (!out.ok) {
        return false;
    }
    Serial.printf("weread ok title=%s\n", out.title);
    return true;
}

bool weread_day_fetch(const char* url, WereadDay& out)
{
    clear_day(out);
    String body;
    if (!http_get_body(url, body, out.error, sizeof(out.error))) {
        return false;
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("weread JSON fail: %s head=%.60s\n", err.c_str(), body.c_str());
        std::snprintf(out.error, sizeof(out.error), "%s", "JSON 解析失败");
        return false;
    }

    copy_field(out.date, sizeof(out.date), doc["date"]);
    copy_field(out.updated_at, sizeof(out.updated_at), doc["updated_at"]);
    apply_server_now(doc);

    if (!doc["books"].is<JsonArray>()) {
        // 兼容旧单本 JSON
        fill_book_from_json(out.books[0], doc);
        if (!out.books[0].ok) {
            std::snprintf(out.error, sizeof(out.error), "%s", out.books[0].error);
            return false;
        }
        out.count = 1;
        out.ok = true;
        Serial.printf("weread day compat count=1 title=%s\n", out.books[0].title);
        return true;
    }

    JsonArray arr = doc["books"].as<JsonArray>();
    int n = 0;
    for (JsonVariantConst item : arr) {
        if (n >= kWereadDayMax) {
            break;
        }
        fill_book_from_json(out.books[n], item);
        if (out.books[n].ok) {
            ++n;
        }
    }
    out.count = n;
    if (n <= 0) {
        std::snprintf(out.error, sizeof(out.error), "%s", "片单无有效书");
        return false;
    }
    out.ok = true;
    Serial.printf("weread day ok date=%s count=%d\n", out.date, out.count);
    return true;
}
