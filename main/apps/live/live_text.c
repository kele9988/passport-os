// main/app_text.c —— UTF-8 处理实现（纯逻辑，可脱离 ESP-IDF 做主机测试）。
#include "live_text.h"

#include <string.h>

#define APP_CP_REPLACEMENT 0xFFFDu
// 缺字占位用 U+00B7（中点）；它在 UTF-8 里是两字节，不能写成单字节字符常量。
static const char APP_SANITIZE_FILL[] = "\xC2\xB7";
#define APP_ELLIPSIS "…"

uint32_t app_utf8_next(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    if (p == NULL || *p == '\0') return 0;

    uint32_t cp;
    size_t need;

    if (p[0] < 0x80) {
        *cursor = (const char *)p + 1;
        return p[0];
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1Fu;
        need = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0Fu;
        need = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07u;
        need = 3;
    } else {
        *cursor = (const char *)p + 1;
        return APP_CP_REPLACEMENT;
    }

    for (size_t i = 1; i <= need; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            // 续字节缺失：只跳过首字节，让后面的合法字符仍能被解出来。
            *cursor = (const char *)p + 1;
            return APP_CP_REPLACEMENT;
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }

    *cursor = (const char *)p + need + 1;
    return cp;
}

size_t app_utf8_length(const char *text)
{
    if (text == NULL) return 0;
    size_t count = 0;
    const char *cursor = text;
    while (app_utf8_next(&cursor) != 0) count++;
    return count;
}

static size_t encode_utf8(uint32_t cp, char *dst)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t app_utf8_truncate(const char *src, size_t max_chars, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0) return 0;
    dst[0] = '\0';
    if (src == NULL) return 0;

    size_t out = 0;
    size_t chars = 0;
    const char *cursor = src;
    while (chars < max_chars) {
        const char *before = cursor;
        uint32_t cp = app_utf8_next(&cursor);
        if (cp == 0) break;
        if (cp == APP_CP_REPLACEMENT && (cursor - before) == 1) {
            continue;   // 非法字节：跳过且不计入字符数
        }
        char buf[4];
        const size_t len = encode_utf8(cp, buf);
        if (out + len + 1 > cap) break;
        memcpy(dst + out, buf, len);
        out += len;
        chars++;
    }
    dst[out] = '\0';
    return out;
}

size_t app_utf8_ellipsize(const char *src, size_t max_chars, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0) return 0;
    if (app_utf8_length(src) <= max_chars) return app_utf8_truncate(src, max_chars, dst, cap);

    // 给省略号留出一个码点的位置；max_chars 为 0 时只输出省略号。
    size_t keep = max_chars > 0 ? max_chars - 1 : 0;
    size_t out = app_utf8_truncate(src, keep, dst, cap);
    const size_t ellipsis_len = sizeof(APP_ELLIPSIS) - 1;
    if (out + ellipsis_len + 1 <= cap) {
        memcpy(dst + out, APP_ELLIPSIS, ellipsis_len);
        out += ellipsis_len;
        dst[out] = '\0';
    }
    return out;
}

size_t app_text_sanitize(const char *src, char *dst, size_t cap,
                         app_glyph_fn has_glyph, void *ctx, uint16_t *replaced)
{
    uint16_t dropped = 0;
    if (dst == NULL || cap == 0) {
        if (replaced) *replaced = 0;
        return 0;
    }
    dst[0] = '\0';
    if (src == NULL) {
        if (replaced) *replaced = 0;
        return 0;
    }

    size_t out = 0;
    const char *cursor = src;
    for (;;) {
        const char *before = cursor;
        uint32_t cp = app_utf8_next(&cursor);
        if (cp == 0) break;

        const bool invalid = (cp == APP_CP_REPLACEMENT) && ((cursor - before) == 1);
        const bool covered = !invalid && (has_glyph == NULL || has_glyph(ctx, cp));

        char buf[4];
        size_t len;
        if (invalid) {
            dropped++;
            continue;
        } else if (covered) {
            len = encode_utf8(cp, buf);
        } else {
            buf[0] = APP_SANITIZE_FILL[0];
            buf[1] = APP_SANITIZE_FILL[1];
            len = 2;
            dropped++;
        }
        if (out + len + 1 > cap) break;
        memcpy(dst + out, buf, len);
        out += len;
    }
    dst[out] = '\0';
    if (replaced) *replaced = dropped;
    return out;
}
