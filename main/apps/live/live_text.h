// main/app_text.h —— UTF-8 文本的安全处理。
//
// 抖音昵称和弹幕是任意外部输入：可能是超长文本、可能是合法但当前字库没有的
// 生僻字，也可能在任意字节处被网络截断。这些函数把"截断"和"缺字"变成可测试
// 的纯逻辑，UI 只负责把结果画出来。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 判断某个 Unicode 码点是否有可用字形。ctx 由调用方提供（例如字库覆盖表）。
typedef bool (*app_glyph_fn)(void *ctx, uint32_t codepoint);

// 解出下一个码点并前移指针。非法字节返回 0xFFFD 且只前移 1 字节，
// 既不会越界，也不会把后续合法字符一起吞掉。
uint32_t app_utf8_next(const char **cursor);

// 统计码点数（不含结尾 NUL）。非法字节按 1 个码点计。
size_t app_utf8_length(const char *text);

// 按码点而非字节截断，保证输出始终是合法 UTF-8 前缀。cap 包含结尾 NUL。
// 返回写入的字节数（不含 NUL）。
size_t app_utf8_truncate(const char *src, size_t max_chars, char *dst, size_t cap);

// 同上，但截断时追加省略号 "…"，避免用户误以为昵称本来就这么短。
size_t app_utf8_ellipsize(const char *src, size_t max_chars, char *dst, size_t cap);

// 把 src 里"当前字库没有字形"的码点替换成 '·'，非法字节丢弃。
// replaced 回填被替换/丢弃的码点数，便于日志与验收统计。
// 输出始终以 NUL 结尾；cap 过小则按码点边界截断。
size_t app_text_sanitize(const char *src, char *dst, size_t cap,
                         app_glyph_fn has_glyph, void *ctx, uint16_t *replaced);
#ifdef __cplusplus
}
#endif
