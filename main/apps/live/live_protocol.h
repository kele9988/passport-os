// main/app_protocol.h —— 设备与 PC 桥接程序之间的线协议。
//
// 设计原则：设备只做"要不要说、什么时候说"，桥接端只做"说什么、怎么合成语音"。
// 这样设备端的策略（限流、去重、优先级）可以脱离网络做主机测试，而大模型与
// TTS 的细碎依赖留在电脑上。
//
// 文本帧：UTF-8 JSON，一条消息一帧。字段如下（详见 docs/apps/douyin-live-companion.md）
//   桥接 → 设备：
//     {"t":"hello","room":"房间名","session":"..."}
//     {"t":"ev","k":"enter|comment|gift|like|follow","u":"昵称","x":"正文/礼物名","g":"礼物名","n":2}
//     {"t":"say","id":17,"k":"gift","u":"昵称","x":"谢谢小明的玫瑰"}
//     {"t":"stats","online":12,"like":340,"gift":5}
//     {"t":"bye","x":"原因"}
//   设备 → 桥接：
//     {"t":"hello","dev":"aipassport","fw":"1.0.0"}
//     {"t":"want","id":17,"k":"gift","u":"昵称","x":"...","g":"玫瑰","n":2}
//     {"t":"ack","id":17}
//
// 二进制帧：12 字节头 + PCM 负载（16 kHz / 16 bit / 单声道，小端）。
// 同一句话的所有音频帧共用 speak_id，靠 first/last 标志划分句子边界。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "live_event.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_FRAME_HEADER_SIZE 12
#define APP_FRAME_MAGIC       "APA1"
#define APP_FRAME_CODEC_PCM16 1
#define APP_FRAME_FLAG_FIRST  0x01
#define APP_FRAME_FLAG_LAST   0x02

typedef struct {
    uint8_t flags;
    uint8_t codec;
    uint16_t speak_id;
    const uint8_t *payload;
    size_t payload_len;    // 实际可读负载（不超过头部声明长度）
    size_t declared_len;   // 头部声明长度，用于校验
} app_frame_t;

// 解析音频帧头。帧不完整、魔数不符或声明长度越界时返回 false。
bool app_frame_decode(const uint8_t *data, size_t len, app_frame_t *out);

// 写音频帧头。cap 不足返回 0；调用方随后把 payload 放在 header 之后。
size_t app_frame_encode(uint8_t *dst, size_t cap, uint8_t flags,
                        uint16_t speak_id, size_t payload_len);

typedef enum {
    APP_MSG_UNKNOWN = 0,
    APP_MSG_EVENT,
    APP_MSG_SAY,
    APP_MSG_HELLO,
    APP_MSG_STATS,
    APP_MSG_BYE,
} app_msg_type_t;

typedef struct {
    app_msg_type_t type;
    app_event_t event;         // APP_MSG_EVENT / APP_MSG_SAY 的事件上下文
    uint16_t say_id;           // APP_MSG_SAY
    app_kind_t say_kind;       // APP_MSG_SAY
    char text[APP_TEXT_MAX];   // say 文本 / hello 房间名 / bye 原因
    char session[40];          // hello 会话标识
    uint32_t online;           // stats
    uint32_t likes;
    uint32_t gifts;
} app_message_t;

// 解析一条文本帧。无法识别或字段缺失时返回 false，并已把 out 清零。
bool app_protocol_parse(const char *json, app_message_t *out);

// 以下构造函数返回写入字节数（不含 NUL），cap 不足返回 0。
size_t app_protocol_build_hello(char *buf, size_t cap, const char *firmware);
size_t app_protocol_build_want(char *buf, size_t cap, uint16_t speak_id, const app_event_t *event);
size_t app_protocol_build_ack(char *buf, size_t cap, uint16_t speak_id);
#ifdef __cplusplus
}
#endif
