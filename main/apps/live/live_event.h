// main/app_event.h —— 直播间事件的应用内统一表示。
//
// 桥接程序把抖音直播间里发生的事情压成一条 JSON，本文件给出设备侧解析后
// 使用的定长结构。字符串一律按 UTF-8 存放，长度受上限约束：ESP32-C3 没有
// PSRAM，事件在队列里是按值拷贝的，不能让单条弹幕无限长。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_USER_MAX 24
#define APP_TEXT_MAX 128
#define APP_GIFT_MAX 24

// 事件种类。数值顺序不代表优先级，优先级只由 app_policy 决定。
typedef enum {
    APP_KIND_NONE = 0,
    APP_KIND_ENTER,     // 有人进入直播间
    APP_KIND_COMMENT,   // 弹幕
    APP_KIND_GIFT,      // 送礼物
    APP_KIND_LIKE,      // 点赞
    APP_KIND_FOLLOW,    // 关注
} app_kind_t;

typedef struct {
    app_kind_t kind;
    char user[APP_USER_MAX];   // 昵称；点赞聚合事件可为空
    char text[APP_TEXT_MAX];   // 弹幕正文；礼物事件里放礼物名
    char gift[APP_GIFT_MAX];   // 礼物名
    uint16_t count;            // 礼物数量 / 点赞次数
} app_event_t;

// 种类名用于日志、协议与 UI 着色。未知种类返回 "unknown"。
const char *app_kind_name(app_kind_t kind);

// 把协议里的短标签（enter/comment/gift/like/follow）映射成种类。
app_kind_t app_kind_from_tag(const char *tag);
#ifdef __cplusplus
}
#endif
