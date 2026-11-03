// main/app_policy.h —— 直播间"说什么、什么时候说"的纯逻辑内核。
//
// 这一层刻意不依赖 Wi-Fi、LVGL 或 ESP-IDF：抖音直播间的事件密度远高于设备
// 能说话的速度，限流、去重、优先级、随机点赞致谢如果写进网络回调，就只能靠
// 真机碰运气验证。把它们收进这里，主机测试可以精确控制时间轴。
//
// 输出的是"一条待播报请求"（app_event_t + 播报 id），由 app_core 发给桥接端
// 换取文字与语音。设备自己从不编造内容，只决定取舍。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "live_event.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_POLICY_QUEUE_MAX 8
#define APP_POLICY_SEEN_MAX  12
#define APP_POLICY_RECENT_MAX 4

typedef struct {
    uint32_t min_gap_ms;            // 两句之间的最小间隔
    uint32_t welcome_user_cooldown_ms;  // 同一用户重复进房不再欢迎
    uint32_t comment_cooldown_ms;   // 弹幕回复的全局冷却
    uint32_t comment_repeat_ms;     // 相同内容去重窗口
    uint32_t like_min_delay_ms;     // 点赞致谢的随机区间下界
    uint32_t like_max_delay_ms;     // 上界（含）
    uint32_t item_ttl_ms;           // 排队项的最长存活时间，过期即丢弃
    uint16_t like_min_count;        // 攒够多少赞才值得开口
    uint8_t comment_sample_every;   // 非关键词弹幕每 N 条回一条，0 = 不回
    bool greet_enter;               // 进房欢迎
    bool thank_gift;                // 礼物致谢
    bool thank_follow;              // 关注致谢
    bool thank_like;                // 点赞致谢
    bool answer_comment;            // 弹幕针对性回复
} app_policy_cfg_t;

typedef struct {
    app_event_t event;
    uint8_t priority;
    uint64_t ready_ms;
    uint64_t born_ms;
    uint32_t seq;
    bool used;
} app_policy_item_t;

typedef struct {
    char user[APP_USER_MAX];
    uint64_t seen_ms;
    bool used;
} app_policy_seen_t;

typedef struct {
    char user[APP_USER_MAX];
    char text[APP_TEXT_MAX];
    uint64_t seen_ms;
    bool used;
} app_policy_recent_t;

typedef struct {
    app_policy_cfg_t cfg;
    app_policy_item_t queue[APP_POLICY_QUEUE_MAX];
    app_policy_seen_t welcomed[APP_POLICY_SEEN_MAX];
    app_policy_recent_t recent[APP_POLICY_RECENT_MAX];

    uint32_t seq;
    uint32_t rng;
    uint16_t next_id;
    uint32_t comment_counter;

    uint64_t last_spoken_ms;
    uint64_t last_comment_ms;

    uint64_t like_deadline_ms;
    uint16_t like_accum;
    bool like_pending;

    uint32_t welcome_window_start_ms;
    uint8_t welcome_in_window;

    bool speaking;
} app_policy_t;

// 保守默认值：本地直播场景下"能听清、不刷屏"比"每条都回"更重要。
void app_policy_defaults(app_policy_cfg_t *cfg);

void app_policy_init(app_policy_t *policy, const app_policy_cfg_t *cfg,
                     uint64_t now_ms, uint32_t seed);

// 收进一条事件，返回是否被接受排队（被限流/去重/过期策略拒绝时返回 false）。
bool app_policy_on_event(app_policy_t *policy, const app_event_t *event, uint64_t now_ms);

// 需要周期性调用（建议 200 ms）：处理点赞致谢的随机延时、清理过期排队项、
// 重置每分钟的欢迎配额。
void app_policy_on_tick(app_policy_t *policy, uint64_t now_ms);

// 取出一条可以立即播报的请求。返回 false 表示当前不该说话（正在播报、
// 间隔未到或队列为空）。id 用于把桥接端返回的语音与这句话对上。
bool app_policy_next(app_policy_t *policy, uint64_t now_ms, app_event_t *out, uint16_t *id);

// 一句话播报结束（ok=false 表示超时/桥接端失败），释放"正在播报"状态。
void app_policy_finish(app_policy_t *policy, uint64_t now_ms, bool ok);

bool app_policy_is_speaking(const app_policy_t *policy);
size_t app_policy_pending(const app_policy_t *policy);

// 断线或切换配置时清空排队项与聚合计数，已建立的去重记录保留。
void app_policy_reset_queue(app_policy_t *policy, uint64_t now_ms);
#ifdef __cplusplus
}
#endif
