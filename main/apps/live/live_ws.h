// main/app_ws.h —— 连接 PC 桥接程序的 WebSocket 客户端。
//
// 只做传输：文本帧交给上层解析，二进制帧拼装完整后交给音频层。重连由
// esp_websocket_client 自己按退避策略处理，上层只需要知道"当前是否可用"。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "live_config.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    APP_WS_IDLE = 0,
    APP_WS_CONNECTING,
    APP_WS_OPEN,
    APP_WS_CLOSED,
} app_ws_state_t;

typedef struct {
    // 文本帧（已保证以 NUL 结尾）。在 WebSocket 任务上下文执行，不能阻塞。
    void (*on_message)(const char *json, void *user);
    // 二进制帧（已按 payload_len 拼装完整）。同样在 WebSocket 任务上下文。
    void (*on_binary)(const uint8_t *data, size_t len, void *user);
    void (*on_state)(app_ws_state_t state, void *user);
    void *user;
} app_ws_callbacks_t;

// 按配置建立连接（不会阻塞等待连接结果，状态通过回调上报）。
esp_err_t app_ws_start(const app_config_t *config, const app_ws_callbacks_t *callbacks);

void app_ws_stop(void);

app_ws_state_t app_ws_state(void);

bool app_ws_connected(void);

// 发送一条文本消息。未连接或发送失败返回错误，调用方自行决定是否丢弃。
esp_err_t app_ws_send_text(const char *text);
#ifdef __cplusplus
}
#endif
