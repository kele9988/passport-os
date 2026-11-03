// apps/vokie/ui_status.h —— Vokie 状态屏(移植自 vokie-ai-passport-firmware, MIT)。
// 移植差异:背光/息屏交给 launcher 统一管理,渲染改为 LVGL timer 轮询。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_STATUS_STARTING = 0,
    UI_STATUS_READY,
    UI_STATUS_RECORDING,
    UI_STATUS_PROCESSING,
    UI_STATUS_SUCCESS,
    UI_STATUS_ERROR,
    UI_STATUS_DISCONNECTED
} ui_status_state_t;

typedef enum {
    UI_STATUS_HINT_NONE = 0,
    UI_STATUS_HINT_VOICE = 1 << 0,
    UI_STATUS_HINT_SEND = 1 << 1,
    UI_STATUS_HINT_UNDO = 1 << 2,
    UI_STATUS_HINT_ALL = (1 << 3) - 1,
} ui_status_hint_t;

// 创建状态屏并加载(lvgl lock 内调用)。返回屏幕对象;失败返回 NULL。
void ui_status_init(void);
// 销毁屏幕与定时器(lvgl lock 内调用)。
void ui_status_deinit(void);

void ui_status_set_state(ui_status_state_t state, const char *message);
// 非阻塞:点亮一条按键提示(或三条)。任意上下文可调。
void ui_status_touch(ui_status_hint_t hint);

#ifdef __cplusplus
}
#endif
