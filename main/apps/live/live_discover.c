// live_discover.c —— UDP 广播监听实现。
#include "live_discover.h"

#include "live_json.h"

#include "esp_log.h"
#include "lwip/api.h"
#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "live_disc";

#define DISC_TASK_STACK 3584
#define DISC_BUF_SIZE   512

static TaskHandle_t s_task;
static volatile bool s_run;
static SemaphoreHandle_t s_lock;
static live_discover_result_t s_latest;

// app_json_value 返回的是"值起点"（字符串值含开头引号），这里拷出引号内的内容。
static void json_copy_str(const char *value, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0) return;
    dst[0] = '\0';
    if (value == NULL) return;
    if (*value == '"') value++;
    size_t i = 0;
    while (value[i] != '\0' && value[i] != '"' && i + 1 < cap) {
        dst[i] = value[i];
        i++;
    }
    dst[i] = '\0';
}

static void update_latest(const live_discover_result_t *next)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = !s_latest.valid ||
                         strcmp(s_latest.host, next->host) != 0 ||
                         s_latest.port != next->port ||
                         strcmp(s_latest.token, next->token) != 0;
    s_latest = *next;
    if (changed) s_latest.seq++;
    else s_latest.seq = next->seq;
    xSemaphoreGive(s_lock);
}

static void disc_task(void *arg)
{
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket 创建失败");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LIVE_DISCOVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %d 失败", LIVE_DISCOVER_PORT);
        closesocket(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 读取超时 2 秒，循环里顺便检查退出标志。
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "监听 UDP %d 等待桥接广播", LIVE_DISCOVER_PORT);
    char buf[DISC_BUF_SIZE];
    uint32_t seq = 0;

    while (s_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr *)&from, &from_len);
        if (len <= 0) continue;
        buf[len] = '\0';

        // 只认带 live_bridge 字段的广播，别的 UDP 包（含 WB 的 7811 广播错端口）
        // 一律忽略。
        const char *host = app_json_value(buf, "live_bridge");
        if (host == NULL || host[0] == '\0') continue;

        live_discover_result_t next = { 0 };
        json_copy_str(host, next.host, sizeof(next.host));
        const char *port_s = app_json_value(buf, "port");
        next.port = port_s ? (uint16_t)atoi(port_s) : 8787;
        if (next.port == 0) next.port = 8787;
        json_copy_str(app_json_value(buf, "token"), next.token, sizeof(next.token));
        next.valid = true;
        next.seq = ++seq;

        update_latest(&next);
        ESP_LOGI(TAG, "发现桥接 %s:%u", next.host, (unsigned)next.port);
    }

    closesocket(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

bool live_discover_start(void)
{
    if (s_task != NULL) return true;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return false;
    }
    s_run = true;
    if (xTaskCreate(disc_task, "live_disc", DISC_TASK_STACK, NULL, 4, &s_task) != pdPASS) {
        s_run = false;
        return false;
    }
    return true;
}

void live_discover_stop(void)
{
    s_run = false;
    // 给任务两秒收尾（读取超时 2 秒内必然退出）。
    for (int i = 0; i < 40 && s_task != NULL; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memset(&s_latest, 0, sizeof(s_latest));
        xSemaphoreGive(s_lock);
    }
}

bool live_discover_get(live_discover_result_t *out)
{
    if (out == NULL || s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_latest;
    xSemaphoreGive(s_lock);
    return out->valid;
}
