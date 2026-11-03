// main/app_protocol.c —— 线协议解析与构造（纯逻辑，可主机测试）。
#include "live_protocol.h"

#include "live_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *app_kind_name(app_kind_t kind)
{
    switch (kind) {
    case APP_KIND_ENTER: return "enter";
    case APP_KIND_COMMENT: return "comment";
    case APP_KIND_GIFT: return "gift";
    case APP_KIND_LIKE: return "like";
    case APP_KIND_FOLLOW: return "follow";
    default: return "unknown";
    }
}

app_kind_t app_kind_from_tag(const char *tag)
{
    if (tag == NULL) return APP_KIND_NONE;
    if (strcmp(tag, "enter") == 0) return APP_KIND_ENTER;
    if (strcmp(tag, "comment") == 0) return APP_KIND_COMMENT;
    if (strcmp(tag, "gift") == 0) return APP_KIND_GIFT;
    if (strcmp(tag, "like") == 0) return APP_KIND_LIKE;
    if (strcmp(tag, "follow") == 0) return APP_KIND_FOLLOW;
    return APP_KIND_NONE;
}

bool app_frame_decode(const uint8_t *data, size_t len, app_frame_t *out)
{
    if (data == NULL || out == NULL) return false;
    if (len < APP_FRAME_HEADER_SIZE) return false;
    if (memcmp(data, APP_FRAME_MAGIC, 4) != 0) return false;

    const uint32_t declared = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                              ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
    const size_t available = len - APP_FRAME_HEADER_SIZE;
    if (declared > available) return false;

    out->flags = data[4];
    out->codec = data[5];
    out->speak_id = (uint16_t)((uint16_t)data[6] | ((uint16_t)data[7] << 8));
    out->payload = data + APP_FRAME_HEADER_SIZE;
    out->payload_len = declared;
    out->declared_len = declared;
    return true;
}

size_t app_frame_encode(uint8_t *dst, size_t cap, uint8_t flags,
                        uint16_t speak_id, size_t payload_len)
{
    if (dst == NULL || cap < APP_FRAME_HEADER_SIZE) return 0;
    if (payload_len > 0xFFFFu) return 0;

    memcpy(dst, APP_FRAME_MAGIC, 4);
    dst[4] = flags;
    dst[5] = APP_FRAME_CODEC_PCM16;
    dst[6] = (uint8_t)(speak_id & 0xFF);
    dst[7] = (uint8_t)(speak_id >> 8);
    const uint32_t length = (uint32_t)payload_len;
    dst[8] = (uint8_t)(length & 0xFF);
    dst[9] = (uint8_t)((length >> 8) & 0xFF);
    dst[10] = (uint8_t)((length >> 16) & 0xFF);
    dst[11] = (uint8_t)((length >> 24) & 0xFF);
    return APP_FRAME_HEADER_SIZE;
}

bool app_protocol_parse(const char *json, app_message_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (json == NULL) return false;

    char type[12] = { 0 };
    if (!app_json_get_string(json, "t", type, sizeof(type))) return false;

    if (strcmp(type, "ev") == 0) {
        char kind[12] = { 0 };
        if (!app_json_get_string(json, "k", kind, sizeof(kind))) return false;
        const app_kind_t k = app_kind_from_tag(kind);
        if (k == APP_KIND_NONE) return false;

        out->type = APP_MSG_EVENT;
        out->event.kind = k;
        (void)app_json_get_string(json, "u", out->event.user, sizeof(out->event.user));
        (void)app_json_get_string(json, "x", out->event.text, sizeof(out->event.text));
        (void)app_json_get_string(json, "g", out->event.gift, sizeof(out->event.gift));

        long count = 0;
        if (app_json_get_int(json, "n", &count) && count > 0) {
            out->event.count = (uint16_t)(count > 0xFFFF ? 0xFFFF : count);
        }
        // 礼物事件若没有单独正文，用礼物名兜底，UI 与日志才不会空白。
        if (out->event.text[0] == '\0' && out->event.gift[0] != '\0') {
            snprintf(out->event.text, sizeof(out->event.text), "%s", out->event.gift);
        }
        return true;
    }

    if (strcmp(type, "say") == 0) {
        long id = 0;
        if (!app_json_get_int(json, "id", &id) || id < 0 || id > 0xFFFF) return false;
        out->type = APP_MSG_SAY;
        out->say_id = (uint16_t)id;
        (void)app_json_get_string(json, "x", out->text, sizeof(out->text));
        (void)app_json_get_string(json, "u", out->event.user, sizeof(out->event.user));

        char kind[12] = { 0 };
        if (app_json_get_string(json, "k", kind, sizeof(kind))) {
            out->say_kind = app_kind_from_tag(kind);
        }
        return true;
    }

    if (strcmp(type, "hello") == 0) {
        out->type = APP_MSG_HELLO;
        (void)app_json_get_string(json, "room", out->text, sizeof(out->text));
        (void)app_json_get_string(json, "session", out->session, sizeof(out->session));
        return true;
    }

    if (strcmp(type, "stats") == 0) {
        long value = 0;
        out->type = APP_MSG_STATS;
        if (app_json_get_int(json, "online", &value) && value >= 0) out->online = (uint32_t)value;
        value = 0;
        if (app_json_get_int(json, "like", &value) && value >= 0) out->likes = (uint32_t)value;
        value = 0;
        if (app_json_get_int(json, "gift", &value) && value >= 0) out->gifts = (uint32_t)value;
        return true;
    }

    if (strcmp(type, "bye") == 0) {
        out->type = APP_MSG_BYE;
        (void)app_json_get_string(json, "x", out->text, sizeof(out->text));
        return true;
    }

    return false;
}

static int append(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
    if (*used >= cap) return -1;
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buf + *used, cap - *used, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= cap - *used) return -1;
    *used += (size_t)written;
    return 0;
}

size_t app_protocol_build_hello(char *buf, size_t cap, const char *firmware)
{
    if (buf == NULL || cap == 0) return 0;
    char fw[24] = { 0 };
    size_t used = 0;
    if (app_json_escape(firmware ? firmware : "", fw, sizeof(fw)) > sizeof(fw) - 1) return 0;
    if (append(buf, cap, &used, "{\"t\":\"hello\",\"dev\":\"aipassport\",\"fw\":\"%s\"}", fw) != 0) {
        return 0;
    }
    return used;
}

size_t app_protocol_build_want(char *buf, size_t cap, uint16_t speak_id, const app_event_t *event)
{
    if (buf == NULL || cap == 0 || event == NULL) return 0;

    // 转义缓冲按"每个字节最坏膨胀成 6 字节"取值：昵称里的引号或控制字符
    // 不会让整条消息构造失败。
    char user[APP_USER_MAX * 6];
    char text[APP_TEXT_MAX * 6];
    char gift[APP_GIFT_MAX * 6];
    if (app_json_escape(event->user, user, sizeof(user)) >= sizeof(user) - 1) return 0;
    if (app_json_escape(event->text, text, sizeof(text)) >= sizeof(text) - 1) return 0;
    if (app_json_escape(event->gift, gift, sizeof(gift)) >= sizeof(gift) - 1) return 0;

    size_t used = 0;
    if (append(buf, cap, &used,
               "{\"t\":\"want\",\"id\":%u,\"k\":\"%s\",\"u\":\"%s\",\"x\":\"%s\",\"g\":\"%s\",\"n\":%u}",
               (unsigned)speak_id, app_kind_name(event->kind), user, text, gift,
               (unsigned)event->count) != 0) {
        return 0;
    }
    return used;
}

size_t app_protocol_build_ack(char *buf, size_t cap, uint16_t speak_id)
{
    if (buf == NULL || cap == 0) return 0;
    size_t used = 0;
    if (append(buf, cap, &used, "{\"t\":\"ack\",\"id\":%u}", (unsigned)speak_id) != 0) return 0;
    return used;
}
