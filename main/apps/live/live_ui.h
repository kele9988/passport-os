// live_ui.h —— 直播助手界面层（移植自 AI Passport app_ui.h）。
//
// 信息层级按主播的注意力顺序排列——连接状态 → 正在说什么 → 最近弹幕 → 互动计数。
//
// 所有函数内部自行获取 LVGL 锁，可以从任意任务调用；拿不到锁就直接放弃这一帧，
// 宁可丢一次刷新，也不让网络任务卡在界面上。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "live_event.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_UI_BOOT = 0,
    APP_UI_CONNECTING,     // 连桥接中
    APP_UI_LIVE,           // 已连上桥接
    APP_UI_OFFLINE,        // 桥接断开
} app_ui_state_t;

typedef struct {
    const char *bridge;    // 已脱敏的桥接地址
    const char *wifi;      // SSID 或 IP 说明
    const char *token_hint;  // 令牌是否已配置
    int volume;
    bool muted;
    bool greet_enter;
    bool thank_gift;
    bool thank_follow;
    bool thank_like;
    bool answer_comment;
} app_ui_settings_t;

// 建主界面。必须在 bsp_lvgl_init() 之后调用。
void app_ui_init(void);

// 退出应用时销毁全部界面与定时器（app_registry 会加载下一个应用的屏幕）。
void app_ui_deinit(void);

void app_ui_set_state(app_ui_state_t state);
void app_ui_set_room(const char *room);

// 顶部右侧的网络说明（如 "192.168.1.23" 或 "未连接"）。
void app_ui_set_network(const char *text);
void app_ui_set_battery(int percent);      // -1 表示读数不可用
void app_ui_set_muted(bool muted);

// 互动计数。
void app_ui_set_counters(uint32_t online, uint32_t likes, uint32_t gifts, uint32_t follows);

// 收一条弹幕/事件进列表。text 为空时按种类生成摘要。
void app_ui_push_event(app_kind_t kind, const char *user, const char *text);

// 顶部"正在说"横幅。
void app_ui_show_speech(app_kind_t kind, const char *user, const char *text);
void app_ui_clear_speech(void);

// 短暂提示（音量、静音、错误）。
void app_ui_toast(const char *text);

// 设置页。
void app_ui_show_settings(const app_ui_settings_t *settings);
void app_ui_settings_select(int index);
void app_ui_hide_settings(void);
#ifdef __cplusplus
}
#endif
