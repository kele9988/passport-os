// main/app_json.h —— 只覆盖本项目协议的极简 JSON 取值器。
//
// 为什么不直接用 cJSON：设备侧只需要从"扁平对象 + 字符串/整数/布尔"里取字段，
// 自带的取值器既能脱离 ESP-IDF 做主机测试（协议解析是崩溃和误判的高发区），
// 又不会为了几个字段把 cJSON 链接进固件。
// 它不检查整条 JSON 是否合法；调用方必须把它当成"从不可信输入里尽力取值"。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 返回 key 对应值的起始位置（指向值本身），找不到或结构异常返回 NULL。
// 只匹配顶层对象的直接成员，嵌套对象里的同名 key 不会被误取。
const char *app_json_value(const char *json, const char *key);

// 取字符串值并解码 JSON 转义（含 \uXXXX）。超长时按 UTF-8 字符边界截断。
// cap 包含结尾 NUL；cap 不足 1 时返回 false。
bool app_json_get_string(const char *json, const char *key, char *out, size_t cap);

// 取整数值。缺失、类型不符或溢出返回 false。
bool app_json_get_int(const char *json, const char *key, long *out);

// 取布尔值。缺失或类型不符返回 false。
bool app_json_get_bool(const char *json, const char *key, bool *out);

// 把 src 转义成 JSON 字符串内容（不含两端引号）。返回写入字节数（不含 NUL）。
// cap 不足时截断在 UTF-8 字符边界，保证输出仍是合法 JSON 片段。
size_t app_json_escape(const char *src, char *dst, size_t cap);
#ifdef __cplusplus
}
#endif
