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
//
// 远程外连模式(2026-11-07):设备拿到外网时 UDP 广播没人听、守护也连不进来,
// 因此新增"设备主动外连"——作为 TCP 客户端直连家里守护的反向监听端口
// (默认 7812,经 lucky/端口映射暴露公网)。协议仍是 JSON 行,连上先发一条
// hello 握手行供守护校验 token。本地连入优先:槽位被占(无论哪个方向)则
// 不发起外连;断线后各自自动恢复。
#include "wb_wifi_link.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "lwip/sockets.h"
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "wb_ble.h"

static const char *TAG = "wb_wifi_link";

#define WB_TCP_PORT 7810
#define WB_UDP_PORT 7811

// ---- 远程外连配置(编译期注入,见 main/CMakeLists.txt) ----
// 未定义 WB_REMOTE_HOST 时外连任务不启动,行为与旧版完全一致。
// 外连走 WebSocket(wss://WB_REMOTE_HOST,经 Cloudflare 隧道回源家里桥):
// CF 边缘节点同时有 v4/v6,设备在无 IPv6 的手机热点下也能连上家里守护。
#ifndef WB_REMOTE_HOST
#define WB_REMOTE_HOST ""
#endif
// 与守护端 wb_app.py 的 REMOTE_TOKEN 默认值保持一致;发行固件可用
// -DPASSPORT_WB_REMOTE_TOKEN=<串> 覆盖,守护端用 wb_remote.json 覆盖。
#ifndef WB_REMOTE_TOKEN
#define WB_REMOTE_TOKEN "wb9f2k7xq4m1t8ze6"
#endif
#define WB_REMOTE_LOG_MS 60000     // 连不上时的日志节流

// WB 远程裸 TCP 直连(优先于 WS):-DPASSPORT_WB_REMOTE_TCP_HOST=<域名> 注入。
// 有 IPv6 时直连守护(经 NAS 转发),不建 TLS —— WSS 常驻要吃 ~40KB 堆,
// 对讲 PTT 时正是这 40KB 把内存压到见底(2026-11-07 实测 free=2KB)。
// TCP 拨不通(纯 v4 热点)才启用 wss://WB_REMOTE_HOST 兜底;TCP 恢复后
// 自动关掉 WS 释放内存。协议与本地通道完全一致(JSON 行 + hello 握手)。
#ifndef WB_REMOTE_TCP_HOST
#define WB_REMOTE_TCP_HOST ""
#endif
#ifndef WB_REMOTE_TCP_PORT
#define WB_REMOTE_TCP_PORT 7812
#endif

// WS 通道占用槽位的哨兵值:s_client == WB_SLOT_WS 表示当前链路是远程 WS。
#define WB_SLOT_WS (-2)

// 1 = 双通道版（2026-11-03）：重开 WiFi TCP 通道（TCP:7810 + UDP:7811 广播），
// PC 蓝牙适配器硬件故障期间由 WiFi 承担守护↔设备链路；蓝牙恢复后仍 TCP 优先。
// UI 保持当前版（余额徽章/标签左对齐），wb_voice 48帧修复、开机直进对讲机均不变。
#define WB_WIFI_LINK_ENABLED 1

static TaskHandle_t s_task;
static volatile bool s_running;
static volatile int s_client = -1;          // 已连接的 PC 客户端 socket(或 WB_SLOT_WS)
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_slot_lock;       // 槽位互斥:本地 accept 与远程外连抢同一 s_client
static volatile bool s_client_remote;       // 当前连接是否为远程外连建立
static esp_websocket_client_handle_t s_ws;  // 远程 WS 客户端(组件自动重连)
static volatile bool s_ws_connected;        // WS 底层已连接(不代表已占槽位)
static volatile bool s_ws_claimed;          // WS 已占用槽位并发过 hello

bool wb_wifi_link_connected(void)
{
    // WB_SLOT_WS 也是"已连":本地 TCP(fd>0)与远程 WS(-2)都算链路在线。
    return s_client != -1;
}

// 尝试占用唯一链路槽位。成功返回 true;已被占(任一方向)返回 false。
// s_client/owner 的读写统一走 s_slot_lock 临界区,避免本地 accept 与
// 远程外连两个任务同时认领造成双 owner。
static bool claim_client(int fd, bool remote)
{
    bool ok = false;
    if (!s_slot_lock) s_slot_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_slot_lock, portMAX_DELAY);
    if (s_client < 0) {
        s_client = fd;
        s_client_remote = remote;
        ok = true;
    }
    xSemaphoreGive(s_slot_lock);
    return ok;
}

int wb_wifi_link_send(const char *data, size_t len)
{
    if (!data || len == 0 || len > 4096) return -1;
    const int fd = s_client;
    if (fd < 0) return -2;
    if (!s_tx_lock) s_tx_lock = xSemaphoreCreateMutex();
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(800)) != pdTRUE) return -3;
    int ret = 0;
    if (fd == WB_SLOT_WS) {
        // 远程 WS 通道:整行一帧发出(组件自动组帧)。
        char buf[4096 + 1];
        memcpy(buf, data, len);
        buf[len] = '\n';
        if (esp_websocket_client_send_text(s_ws, buf, len + 1,
                                           pdMS_TO_TICKS(3000)) < 0) {
            ret = -4;
            ESP_LOGW(TAG, "WS 发送失败,链路交由断线检测回收");
        }
        xSemaphoreGive(s_tx_lock);
        return ret;
    }
    // MSG_NOSIGNAL:对端断开时返回错误而不是杀进程(ESP 上默认也无 SIGPIPE,
    // 显式声明语义)。TCP 自己会分段,无需按 MTU 切。
    char buf[4096 + 1];
    memcpy(buf, data, len);
    buf[len] = '\n';
    size_t off = 0;
    const size_t total = len + 1;
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
    if (!s_slot_lock) s_slot_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_slot_lock, portMAX_DELAY);
    const int fd = s_client;
    const bool remote = s_client_remote;
    s_client = -1;
    xSemaphoreGive(s_slot_lock);
    if (fd < 0) return;
    closesocket(fd);
    ESP_LOGW(TAG, "PC 断开(%s%s)", remote ? "远程" : "", why);
    wb_ble_route_notify(false);
}

// ---- 远程 WS 外连(经 Cloudflare 隧道) ----
// WS 断开:归还槽位(仅当槽位属于 WS);本地 TCP accept 拿 s_client<0 判断,
// WS 占坑用 WB_SLOT_WS 哨兵,主循环看到它跳过 recv(数据走事件回调)。
static void ws_release_if_owned(const char *why)
{
    if (!s_slot_lock) s_slot_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_slot_lock, portMAX_DELAY);
    const bool owned = (s_client == WB_SLOT_WS);
    if (owned) s_client = -1;
    xSemaphoreGive(s_slot_lock);
    if (owned) {
        s_ws_claimed = false;
        ESP_LOGW(TAG, "远程 WS 断开(%s),自动重连中", why);
        wb_ble_route_notify(false);
    }
}

static void ws_send_hello(void)
{
    char hello[128];
    const int hn = snprintf(hello, sizeof(hello),
        "{\"wb_hello\":\"FoloPassport\",\"token\":\"%s\"}\n", WB_REMOTE_TOKEN);
    if (esp_websocket_client_send_text(s_ws, hello, hn,
                                       pdMS_TO_TICKS(3000)) < 0) {
        ESP_LOGW(TAG, "hello 发送失败,等待链路重连");
    } else {
        ESP_LOGW(TAG, "守护已连上(远程 WS)");
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *event_data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_connected = true;
        ESP_LOGW(TAG, "远程 WS 建立连接(%s)", WB_REMOTE_HOST);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_ws_connected = false;
        ws_release_if_owned("底层断开");
        break;
    case WEBSOCKET_EVENT_DATA:
        // op_code 0x1=text 0x2=binary;payload 走 JSON 行协议,二者都收。
        if (e->op_code == 0x1 || e->op_code == 0x2) {
            if (e->data_ptr && e->data_len > 0 && s_client == WB_SLOT_WS) {
                feed_lines((uint8_t *)e->data_ptr, e->data_len);
            }
        } else if (e->op_code == 0x8 && e->data_len == 2) {
            // 对端关闭帧:立即视为断开,不等 TCP 层超时。
            s_ws_connected = false;
            ws_release_if_owned("对端关闭帧");
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "远程 WS 错误 errno=%d", e->error_handle.esp_tls_last_esp_err);
        break;
    default:
        break;
    }
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
        if (s_client == WB_SLOT_WS) {
            // 远程 WS 通道在用:收发走组件事件回调,这里只让广播照常发。
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
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
            // 与远程外连抢同一槽位:被先占则直接放弃这条(拒绝本地新连)。
            if (!claim_client(fd, false)) {
                ESP_LOGW(TAG, "本地连入被拒:槽位已占用");
                closesocket(fd);
                continue;
            }
            ESP_LOGW(TAG, "PC 已连入(TCP 本地)");
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

#if WB_WIFI_LINK_ENABLED
// ---- 远程外连任务:设备主动连家里守护(双通道) ----
// ①裸 TCP 直连 WB_REMOTE_TCP_HOST:WB_REMOTE_TCP_PORT(有 IPv6 时;协议与
//   本地通道完全一致,不建 TLS —— WSS 常驻要吃 ~40KB 堆,正是把对讲 PTT
//   时内存压到见底的元凶);
// ②TCP 拨不通(纯 v4 热点)才启用 wss://WB_REMOTE_HOST(CF 隧道 → NAS WS
//   桥 7813 → 守护 7812),TCP 恢复后自动关闭 WS 释放 TLS 内存。
// 本地连入始终优先(槽位互斥)。
static TaskHandle_t s_task_remote;

// 裸 TCP 拨号:v4/v6 都试,单地址 5s 上限,成功返回已恢复阻塞模式的 fd。
static int remote_tcp_dial(void)
{
    char port[8];
    snprintf(port, sizeof(port), "%d", WB_REMOTE_TCP_PORT);
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(WB_REMOTE_TCP_HOST, port, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        const int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        // 非阻塞 connect + 5s select 上限,避免不可达地址拖死重连节奏
        const int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        if (connect(s, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS) {
            closesocket(s);
            continue;
        }
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        if (select(s + 1, NULL, &wf, NULL, &tv) <= 0) { closesocket(s); continue; }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { closesocket(s); continue; }
        fcntl(s, F_SETFL, fl);      // 恢复阻塞模式
        fd = s;
    }
    freeaddrinfo(res);
    return fd;
}

// 远程 TCP 链路调参:发送 5s 超时(半死链路由发送路径 shutdown 唤醒读循环
// 统一回收);收包不设超时(与 WS 路径一致,空闲不踢),死链靠 TCP keepalive
// 在 ~35s 内探测出(recv 报错 → close_client → 重拨)。
static void remote_tcp_tune(int fd)
{
    const struct timeval tx_tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tx_tv, sizeof(tx_tv));
    const int ka = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
#ifdef TCP_KEEPIDLE
    const int idle = 20, intvl = 5, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

// 关闭并释放 WSS 兜底客户端(TCP 接管后调用,释放 TLS 占用的堆)。
static void ws_stop_and_free(void)
{
    if (!s_ws) return;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_ws_connected = false;
    s_ws_claimed = false;
    ESP_LOGW(TAG, "WSS 兜底已关闭(远程走裸 TCP,释放 TLS 内存)");
}

static void wb_remote_task(void *arg)
{
    (void)arg;
    if (!WB_REMOTE_HOST[0] && !WB_REMOTE_TCP_HOST[0]) {
        s_task_remote = NULL; vTaskDelete(NULL); return;
    }

    // 等 WiFi 拿到 IP(与本地服务任务同款等待)。
    esp_netif_ip_info_t ip;
    for (;;) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    char uri[96];
    snprintf(uri, sizeof(uri), "wss://%s", WB_REMOTE_HOST);
    bool ws_started = false;
    uint32_t last_tcp_try = 0;

    for (;;) {
        if (!s_running) break;
        const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        // ---- 通道①:裸 TCP 直连(槽位空闲时;WS 兜底在跑则每 60s 才重试一次,
        //      避免 v6 不可达的网络里反复空拨) ----
        if (s_client < 0 && WB_REMOTE_TCP_HOST[0] &&
            (!ws_started || now - last_tcp_try >= 60000)) {
            last_tcp_try = now;
            const int fd = remote_tcp_dial();
            if (fd >= 0) {
                if (ws_started) ws_stop_and_free();
                remote_tcp_tune(fd);
                if (claim_client(fd, true)) {
                    // hello 握手行(与守护端 token 校验一致)。直接 send,
                    // 不走 wb_wifi_link_send(它有 4KB 栈上缓冲,这个任务栈小)。
                    char hello[96];
                    const int hn = snprintf(hello, sizeof(hello),
                        "{\"wb_hello\":\"FoloPassport\",\"token\":\"%s\"}\n",
                        WB_REMOTE_TOKEN);
                    const int rc = send(fd, hello, hn, 0);
                    if (rc == hn) {
                        wb_ble_route_notify(true);
                        ESP_LOGW(TAG, "守护已连上(远程 TCP %s:%d)",
                                 WB_REMOTE_TCP_HOST, WB_REMOTE_TCP_PORT);
                        // 链路存活期间驻留;槽位被回收(对端断/发送失败/本地
                        // 抢占)后退出,走下一轮重拨。
                        while (s_running && s_client == fd) {
                            vTaskDelay(pdMS_TO_TICKS(500));
                        }
                        ESP_LOGW(TAG, "远程 TCP 断开,准备重连");
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        continue;
                    }
                    ESP_LOGW(TAG, "远程 hello 发送失败 rc=%d", rc);
                    close_client("远程 hello 发送失败");
                } else {
                    closesocket(fd);    // 槽位被本地抢了,本地优先
                }
            }
        }

        if (ws_started) {
            // ---- 通道②:WSS 兜底(认领/回收沿用原机制,本地优先) ----
            if (s_ws_connected && !s_ws_claimed && s_client < 0) {
                if (claim_client(WB_SLOT_WS, true)) {
                    s_ws_claimed = true;
                    wb_ble_route_notify(true);
                    ws_send_hello();
                }
            }
            if (!s_ws_connected && s_ws_claimed) {
                // 竞态兜底:断开事件与认领交错时在这里回收。
                ws_release_if_owned("状态复位");
            }
            static uint32_t last_log;
            if (!s_ws_connected && now - last_log >= WB_REMOTE_LOG_MS) {
                last_log = now;
                ESP_LOGW(TAG, "远程 WS 未连接(%s),组件自动重试", uri);
            }
        } else if (WB_REMOTE_HOST[0] && s_client < 0) {
            // ---- TCP 拨不通:启用 WSS 兜底(组件自带断线自动重连) ----
            esp_websocket_client_config_t cfg = {
                .uri = uri,
                .buffer_size = 2048,
                .reconnect_timeout_ms = 5000,   // 断线自动重连间隔
                .network_timeout_ms = 10000,
                .task_stack = 6144,             // TLS+WS 握手吃栈,给足
                // 本 IDF 的 esp-tls 不允许"无校验源"的 TLS:不给证书直接
                // ESP_ERR_MBEDTLS_SSL_SETUP_FAILED。挂系统证书包(内置主流 CA,
                // 覆盖 Cloudflare 边缘证书),证书有效期校验依赖系统时间,
                // 联网后 SNTP 会同步,开机头几分钟若时间未同步可能握手失败重试。
                .crt_bundle_attach = esp_crt_bundle_attach,
            };
            s_ws = esp_websocket_client_init(&cfg);
            if (s_ws) {
                esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                              ws_event_handler, NULL);
                ESP_LOGW(TAG, "远程 TCP 不可达,启用 WSS 兜底: %s", uri);
                esp_websocket_client_start(s_ws);
                ws_started = true;
            } else {
                ESP_LOGE(TAG, "WS 客户端创建失败,10s 后重试");
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (ws_started && s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_ws_connected = false;
    if (s_client == WB_SLOT_WS) ws_release_if_owned("任务退出");
    s_task_remote = NULL;
    vTaskDelete(NULL);
}
#endif

void wb_wifi_link_start(void)
{
#if !WB_WIFI_LINK_ENABLED
    return;                              // 蓝牙版:链路任务不启动
#endif
    if (s_task) return;
    s_running = true;
    if (!s_tx_lock) s_tx_lock = xSemaphoreCreateMutex();
    if (!s_slot_lock) s_slot_lock = xSemaphoreCreateMutex();
    if (xTaskCreate(wifi_link_task, "wb_wifilnk", 6144, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "wifi_link 任务创建失败");
        s_task = NULL;
    }
#if WB_WIFI_LINK_ENABLED
    if (!s_task_remote && WB_REMOTE_HOST[0]) {
        // 8192:任务内做 getaddrinfo(域名解析是最吃栈的一段),6144 原值不够
        if (xTaskCreate(wb_remote_task, "wb_remote", 8192, NULL, 3, &s_task_remote) != pdPASS) {
            ESP_LOGE(TAG, "remote_link 任务创建失败");
            s_task_remote = NULL;
        }
    }
#endif
}
