// main/app_ws.c —— WebSocket 客户端实现（基于 ESP-IDF esp_websocket_client）。
#include "live_ws.h"

#include "esp_log.h"
#include "esp_websocket_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ws";

// 一帧音频上限：桥接端按 4 KB 分片，留出余量即可。文本消息都很短。
#define RX_BUFFER_SIZE 6144
#define WS_TASK_STACK  6144

static esp_websocket_client_handle_t s_client;
static SemaphoreHandle_t s_send_lock;
static app_ws_callbacks_t s_callbacks;
static volatile app_ws_state_t s_state;

static uint8_t s_rx[RX_BUFFER_SIZE];
static size_t s_rx_len;
static size_t s_rx_total;
static int s_rx_opcode;
static bool s_rx_active;
static bool s_rx_oversize;

static void report_state(app_ws_state_t state)
{
    s_state = state;
    if (s_callbacks.on_state) s_callbacks.on_state(state, s_callbacks.user);
}

static void dispatch_message(void)
{
    if (s_rx_opcode == 0x01) {
        // 文本帧：补齐 NUL 后交给上层。上层必须在返回前完成解析或拷贝。
        const size_t usable = s_rx_len < sizeof(s_rx) - 1 ? s_rx_len : sizeof(s_rx) - 1;
        s_rx[usable] = '\0';
        if (s_callbacks.on_message) s_callbacks.on_message((const char *)s_rx, s_callbacks.user);
    } else if (s_rx_opcode == 0x02) {
        if (s_callbacks.on_binary) s_callbacks.on_binary(s_rx, s_rx_len, s_callbacks.user);
    }
}

static void handle_data(const esp_websocket_event_data_t *event)
{
    const int opcode = event->op_code;
    if (opcode != 0x01 && opcode != 0x02) return;

    const size_t offset = event->payload_offset;
    const size_t total = (size_t)event->payload_len;
    const size_t chunk = (size_t)event->data_len;

    if (offset == 0) {
        s_rx_len = 0;
        s_rx_total = total;
        s_rx_opcode = opcode;
        s_rx_active = true;
        s_rx_oversize = (total >= sizeof(s_rx));
        if (s_rx_oversize) {
            ESP_LOGW(TAG, "消息过大(%u 字节)，丢弃", (unsigned)total);
        }
    }

    if (!s_rx_active) return;

    if (!s_rx_oversize) {
        if (s_rx_len + chunk > sizeof(s_rx)) {
            s_rx_oversize = true;
        } else {
            memcpy(s_rx + s_rx_len, event->data_ptr, chunk);
            s_rx_len += chunk;
        }
    }

    // 最后一帧：payload_offset + data_len 已覆盖声明长度。
    if (offset + chunk >= total) {
        s_rx_active = false;
        if (!s_rx_oversize) dispatch_message();
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *data = event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "桥接已连接");
        s_rx_active = false;
        report_state(APP_WS_OPEN);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "桥接断开");
        s_rx_active = false;
        report_state(APP_WS_CLOSED);
        break;
    case WEBSOCKET_EVENT_DATA:
        handle_data(data);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket 错误");
        break;
    default:
        break;
    }
}

esp_err_t app_ws_start(const app_config_t *config, const app_ws_callbacks_t *callbacks)
{
    if (config == NULL || callbacks == NULL) return ESP_ERR_INVALID_ARG;
    if (s_client) return ESP_ERR_INVALID_STATE;

    char uri[160];
    if (app_config_build_uri(config, uri, sizeof(uri)) == 0) return ESP_ERR_INVALID_ARG;

    s_callbacks = *callbacks;
    if (s_send_lock == NULL) {
        s_send_lock = xSemaphoreCreateMutex();
        if (s_send_lock == NULL) return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .task_stack = WS_TASK_STACK,
        .buffer_size = RX_BUFFER_SIZE,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 8000,
        .disable_auto_reconnect = false,
    };

    s_client = esp_websocket_client_init(&ws_cfg);
    if (s_client == NULL) return ESP_ERR_NO_MEM;

    if (config->token[0] != '\0') {
        // 令牌走请求头：既避免 URL 编码问题，也不会出现在日志的 URI 里。
        esp_websocket_client_append_header(s_client, "X-Passport-Token", config->token);
    }

    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, NULL);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    report_state(APP_WS_CONNECTING);
    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        report_state(APP_WS_IDLE);
        return err;
    }

    ESP_LOGI(TAG, "正在连接 %s", uri);
    return ESP_OK;
}

void app_ws_stop(void)
{
    if (s_client == NULL) return;
    (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
    (void)esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_rx_active = false;
    report_state(APP_WS_IDLE);
}

bool app_ws_connected(void)
{
    return s_client != NULL && s_state == APP_WS_OPEN;
}

app_ws_state_t app_ws_state(void)
{
    return s_state;
}

esp_err_t app_ws_send_text(const char *text)
{
    if (text == NULL) return ESP_ERR_INVALID_ARG;
    if (s_client == NULL || s_send_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (!esp_websocket_client_is_connected(s_client)) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    const int sent = esp_websocket_client_send_text(s_client, text, (int)strlen(text),
                                                    pdMS_TO_TICKS(2000));
    xSemaphoreGive(s_send_lock);
    return (sent < 0) ? ESP_FAIL : ESP_OK;
}
