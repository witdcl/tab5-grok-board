#include "status_client.h"

#include "bots_roster.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

#include <cstring>

namespace {

constexpr int kMaxBodyBytes = 12 * 1024;

void clear_snapshot(StatusSnapshot& out)
{
    out.ok = false;
    out.updated_at[0] = '\0';
    out.error[0] = '\0';
    out.count = 0;
    for (uint8_t i = 0; i < kMaxStatusItems; ++i) {
        out.items[i].bot[0] = '\0';
        out.items[i].status[0] = '\0';
        out.items[i].task[0] = '\0';
        out.items[i].note[0] = '\0';
    }
}

void set_error(StatusSnapshot& out, const char* message)
{
    out.ok = false;
    out.count = 0;
    out.updated_at[0] = '\0';
    std::strncpy(out.error, message, sizeof(out.error) - 1);
    out.error[sizeof(out.error) - 1] = '\0';
}

// 按 UTF-8 码点拷贝，避免在汉字中间截断。
void copy_utf8(char* dst, size_t dst_len, const char* src)
{
    if (dst_len == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    size_t written = 0;
    while (src[written] != '\0') {
        const unsigned char lead = static_cast<unsigned char>(src[written]);
        size_t len = 1;
        if ((lead & 0x80) == 0x00) {
            len = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        } else {
            break;
        }
        if (written + len >= dst_len) {
            break;
        }
        bool complete = true;
        for (size_t k = 0; k < len; ++k) {
            if (src[written + k] == '\0') {
                complete = false;
                break;
            }
        }
        if (!complete) {
            break;
        }
        for (size_t k = 0; k < len; ++k) {
            dst[written + k] = src[written + k];
        }
        written += len;
    }
    dst[written] = '\0';
}

const char* json_string(JsonVariantConst value)
{
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();
        return text == nullptr ? "" : text;
    }
    return "";
}

bool parse_document(const char* body, StatusSnapshot& out)
{
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, body);
    if (err) {
        set_error(out, "JSON 解析失败");
        Serial.printf("status json: %s\n", err.c_str());
        return false;
    }
    if (!doc["items"].is<JsonArrayConst>()) {
        set_error(out, "JSON 缺少 items 数组");
        return false;
    }

    copy_utf8(out.updated_at, sizeof(out.updated_at), json_string(doc["updated_at"]));
    out.count = 0;
    for (JsonObjectConst item : doc["items"].as<JsonArrayConst>()) {
        if (out.count >= kMaxStatusItems) {
            break;
        }
        const char* bot_name = json_string(item["bot"]);
        if (!bots_is_watched(bot_name)) {
            continue;
        }
        StatusItem& dst = out.items[out.count];
        copy_utf8(dst.bot, sizeof(dst.bot), bot_name);
        copy_utf8(dst.status, sizeof(dst.status), json_string(item["status"]));
        copy_utf8(dst.task, sizeof(dst.task), json_string(item["task"]));
        copy_utf8(dst.note, sizeof(dst.note), json_string(item["note"]));
        ++out.count;
    }
    out.ok = true;
    out.error[0] = '\0';
    return true;
}

}  // namespace

bool status_fetch(const char* url, StatusSnapshot& out)
{
    clear_snapshot(out);
    if (url == nullptr || url[0] == '\0') {
        set_error(out, "STATUS_URL 为空");
        return false;
    }

    Serial.printf("status GET %s\n", url);

    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("Tab5-StatusBoard");
    if (!http.begin(String(url))) {
        set_error(out, "HTTP 打不开这个 URL");
        return false;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char message[sizeof(out.error)];
        if (code < 0) {
            snprintf(message, sizeof(message), "HTTP 失败：%s", HTTPClient::errorToString(code).c_str());
        } else {
            snprintf(message, sizeof(message), "HTTP 状态 %d", code);
        }
        http.end();
        set_error(out, message);
        Serial.println(message);
        return false;
    }

    const String body = http.getString();
    http.end();
    if (body.length() == 0) {
        set_error(out, "HTTP 正文是空的");
        return false;
    }
    if (body.length() > kMaxBodyBytes) {
        set_error(out, "HTTP 正文太长");
        return false;
    }

    const bool parsed = parse_document(body.c_str(), out);
    if (parsed) {
        Serial.printf("status ok items=%u updated_at=%s\n", out.count, out.updated_at);
    }
    return parsed;
}
