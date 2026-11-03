// apps/wbconsole/wb_wifi_link.c —— WorkBuddy 伴生链路的 WiFi TCP 通道(见 wb_wifi_link.h)。
//
// 背景:PC 伴生守护原本只走 BLE。WinRT 蓝牙会话脆弱(设备重启即报废、扫描
// 到设备也不发起连接),语音延迟也受通知间隔限制。设备本来就常驻同一局域网
// 的 WiFi,直接开一个 TCP 服务:协议(JSON 行 + '\n')与 BLE 完全一致,PC 守护
// 连上即用。BLE 保留作兜底(设备不在 WiFi 时)。
//
// 发现机制:每 2 秒向 UDP 7811 广播一条 JSON:
//   {"wb_announce":"FoloPassport","ip":"192.168.2.x","port":7810}
// PC 守护监听广播拿到设备 IP,主动连 TCP。设备侧零配置。
#include "wb_wifi_link.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "wb_ble.h"

static const char *TAG = "wb_wifi_link";

#define WB_TCP_PORT 7810
#define WB_UDP_PORT 7811

// 1 = 双通道版（2026-11-03）：重开 WiFi TCP 通道（TCP:7810 + UDP:7811 广播），
// PC 蓝牙适配器硬件故障期间由 WiFi 承担守护↔设备链路；蓝牙恢复后仍 TCP 优先。
// UI 保持当前版（余额徽章/标签左对齐），wb_voice 48帧修复、开机直进对讲机均不变。
#define WB_WIFI_LINK_ENABLED 1

static TaskHandle_t s_task;
static volatile bool s_running;
static volatile int s_client = -1;          // 已连接的 PC 客户端 socket
static SemaphoreHandle_t s_tx_lock;

bool wb_wifi_link_connected(void) { return s_client >= 0; }

int wb_wifi_link_send(const char *data, size_t len)
{
    if (!data || len == 0 || len > 4096) return -1;
    const int fd = s_client;
    if (fd < 0) return -2;
    if (!s_tx_lock) s_tx_lock = xSemaphoreCreateMutex();
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(800)) != pdTRUE) return -3;
    // MSG_NOSIGNAL:对端断开时返回错误而不是杀进程(ESP 上默认也无 SIGPIPE,
    // 显式声明语义)。TCP 自己会分段,无需按 MTU 切。
    char buf[4096 + 1];
    memcpy(buf, data, len);
    buf[len] = '\n';
    size_t off = 0;
    const size_t total = len + 1;
    int ret = 0;
    while (off < total) {
        const int n = send(fd, buf + off, total - off, 0);
        if (n <= 0) {
            ret = -4;
            // 连接已半死(超时/对端异常):shutdown 唤醒读循环,由它统一
            // close_client,避免在发送路径与 recv 并发 close 同一 fd。
            shutdown(fd, SHUT_RDWR);
            break;
        }
        off += (size_t)n;
    }
    xSemaphoreGive(s_tx_lock);
    return ret;
}

// 把收到的行喂进与 BLE 共用的分发入口(环形缓冲 + proto 任务)。
static void feed_lines(uint8_t *buf, int n)
{
    static uint8_t carry[512];
    static int carry_len = 0;
    for (int i = 0; i < n; ++i) {
        carry[carry_len++] = buf[i];
        if (buf[i] == '\n' || carry_len >= (int)sizeof(carry) - 1) {
            wb_ble_inject_rx(carry, carry_len);
            carry_len = 0;
        }
    }
}

static void close_client(const char *why)
{
    const int fd = s_client;
    if (fd < 0) return;
    s_client = -1;
    closesocket(fd);
    ESP_LOGW(TAG, "PC 断开(%s)", why);
    wb_ble_route_notify(false);
}

static void wifi_link_task(void *arg)
{
    (void)arg;
    // 等 WiFi 拿到 IP(WiFi 没连上就轮询等待,连上后服务器常开)。
    esp_netif_ip_info_t ip;
    for (;;) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    char ipstr[16];
    esp_ip4addr_ntoa(&ip.ip, ipstr, sizeof(ipstr));

    // TCP 服务:同网段任意来源都可连(伴生协议无鉴权,与 BLE 一致;局域网内可信)。
    const int srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(WB_TCP_PORT);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 1) != 0) {
        ESP_LOGE(TAG, "TCP %d 监听失败 errno=%d", WB_TCP_PORT, errno);
        closesocket(srv);
        vTaskDelete(NULL);
        return;
    }

    // UDP 广播 socket:持续宣告设备 IP,PC 守护被动发现。
    const int udp = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in bcast = {0};
    bcast.sin_family = AF_INET;
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    bcast.sin_port = htons(WB_UDP_PORT);
    char ann[96];
    const int ann_len = snprintf(ann, sizeof(ann),
                                 "{\"wb_announce\":\"FoloPassport\",\"ip\":\"%s\",\"port\":%d}\n",
                                 ipstr, WB_TCP_PORT);
    ESP_LOGW(TAG, "TCP 服务就绪 %s:%d,UDP 广播每 2s", ipstr, WB_TCP_PORT);

    uint32_t last_ann = 0;
    for (;;) {
        if (!s_running) break;
        const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - last_ann >= 2000) {
            last_ann = now;
            (void)sendto(udp, ann, ann_len, 0, (struct sockaddr *)&bcast, sizeof(bcast));
        }
        if (s_client < 0) {
            // 等待新连接:select 200ms,周期性广播不受阻塞。
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(srv, &rf);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
            if (select(srv + 1, &rf, NULL, NULL, &tv) <= 0) continue;
            const int fd = accept(srv, NULL, NULL);
            if (fd < 0) continue;
            // 发送超时兜底:PC 端一旦停止读取(进程卡死/网络半开),TCP 发送
            // 缓冲填满后 send() 会无限阻塞 —— 调用方是协议任务,挂住它等于
            // 整条伴生链路假死(上下行全断,UI 也拿不到任何响应)。超时后
            // 主动断开,由守护的广播发现机制自动重连。
            const struct timeval tx_tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tx_tv, sizeof(tx_tv));
            // 接收超时:对端异常掉电/进程被杀时 TCP 半开,recv() 会永久挂起,
            // 连带循环顶部的 UDP 广播也停了 —— 守护再也发现不了设备,只能
            // 人工重启。超时只是回循环顶部:广播照发,连接没死也无损。
            const struct timeval rx_tv = { .tv_sec = 10, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rx_tv, sizeof(rx_tv));
            s_client = fd;
            ESP_LOGW(TAG, "PC 已连入(TCP)");
            wb_ble_route_notify(true);
            continue;
        }
        // 已连接:读数据。
        uint8_t tmp[512];
        const int n = recv(s_client, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            close_client(n < 0 ? "recv 错误" : "对端关闭");
            continue;
        }
        feed_lines(tmp, n);
    }
    if (s_client >= 0) { closesocket(s_client); s_client = -1; }
    closesocket(srv);
    closesocket(udp);
    s_task = NULL;
    vTaskDelete(NULL);
}

void wb_wifi_link_start(void)
{
#if !WB_WIFI_LINK_ENABLED
    return;                              // 蓝牙版:链路任务不启动
#endif
    if (s_task) return;
    s_running = true;
    if (!s_tx_lock) s_tx_lock = xSemaphoreCreateMutex();
    if (xTaskCreate(wifi_link_task, "wb_wifilnk", 6144, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "wifi_link 任务创建失败");
        s_task = NULL;
    }
}
