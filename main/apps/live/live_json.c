// main/app_json.c —— 极简 JSON 取值实现（纯逻辑）。
#include "live_json.h"

#include <string.h>

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// p 指向 '"'，返回结尾引号之后的位置。转义序列里的引号不会提前结束。
static const char *skip_string(const char *p)
{
    if (*p != '"') return p;
    p++;
    while (*p != '\0') {
        if (*p == '\\') {
            if (p[1] == '\0') return p + 1;
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p != '\0') {
            if (*p == '"') {
                p = skip_string(p);
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                p++;
                if (depth == 0) return p;
                continue;
            }
            p++;
        }
        return p;
    }
    // 数字、true/false/null：吃到分隔符为止。
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        p++;
    }
    return p;
}

const char *app_json_value(const char *json, const char *key)
{
    if (json == NULL || key == NULL) return NULL;
    const size_t key_len = strlen(key);

    const char *p = skip_ws(json);
    if (*p != '{') return NULL;
    p++;

    for (;;) {
        p = skip_ws(p);
        if (*p == '}' || *p == '\0') return NULL;
        if (*p != '"') return NULL;

        const char *name = p + 1;
        const char *name_end = skip_string(p);
        const size_t name_len = (size_t)((name_end - 1) - name);
        const bool match = (name_len == key_len) && (memcmp(name, key, key_len) == 0);

        p = skip_ws(name_end);
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);

        if (match) return p;

        p = skip_ws(skip_value(p));
        if (*p == ',') {
            p++;
            continue;
        }
        return NULL;
    }
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

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(const char *p, uint32_t *out)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        const int digit = hex_digit(p[i]);
        if (digit < 0) return false;
        value = (value << 4) | (uint32_t)digit;
    }
    *out = value;
    return true;
}

// 把字符串值解码写入 out。返回写入字节数；cap 不足时在字符边界停下。
static size_t decode_string(const char *value, char *out, size_t cap)
{
    size_t used = 0;
    const char *p = value + 1;   // 跳过起始引号

    while (*p != '\0' && *p != '"') {
        char buf[4];
        size_t len = 1;
        buf[0] = *p;

        if (*p == '\\') {
            const char escape = p[1];
            if (escape == '\0') break;

            switch (escape) {
            case '"': buf[0] = '"'; break;
            case '\\': buf[0] = '\\'; break;
            case '/': buf[0] = '/'; break;
            case 'b': buf[0] = '\b'; break;
            case 'f': buf[0] = '\f'; break;
            case 'n': buf[0] = '\n'; break;
            case 'r': buf[0] = '\r'; break;
            case 't': buf[0] = '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!read_hex4(p + 2, &cp)) {
                    p += 2;   // 非法 \u：跳过反斜杠与 u，继续按普通字符处理
                    continue;
                }
                p += 6;       // 反斜杠 + u + 4 位十六进制
                // UTF-16 代理对：合法的高低组合成补充平面码点。
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    uint32_t low = 0;
                    if (read_hex4(p + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        p += 6;
                    }
                }
                len = encode_utf8(cp, buf);
                goto emit;
            }
            default:
                // 未知转义：原样保留字符本身，丢掉反斜杠。
                buf[0] = escape;
                break;
            }
            p += 2;
        } else {
            p += 1;
        }

    emit:
        if (used + len + 1 > cap) break;
        memcpy(out + used, buf, len);
        used += len;
    }

    if (cap > 0) out[used] = '\0';
    return used;
}

bool app_json_get_string(const char *json, const char *key, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    const char *value = app_json_value(json, key);
    if (value == NULL || *value != '"') return false;
    decode_string(value, out, cap);
    return true;
}

bool app_json_get_int(const char *json, const char *key, long *out)
{
    const char *value = app_json_value(json, key);
    if (value == NULL) return false;

    bool negative = false;
    if (*value == '-') {
        negative = true;
        value++;
    }
    if (*value < '0' || *value > '9') return false;

    long parsed = 0;
    while (*value >= '0' && *value <= '9') {
        if (parsed > (100000000L)) return false;   // 事件字段不需要更大，防溢出
        parsed = parsed * 10 + (*value - '0');
        value++;
    }
    if (*value == '.') return false;   // 本项目协议只用整数
    if (out) *out = negative ? -parsed : parsed;
    return true;
}

bool app_json_get_bool(const char *json, const char *key, bool *out)
{
    const char *value = app_json_value(json, key);
    if (value == NULL) return false;
    if (strncmp(value, "true", 4) == 0) {
        if (out) *out = true;
        return true;
    }
    if (strncmp(value, "false", 5) == 0) {
        if (out) *out = false;
        return true;
    }
    return false;
}

size_t app_json_escape(const char *src, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0) return 0;
    dst[0] = '\0';
    if (src == NULL) return 0;

    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        char buf[6];
        size_t len;
        switch (*p) {
        case '"':
            buf[0] = '\\'; buf[1] = '"'; len = 2;
            break;
        case '\\':
            buf[0] = '\\'; buf[1] = '\\'; len = 2;
            break;
        case '\n':
            buf[0] = '\\'; buf[1] = 'n'; len = 2;
            break;
        case '\r':
            buf[0] = '\\'; buf[1] = 'r'; len = 2;
            break;
        case '\t':
            buf[0] = '\\'; buf[1] = 't'; len = 2;
            break;
        default:
            if (*p < 0x20) {
                // 其余控制字符统一走 \u00XX，避免破坏 JSON。
                static const char hex[] = "0123456789abcdef";
                buf[0] = '\\'; buf[1] = 'u'; buf[2] = '0'; buf[3] = '0';
                buf[4] = hex[(*p >> 4) & 0x0F];
                buf[5] = hex[*p & 0x0F];
                len = 6;
            } else {
                buf[0] = (char)*p;
                len = 1;
            }
            break;
        }
        if (used + len + 1 > cap) break;
        memcpy(dst + used, buf, len);
        used += len;
    }
    dst[used] = '\0';
    return used;
}
