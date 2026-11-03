// main/app_policy.c —— 直播间播报策略实现（纯逻辑）。
#include "live_policy.h"

#include <stdio.h>
#include <string.h>

// 优先级：数字越大越先播。
#define PRIO_LIKE    0
#define PRIO_COMMENT 1
#define PRIO_ENTER   2
#define PRIO_GIFT    3
#define PRIO_FOLLOW  3

// 每分钟最多欢迎多少个新观众：遇到"进房潮"时宁可少欢迎，也不要变成复读机。
#define WELCOME_PER_MINUTE_MAX 6

// 这类词代表观众真的在等一个答复，命中就走"立即回复"，不参与抽样。
static const char *const COMMENT_KEYWORDS[] = {
    "多少钱", "价格", "怎么卖", "怎么买", "在哪买", "链接", "小黄车", "下单",
    "优惠", "便宜", "尺码", "发货", "现货", "有货", "包邮", "福袋",
    "推荐", "教程", "怎么用", "在吗", "多久", "保修",
};
#define KEYWORD_COUNT (sizeof(COMMENT_KEYWORDS) / sizeof(COMMENT_KEYWORDS[0]))

void app_policy_defaults(app_policy_cfg_t *cfg)
{
    if (cfg == NULL) return;
    cfg->min_gap_ms = 1600;
    cfg->welcome_user_cooldown_ms = 30u * 60u * 1000u;
    cfg->comment_cooldown_ms = 5000;
    cfg->comment_repeat_ms = 120u * 1000u;
    cfg->like_min_delay_ms = 45u * 1000u;
    cfg->like_max_delay_ms = 120u * 1000u;
    cfg->item_ttl_ms = 30u * 1000u;
    cfg->like_min_count = 5;
    cfg->comment_sample_every = 4;
    cfg->greet_enter = true;
    cfg->thank_gift = true;
    cfg->thank_follow = true;
    cfg->thank_like = true;
    cfg->answer_comment = true;
}

static uint32_t rng_next(app_policy_t *policy)
{
    // xorshift32：状态小、足够随机，且不依赖 C 库 rand 的实现差异。
    uint32_t x = policy->rng ? policy->rng : 0x1234567u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    policy->rng = x;
    return x;
}

static uint32_t rng_range(app_policy_t *policy, uint32_t low, uint32_t high)
{
    if (high <= low) return low;
    return low + (rng_next(policy) % (high - low + 1u));
}

void app_policy_init(app_policy_t *policy, const app_policy_cfg_t *cfg,
                     uint64_t now_ms, uint32_t seed)
{
    if (policy == NULL) return;
    memset(policy, 0, sizeof(*policy));
    app_policy_defaults(&policy->cfg);
    if (cfg) policy->cfg = *cfg;
    if (policy->cfg.item_ttl_ms == 0) policy->cfg.item_ttl_ms = 30000;
    policy->rng = seed ? seed : 0x9E3779B9u;
    policy->next_id = 1;
    policy->last_spoken_ms = now_ms;
    policy->welcome_window_start_ms = (uint32_t)now_ms;
    policy->like_deadline_ms = now_ms;
}

static uint8_t priority_of(app_kind_t kind)
{
    switch (kind) {
    case APP_KIND_GIFT: return PRIO_GIFT;
    case APP_KIND_FOLLOW: return PRIO_FOLLOW;
    case APP_KIND_ENTER: return PRIO_ENTER;
    case APP_KIND_COMMENT: return PRIO_COMMENT;
    case APP_KIND_LIKE: return PRIO_LIKE;
    default: return 0;
    }
}

static bool same_user(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static bool enqueue(app_policy_t *policy, const app_event_t *event, uint8_t priority,
                    uint64_t ready_ms, uint64_t now_ms)
{
    int free_slot = -1;
    for (size_t i = 0; i < APP_POLICY_QUEUE_MAX; i++) {
        if (!policy->queue[i].used) {
            free_slot = (int)i;
            break;
        }
    }

    if (free_slot < 0) {
        // 队列满：挤掉优先级最低、排得最早的那一项。新事件更不重要时直接丢弃，
        // 不能让低优先级的点赞把礼物致谢挤出队列。
        int victim = -1;
        for (size_t i = 0; i < APP_POLICY_QUEUE_MAX; i++) {
            if (victim < 0 ||
                policy->queue[i].priority < policy->queue[victim].priority ||
                (policy->queue[i].priority == policy->queue[victim].priority &&
                 policy->queue[i].seq < policy->queue[victim].seq)) {
                victim = (int)i;
            }
        }
        if (victim < 0 || policy->queue[victim].priority >= priority) return false;
        free_slot = victim;
    }

    app_policy_item_t *slot = &policy->queue[free_slot];
    slot->event = *event;
    slot->priority = priority;
    slot->ready_ms = ready_ms;
    slot->born_ms = now_ms;
    slot->seq = ++policy->seq;
    slot->used = true;
    return true;
}

static bool welcome_allowed(app_policy_t *policy, const char *user, uint64_t now_ms)
{
    if (user == NULL || user[0] == '\0') return false;

    // 每分钟配额：按窗口重置，避免进房潮把队列占满。
    if ((uint32_t)(now_ms - policy->welcome_window_start_ms) >= 60000u) {
        policy->welcome_window_start_ms = (uint32_t)now_ms;
        policy->welcome_in_window = 0;
    }
    if (policy->welcome_in_window >= WELCOME_PER_MINUTE_MAX) return false;

    int free_slot = -1;
    int oldest = -1;
    for (size_t i = 0; i < APP_POLICY_SEEN_MAX; i++) {
        app_policy_seen_t *entry = &policy->welcomed[i];
        if (!entry->used) {
            if (free_slot < 0) free_slot = (int)i;
            continue;
        }
        if (same_user(entry->user, user)) {
            if (now_ms - entry->seen_ms < policy->cfg.welcome_user_cooldown_ms) return false;
            entry->seen_ms = now_ms;
            policy->welcome_in_window++;
            return true;
        }
        if (oldest < 0 || entry->seen_ms < policy->welcomed[oldest].seen_ms) oldest = (int)i;
    }

    if (free_slot < 0) free_slot = oldest >= 0 ? oldest : 0;
    app_policy_seen_t *entry = &policy->welcomed[free_slot];
    snprintf(entry->user, sizeof(entry->user), "%s", user);
    entry->seen_ms = now_ms;
    entry->used = true;
    policy->welcome_in_window++;
    return true;
}

static bool keyword_hit(const char *text)
{
    if (text == NULL) return false;
    for (size_t i = 0; i < KEYWORD_COUNT; i++) {
        if (strstr(text, COMMENT_KEYWORDS[i]) != NULL) return true;
    }
    return false;
}

static bool comment_is_repeat(app_policy_t *policy, const app_event_t *event, uint64_t now_ms)
{
    for (size_t i = 0; i < APP_POLICY_RECENT_MAX; i++) {
        const app_policy_recent_t *entry = &policy->recent[i];
        if (!entry->used) continue;
        if (now_ms - entry->seen_ms > policy->cfg.comment_repeat_ms) continue;
        if (same_user(entry->user, event->user) && strcmp(entry->text, event->text) == 0) {
            return true;
        }
    }

    int slot = 0;
    for (size_t i = 0; i < APP_POLICY_RECENT_MAX; i++) {
        if (!policy->recent[i].used) {
            slot = (int)i;
            break;
        }
        if (policy->recent[i].seen_ms < policy->recent[slot].seen_ms) slot = (int)i;
    }
    app_policy_recent_t *entry = &policy->recent[slot];
    snprintf(entry->user, sizeof(entry->user), "%s", event->user);
    snprintf(entry->text, sizeof(entry->text), "%s", event->text);
    entry->seen_ms = now_ms;
    entry->used = true;
    return false;
}

bool app_policy_on_event(app_policy_t *policy, const app_event_t *event, uint64_t now_ms)
{
    if (policy == NULL || event == NULL) return false;
    const app_policy_cfg_t *cfg = &policy->cfg;
    const uint8_t priority = priority_of(event->kind);

    switch (event->kind) {
    case APP_KIND_GIFT: {
        if (!cfg->thank_gift) return false;
        // 连击合并：同一观众在排队期间继续送，只累加数量，毕业成为一句"谢谢"。
        for (size_t i = 0; i < APP_POLICY_QUEUE_MAX; i++) {
            app_policy_item_t *item = &policy->queue[i];
            if (!item->used || item->event.kind != APP_KIND_GIFT) continue;
            if (!same_user(item->event.user, event->user)) continue;
            uint32_t total = (uint32_t)item->event.count + (event->count ? event->count : 1);
            item->event.count = (uint16_t)(total > 0xFFFFu ? 0xFFFFu : total);
            if (event->gift[0] != '\0') {
                snprintf(item->event.gift, sizeof(item->event.gift), "%s", event->gift);
                snprintf(item->event.text, sizeof(item->event.text), "%s", event->gift);
            }
            item->ready_ms = now_ms;
            item->born_ms = now_ms;   // 合并后重新计时，避免刚合并就过期
            return true;
        }
        return enqueue(policy, event, priority, now_ms, now_ms);
    }

    case APP_KIND_FOLLOW:
        if (!cfg->thank_follow) return false;
        return enqueue(policy, event, priority, now_ms, now_ms);

    case APP_KIND_ENTER:
        if (!cfg->greet_enter) return false;
        if (!welcome_allowed(policy, event->user, now_ms)) return false;
        return enqueue(policy, event, priority, now_ms, now_ms);

    case APP_KIND_LIKE: {
        if (!cfg->thank_like) return false;
        const uint32_t add = event->count ? event->count : 1u;
        uint32_t total = (uint32_t)policy->like_accum + add;
        policy->like_accum = (uint16_t)(total > 0xFFFFu ? 0xFFFFu : total);
        if (!policy->like_pending) {
            policy->like_pending = true;
            policy->like_deadline_ms = now_ms +
                rng_range(policy, cfg->like_min_delay_ms, cfg->like_max_delay_ms);
        }
        return true;
    }

    case APP_KIND_COMMENT: {
        if (!cfg->answer_comment) return false;
        if (event->text[0] == '\0') return false;
        if (now_ms - policy->last_comment_ms < cfg->comment_cooldown_ms) return false;
        if (comment_is_repeat(policy, event, now_ms)) return false;

        bool accept = keyword_hit(event->text);
        if (!accept && cfg->comment_sample_every > 0) {
            policy->comment_counter++;
            accept = (policy->comment_counter % cfg->comment_sample_every) == 0;
        }
        if (!accept) return false;

        if (!enqueue(policy, event, priority, now_ms, now_ms)) return false;
        policy->last_comment_ms = now_ms;
        return true;
    }

    default:
        return false;
    }
}

void app_policy_on_tick(app_policy_t *policy, uint64_t now_ms)
{
    if (policy == NULL) return;

    // 过期清理：排队太久的弹幕已经没有回复价值，说出来反而莫名其妙。
    for (size_t i = 0; i < APP_POLICY_QUEUE_MAX; i++) {
        app_policy_item_t *item = &policy->queue[i];
        if (!item->used) continue;
        uint64_t ttl = policy->cfg.item_ttl_ms;
        if (item->event.kind == APP_KIND_ENTER || item->event.kind == APP_KIND_FOLLOW ||
            item->event.kind == APP_KIND_GIFT) {
            ttl *= 3;
        }
        if (now_ms - item->born_ms > ttl) item->used = false;
    }

    if (policy->like_pending && now_ms >= policy->like_deadline_ms) {
        if (policy->like_accum >= policy->cfg.like_min_count) {
            app_event_t event = { 0 };
            event.kind = APP_KIND_LIKE;
            event.count = policy->like_accum;
            (void)enqueue(policy, &event, PRIO_LIKE, now_ms, now_ms);
            policy->like_accum = 0;
            policy->like_pending = false;
        } else if (policy->like_accum > 0) {
            // 数量还不够：把窗口往后延一次，继续等观众点赞，别急着开口。
            policy->like_deadline_ms = now_ms +
                rng_range(policy, policy->cfg.like_min_delay_ms, policy->cfg.like_max_delay_ms);
        } else {
            policy->like_pending = false;
        }
    }
}

bool app_policy_next(app_policy_t *policy, uint64_t now_ms, app_event_t *out, uint16_t *id)
{
    if (policy == NULL || out == NULL) return false;
    if (policy->speaking) return false;
    if (now_ms - policy->last_spoken_ms < policy->cfg.min_gap_ms) return false;

    int best = -1;
    for (size_t i = 0; i < APP_POLICY_QUEUE_MAX; i++) {
        const app_policy_item_t *item = &policy->queue[i];
        if (!item->used || item->ready_ms > now_ms) continue;
        if (best < 0 ||
            item->priority > policy->queue[best].priority ||
            (item->priority == policy->queue[best].priority &&
             item->seq < policy->queue[best].seq)) {
            best = (int)i;
        }
    }
    if (best < 0) return false;

    *out = policy->queue[best].event;
    if (id) *id = policy->next_id++;
    policy->queue[best].used = false;
    policy->speaking = true;
    policy->last_spoken_ms = now_ms;
    return true;
}

void app_policy_finish(app_policy_t *policy, uint64_t now_ms, bool ok)
{
    (void)ok;
    if (policy == NULL) return;
    policy->speaking = false;
    policy->last_spoken_ms = now_ms;
}

bool app_policy_is_speaking(const app_policy_t *policy)
{
    return policy != NULL && policy->speaking;
}

size_t app_policy_pending(const app_policy_t *policy)
{
    if (policy == NULL) return 0;
    size_t count = 0;
    for (size_t i = 0; i < APP_POLICY_QUEUE_MAX; i++) {
        if (policy->queue[i].used) count++;
    }
    return count;
}

void app_policy_reset_queue(app_policy_t *policy, uint64_t now_ms)
{
    if (policy == NULL) return;
    memset(policy->queue, 0, sizeof(policy->queue));
    policy->speaking = false;
    policy->like_accum = 0;
    policy->like_pending = false;
    policy->last_spoken_ms = now_ms;
}
