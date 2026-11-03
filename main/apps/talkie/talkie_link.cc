// 对讲机网络层实现。协议细节见 talkie_proto.h，服务端见 talkie_server/relay.py。
#include "apps/talkie/talkie_link.h"

#include "apps/talkie/talkie_proto.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace talkie {
namespace {

const char *TAG = "talkie_net";

constexpr const char *kNvsNs = "talkie";
constexpr uint16_t kDefaultPort = 7801;
// 默认房间号：可用 CMake -DPASSPORT_DEFAULT_ROOM=<4位数字> 注入
// （编译为 TALKIE_DEFAULT_ROOM 宏），私享分发版让朋友与你同房间直接通话。
#ifdef TALKIE_DEFAULT_ROOM
constexpr const char *kDefaultRoom = TALKIE_DEFAULT_ROOM;
#else
constexpr const char *kDefaultRoom = "1234";
#endif
// 默认中转主机。
// 开发版：飞牛（relay 服务端）在用户家庭局域网内的 IPv4 地址。设备与它同二层，
// 走 IPv4 直连最稳：没有 IPv6 链路本地(fe80)下行不可达、公网 IPv6 地址过期、
// DNS 仅 AAAA 导致只能走 IPv6 等坑。当 NVS 里的 host 经 DNS 只解析到 AAAA
// 时，dial() 会把这里作为 IPv4 兜底候选，确保设备始终能连上。
// 公开发行版（PASSPORT_PUBLIC_RELEASE=ON 时由 CMake 定义 TALKIE_PUBLIC_RELEASE）
// 默认不预置任何私有地址，使用者自行配置中转主机，避免泄露作者私网信息。
// 私享分发版可通过 CMake -DPASSPORT_DEFAULT_HOST=<域名或IP> 注入默认中转主机
// （编译为 TALKIE_DEFAULT_HOST 宏），朋友刷完即可直连指定中转。
#ifdef TALKIE_PUBLIC_RELEASE
#ifdef TALKIE_DEFAULT_HOST
constexpr const char *kDefaultHost = TALKIE_DEFAULT_HOST;
#else
constexpr const char *kDefaultHost = "";
#endif
#else
constexpr const char *kDefaultHost = "192.168.2.5";
#endif

constexpr int kRxSlots = 6;             // 约 0.36 秒音频余量
constexpr size_t kRxSlotBytes = proto::kMaxFrame;
constexpr int kConnectTimeoutSec = 8;
constexpr int kHeartbeatMs = 10000;
constexpr int kRetryMs = 4000;
constexpr int kRecvTimeoutMs = 500;     // SO_RCVTIMEO:顺便当内务节拍

// ---------------------------------------------------------------- 共享状态

SemaphoreHandle_t s_lock = nullptr;     // 保护 socket 写入
int s_sock = -1;
TaskHandle_t s_task = nullptr;
std::atomic<bool> s_run{false};
// 任务代际：start() 每次递增。net_task/heartbeat_task 持有自己的代际，
// 一旦发现不符（快速切房间时 stop() 等不满、start() 又起新任务）立即自行
// 退出，且只清理自己名下的资源——否则旧任务会在新代际里复活，带着旧房间
// 号继续拨号、抢写 s_sock，反复切频道还会把任务栈(8KB+2.5KB/次)漏到堆耗尽。
std::atomic<uint32_t> s_gen{0};
std::atomic<State> s_state{State::kIdle};
std::atomic<int> s_peers{0};
std::atomic<bool> s_remote_talking{false};
std::atomic<int> s_rtt_ms{0};
std::atomic<uint8_t> s_tx_seq{0};

char s_err[48] = "";

// 收包队列：网络任务写 head，app 任务读 tail。
uint8_t s_rx[kRxSlots][kRxSlotBytes];
uint16_t s_rx_len[kRxSlots];
std::atomic<int> s_rx_head{0};
std::atomic<int> s_rx_tail{0};
SemaphoreHandle_t s_rx_lock = nullptr;

void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", s_err);
}

void rx_push(const uint8_t *data, size_t len) {
    if (len == 0 || len > kRxSlotBytes) return;
    if (xSemaphoreTake(s_rx_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    const int head = s_rx_head.load();
    const int next = (head + 1) % kRxSlots;
    if (next == s_rx_tail.load()) {
        // 满了：丢掉最旧的一帧，保实时
        s_rx_tail.store((s_rx_tail.load() + 1) % kRxSlots);
    }
    memcpy(s_rx[head], data, len);
    s_rx_len[head] = static_cast<uint16_t>(len);
    s_rx_head.store(next);
    xSemaphoreGive(s_rx_lock);
}

// ---------------------------------------------------------------- NVS 配置

bool nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) == ESP_OK) return true;
    out[0] = '\0';
    return false;
}

}  // namespace

void config_load(Config *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->port = kDefaultPort;
    std::snprintf(out->room, sizeof(out->room), "%s", kDefaultRoom);

    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;

    nvs_read_str(h, "host", out->host, sizeof(out->host));
    nvs_read_str(h, "room", out->room, sizeof(out->room));
    nvs_read_str(h, "token", out->token, sizeof(out->token));

    uint16_t port = kDefaultPort;
    if (nvs_get_u16(h, "port", &port) == ESP_OK && port > 0) out->port = port;
    nvs_close(h);

    if (out->host[0] == '\0') std::snprintf(out->host, sizeof(out->host), "%s", kDefaultHost);
    if (out->room[0] == '\0') std::snprintf(out->room, sizeof(out->room), "%s", kDefaultRoom);
    // 房间号必须是 4 位，少补 0、多截断 —— 服务器按 4 字节匹配，长度错了就进错房间。
    char fixed[5] = {'0', '0', '0', '0', 0};
    for (int i = 0; i < 4 && out->room[i]; ++i) fixed[i] = out->room[i];
    memcpy(out->room, fixed, sizeof(fixed));
}

bool config_save(const Config &cfg) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "host", cfg.host);
    if (err == ESP_OK) err = nvs_set_u16(h, "port", cfg.port);
    if (err == ESP_OK) err = nvs_set_str(h, "room", cfg.room);
    if (err == ESP_OK) err = nvs_set_str(h, "token", cfg.token);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool config_set_host(const char *text) {
    if (!text || !*text) return false;
    Config cfg;
    config_load(&cfg);

    const char *p = text;
    const char *port_at = nullptr;
    if (*p == '[') {                       // [::1]:7801
        const char *close = strchr(p, ']');
        if (!close) return false;
        const size_t n = static_cast<size_t>(close - p - 1);
        if (n == 0 || n >= sizeof(cfg.host)) return false;
        memcpy(cfg.host, p + 1, n);
        cfg.host[n] = '\0';
        if (close[1] == ':') port_at = close + 2;
    } else {
        const char *colon = strchr(p, ':');
        // 出现多个冒号说明是裸 IPv6 字面量，此时冒号不是端口分隔符
        if (colon && strchr(colon + 1, ':') == nullptr) {
            const size_t n = static_cast<size_t>(colon - p);
            if (n == 0 || n >= sizeof(cfg.host)) return false;
            memcpy(cfg.host, p, n);
            cfg.host[n] = '\0';
            port_at = colon + 1;
        } else {
            std::snprintf(cfg.host, sizeof(cfg.host), "%s", p);
        }
    }
    if (port_at && *port_at) {
        const int v = atoi(port_at);
        if (v > 0 && v < 65536) cfg.port = static_cast<uint16_t>(v);
    }
    const bool ok = config_save(cfg);
    ESP_LOGI(TAG, "服务器设为 %s:%u (ok=%d)", cfg.host, cfg.port, ok);
    return ok;
}

bool config_set_room(const char *text) {
    if (!text || !*text) return false;
    Config cfg;
    config_load(&cfg);
    char fixed[5] = {'0', '0', '0', '0', 0};
    for (int i = 0; i < 4 && text[i]; ++i) fixed[i] = text[i];
    memcpy(cfg.room, fixed, sizeof(fixed));
    const bool ok = config_save(cfg);
    ESP_LOGI(TAG, "房间号设为 %s (ok=%d)", cfg.room, ok);
    return ok;
}

// ---------------------------------------------------------------- 状态查询

State state() { return s_state.load(); }
bool joined() { return s_state.load() == State::kJoined; }
int peers() { return s_peers.load(); }
bool remote_talking() { return s_remote_talking.load(); }
int rtt_ms() { return s_rtt_ms.load(); }
const char *last_error() { return s_err; }

// ---------------------------------------------------------------- 收发

size_t pop_audio(uint8_t *out, size_t cap) {
    if (!out || !s_rx_lock) return 0;
    if (xSemaphoreTake(s_rx_lock, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    size_t got = 0;
    const int tail = s_rx_tail.load();
    if (tail != s_rx_head.load()) {
        const size_t n = s_rx_len[tail];
        if (n <= cap) {
            memcpy(out, s_rx[tail], n);
            got = n;
        }
        s_rx_tail.store((tail + 1) % kRxSlots);
    }
    xSemaphoreGive(s_rx_lock);
    return got;
}

bool send_audio(const uint8_t *data, size_t len) {
    if (!data || len == 0 || len > proto::kMaxFrame) return false;
    if (s_sock < 0 || !joined()) return false;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(120)) != pdTRUE) return false;

    uint8_t pkt[proto::kHeaderLen + proto::kMaxFrame];
    proto::put_header(pkt, proto::kTypeAudio, static_cast<uint16_t>(len), s_tx_seq.fetch_add(1));
    memcpy(pkt + proto::kHeaderLen, data, len);
    const int rc = send(s_sock, pkt, proto::kHeaderLen + len, 0);
    xSemaphoreGive(s_lock);
    if (rc < 0) ESP_LOGW(TAG, "send 失败 errno=%d", errno);
    return rc == static_cast<int>(proto::kHeaderLen + len);
}

namespace {

bool send_frame(uint8_t type, const uint8_t *payload, size_t len) {
    if (s_sock < 0) return false;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    uint8_t pkt[proto::kHeaderLen + 16];
    if (len > 16) len = 16;
    proto::put_header(pkt, type, static_cast<uint16_t>(len), s_tx_seq.fetch_add(1));
    if (len) memcpy(pkt + proto::kHeaderLen, payload, len);
    const int rc = send(s_sock, pkt, proto::kHeaderLen + len, 0);
    xSemaphoreGive(s_lock);
    return rc > 0;
}

// ---------------------------------------------------------------- 拨号

// 设备是否与 kDefaultHost(飞牛局域网 IP) 同处一个 /24 网段。
// 只有在家(同一台路由器)时局域网 IPv4 直连才可达；异地/外网时私网地址
// 不可达，试连只会白等 kConnectTimeoutSec 才超时，拖慢每一次重连。
bool on_same_lan() {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == nullptr) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) return false;
    // 192.168.2.0/24，addr 为网络字节序
    const uint32_t mask = lwip_htonl(0xFFFFFF00u);
    const uint32_t prefix = lwip_htonl(0xC0A80200u);   // 192.168.2.x
    return info.ip.addr != 0 && (info.ip.addr & mask) == prefix;
}

// 连接策略：
//  - 候选主机：配置里的 host 优先；仅当设备与飞牛同网段(在家)时追加
//    kDefaultHost（飞牛局域网 IPv4）作为兜底，保证局域网下走 IPv4 直连。
//    异地时域名解析出的公网 IPv6 是唯一通道，直接走它。
//  - 地址族：先试 IPv4 再试 IPv6。IPv4 在二层局域网里最稳，没有 IPv6 链路本地
//    (fe80) 下行不可达、公网 IPv6 地址过期等坑；只有所有 IPv4 候选都失败才退回 IPv6。
int dial(const Config &cfg) {
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(cfg.port));

    // 候选主机：配置 host 在前；同网段(在家)时才追加局域网兜底 IP。
    // 异地/外网时 192.168.2.5 不可达，绝不加入候选，避免白等连接超时。
    const char *hosts[2];
    int nhosts = 0;
    hosts[nhosts++] = cfg.host;
    if ((cfg.host[0] == '\0' || std::strcmp(cfg.host, kDefaultHost) != 0) && on_same_lan()) {
        hosts[nhosts++] = kDefaultHost;
    }

    // pass 0：所有主机的 IPv4；pass 1：所有主机的 IPv6（仅当 IPv4 全失败）
    for (int pass = 0; pass < 2; ++pass) {
        const int fam = (pass == 0) ? AF_INET : AF_INET6;
        for (int hi = 0; hi < nhosts; ++hi) {
            const char *host = hosts[hi];
            if (host[0] == '\0') continue;

            struct addrinfo hints;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = fam;
            hints.ai_socktype = SOCK_STREAM;

            struct addrinfo *res = nullptr;
            const int grc = getaddrinfo(host, port_str, &hints, &res);
            if (grc != 0 || res == nullptr) {
                ESP_LOGD(TAG, "getaddrinfo(%s, AF=%d) 无结果: %d", host, fam, grc);
                continue;
            }

            int s = -1;
            for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
                const int sock = socket(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
                if (sock < 0) continue;

                int flags = fcntl(sock, F_GETFL, 0);
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);

                int rc = connect(sock, ai->ai_addr, ai->ai_addrlen);
                bool ok = (rc == 0);
                if (!ok && errno == EINPROGRESS) {
                    fd_set wf;
                    FD_ZERO(&wf);
                    FD_SET(sock, &wf);
                    struct timeval tv;
                    tv.tv_sec = kConnectTimeoutSec;
                    tv.tv_usec = 0;
                    const int n = select(sock + 1, nullptr, &wf, nullptr, &tv);
                    if (n > 0) {
                        int so_err = 0;
                        socklen_t el = sizeof(so_err);
                        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &el) == 0 && so_err == 0) {
                            ok = true;
                        } else {
                            errno = so_err;
                        }
                    } else if (n == 0) {
                        errno = ETIMEDOUT;
                    }
                }

                if (!ok) {
                    set_error("连接 %s:%u 失败(%d)", host, cfg.port, errno);
                    close(sock);
                    continue;
                }

                fcntl(sock, F_SETFL, flags);
                struct timeval rtv;
                rtv.tv_sec = 0;
                rtv.tv_usec = kRecvTimeoutMs * 1000;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
                struct timeval stv;
                stv.tv_sec = 0;
                stv.tv_usec = 300 * 1000;
                setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
                int one = 1;
                setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

                ESP_LOGI(TAG, "已连接 %s:%u (AF=%s)", host, cfg.port,
                         ai->ai_family == AF_INET6 ? "inet6" : "inet");
                s = sock;
                break;
            }
            freeaddrinfo(res);
            if (s >= 0) return s;
        }
    }
    return -1;
}

// 读满 n 字节，处理 EAGAIN(超时)与对端关闭。
// 返回 true 表示读满；false 表示连接已不可用。
bool recv_exact(int s, uint8_t *buf, size_t n, std::atomic<bool> *running) {
    size_t got = 0;
    while (got < n) {
        if (running && !running->load()) return false;
        const int rc = recv(s, buf + got, n - got, 0);
        if (rc > 0) {
            got += static_cast<size_t>(rc);
        } else if (rc == 0) {
            return false;                        // 对端正常关闭
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // 只是接收超时
            if (errno == EINTR) continue;
            return false;
        }
    }
    return true;
}

bool do_join(int s, const Config &cfg, std::atomic<bool> *running) {
    uint8_t payload[12];
    // 房间号 4 字节；设备名取本机 MAC 后两字节的可打印形式，服务器日志里能认人
    memcpy(payload, cfg.room, 4);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(reinterpret_cast<char *>(payload + 4), 5, "%02X%02X", mac[4], mac[5]);
    memset(payload + 8, 0, 4);
    if (cfg.token[0]) memcpy(payload + 8, cfg.token, 4);

    if (!send_frame(proto::kTypeJoin, payload, sizeof(payload))) {
        set_error("JOIN 发送失败");
        return false;
    }

    uint8_t hdr[proto::kHeaderLen];
    if (!recv_exact(s, hdr, sizeof(hdr), running)) {
        set_error("等回包时连接断开");
        return false;
    }
    if (hdr[0] != proto::kMagic0 || hdr[1] != proto::kMagic1) {
        set_error("服务器回包格式不对(可能不是中转服务)");
        return false;
    }
    const uint8_t type = hdr[2];
    const uint16_t len = proto::get_u16(hdr + 3);
    uint8_t body[64] = {0};
    if (len > sizeof(body)) {
        set_error("回包过长 %u", len);
        return false;
    }
    if (len && !recv_exact(s, body, len, running)) {
        set_error("读回包内容失败");
        return false;
    }
    if (type != proto::kTypeJoinAck || len < 6) {
        set_error("不是 JOIN_ACK (type=0x%02x)", type);
        return false;
    }

    switch (body[0]) {
        case proto::kResultOk:
            break;
        case proto::kResultFull:
            set_error("房间 %s 已满", cfg.room);
            return false;
        case proto::kResultToken:
            set_error("房间口令不对");
            return false;
        default:
            set_error("服务器拒绝: %u", body[0]);
            return false;
    }

    s_peers.store(body[1]);
    ESP_LOGI(TAG, "已进入房间 %s，在线 %u 人，本机编号 #%u",
             cfg.room, body[1], proto::get_u32(body + 2));
    return true;
}

void drop_link(const char *why) {
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_sock >= 0) {
            close(s_sock);
            s_sock = -1;
        }
        xSemaphoreGive(s_lock);
    }
    s_state.store(State::kFailed);
    s_peers.store(0);
    s_remote_talking.store(false);
    if (why) set_error("%s", why);
}

// ---------------------------------------------------------------- 网络任务

struct TaskArg {
    Config cfg;
    uint32_t gen;
};

void net_task(void *arg) {
    TaskArg *a = static_cast<TaskArg *>(arg);
    const Config cfg = a->cfg;
    const uint32_t my_gen = a->gen;
    free(a);

    bool first = true;
    int owned_sock = -1;   // 本任务拨到、已写入 s_sock 的连接；drop_link 会关掉它
    while (s_run.load() && s_gen.load() == my_gen) {
        s_state.store(State::kConnecting);

        const int s = dial(cfg);
        if (s < 0) {
            if (s_gen.load() != my_gen || !s_run.load()) break;
            if (first) s_state.store(State::kFailed);
            vTaskDelay(pdMS_TO_TICKS(kRetryMs));
            first = false;
            continue;
        }
        // 拨号最长阻塞 kConnectTimeoutSec，期间代际可能已更替（快速切房间）。
        // 这个 socket 属于旧房间，必须关掉走人，绝不能写进 s_sock 顶撞新任务。
        if (s_gen.load() != my_gen || !s_run.load()) {
            close(s);
            break;
        }

        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        s_sock = s;
        owned_sock = s;
        if (s_lock) xSemaphoreGive(s_lock);
        first = false;

        if (!do_join(s, cfg, &s_run)) {
            if (s_gen.load() != my_gen) break;
            drop_link(nullptr);
            owned_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(kRetryMs));
            continue;
        }

        s_state.store(State::kJoined);
        s_err[0] = '\0';
        // 栈水位:域名解析(getaddrinfo)是最吃栈的一段,这里打的是它的低谷值。
        ESP_LOGI(TAG, "已入房, net_task 栈余 %u B, 堆余 %u B",
                 (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
                 (unsigned)esp_get_free_heap_size());
        int64_t last_hb = esp_timer_get_time();

        // ---- 收包主循环 ----
        bool link_ok = true;
        while (s_run.load() && s_gen.load() == my_gen && link_ok) {
            uint8_t hdr[proto::kHeaderLen];
            if (!recv_exact(s, hdr, sizeof(hdr), &s_run)) {
                link_ok = false;
                break;
            }
            if (hdr[0] != proto::kMagic0 || hdr[1] != proto::kMagic1) {
                link_ok = false;
                set_error("帧头错乱，断开重连");
                break;
            }
            const uint8_t type = hdr[2];
            const uint16_t len = proto::get_u16(hdr + 3);
            if (len > proto::kMaxFrame) {
                link_ok = false;
                set_error("帧过大 %u，断开重连", len);
                break;
            }
            uint8_t body[proto::kMaxFrame];
            if (len && !recv_exact(s, body, len, &s_run)) {
                link_ok = false;
                break;
            }

            switch (type) {
                case proto::kTypeAudio:
                    rx_push(body, len);
                    break;
                case proto::kTypePeers:
                    if (len >= 1) s_peers.store(body[0]);
                    break;
                case proto::kTypeTalk:
                    if (len >= 5) s_remote_talking.store(body[0] != 0);
                    break;
                case proto::kTypePong:
                    if (len >= 8) {
                        const int64_t sent = static_cast<int64_t>(proto::get_u32(body)) << 32 |
                                             proto::get_u32(body + 4);
                        const int ms = static_cast<int>((esp_timer_get_time() - sent) / 1000);
                        s_rtt_ms.store(ms > 0 ? ms : 1);
                    }
                    break;
                default:
                    break;
            }
        }

        if (!s_run.load() || s_gen.load() != my_gen) break;
        drop_link("链路断开，正在重连");
        owned_sock = -1;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 退出清理：只动自己名下的 socket；共享状态仅当代际仍是自己时才复位，
    // 避免旧任务退出时踩掉新任务刚建好的状态。
    const bool current = (s_gen.load() == my_gen);
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (owned_sock >= 0 && s_sock == owned_sock) {
            close(s_sock);
            s_sock = -1;
        }
        xSemaphoreGive(s_lock);
    }
    if (current) {
        s_state.store(State::kIdle);
        s_peers.store(0);
        s_remote_talking.store(false);
    }
    if (s_task == xTaskGetCurrentTaskHandle()) s_task = nullptr;
    ESP_LOGI(TAG, "网络任务已退出 (gen=%u)", static_cast<unsigned>(my_gen));
    vTaskDelete(nullptr);
}

// 心跳单独一条小任务：收包主循环会被 recv 超时切片，但它本身不适合兼任定时器，
// 因为一旦对端完全静默，recv 每次都要等满 500ms 才返回，心跳粒度会被拉粗。
void heartbeat_task(void *arg) {
    // 代际不符说明已有新一轮 start()，本任务立即让位，否则每次 start()
    // 都会漏一个心跳任务（2560B 栈）直到堆耗尽。
    const uint32_t my_gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    while (s_run.load() && s_gen.load() == my_gen) {
        // 先把间隔睡掉再判断:未入房时也得周期性起来看一眼,否则刚连上那会儿
        // 会白等一个完整心跳周期才发出第一个 PING,界面上的 RTT 一直是 --ms。
        if (!joined()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint8_t body[8];
        const int64_t now = esp_timer_get_time();
        proto::put_u32(body, static_cast<uint32_t>(now >> 32));
        proto::put_u32(body + 4, static_cast<uint32_t>(now & 0xFFFFFFFF));
        send_frame(proto::kTypePing, body, sizeof(body));
        vTaskDelay(pdMS_TO_TICKS(kHeartbeatMs));
    }
    vTaskDelete(nullptr);
}

}  // namespace

// ---------------------------------------------------------------- 对外接口

bool start() {
    if (s_run.load()) return true;

    Config cfg;
    config_load(&cfg);
    if (cfg.host[0] == '\0') {
        set_error("还没设置对讲服务器");
        s_state.store(State::kIdle);
        return false;
    }

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_rx_lock) s_rx_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_rx_lock) {
        set_error("信号量创建失败");
        return false;
    }

    s_rx_head.store(0);
    s_rx_tail.store(0);
    s_peers.store(0);
    s_remote_talking.store(false);
    s_rtt_ms.store(0);
    s_err[0] = '\0';
    s_run.store(true);
    s_state.store(State::kConnecting);

    const uint32_t gen = s_gen.fetch_add(1) + 1;
    auto *arg_heap = static_cast<TaskArg *>(malloc(sizeof(TaskArg)));
    if (!arg_heap) {
        s_run.store(false);
        set_error("内存不足");
        return false;
    }
    arg_heap->cfg = cfg;
    arg_heap->gen = gen;

    // 8192:任务栈里除 512B 收包缓冲外,还要跑 getaddrinfo(域名解析期间的
    // dns_api_msg 和日志缓冲都落在栈上)。原先 6144 余量偏紧,实测出现过
    // 连接后整机静默(04:24 那次),加大以排除栈溢出这一可能。
    if (xTaskCreate(net_task, "talkie_net", 8192, arg_heap, 5, &s_task) != pdPASS) {
        free(arg_heap);
        s_run.store(false);
        set_error("网络任务创建失败(内存不足)");
        return false;
    }
    xTaskCreate(heartbeat_task, "talkie_hb", 2560,
                reinterpret_cast<void *>(static_cast<uintptr_t>(gen)), 3, nullptr);
    ESP_LOGI(TAG, "连接 %s:%u 房间 %s (gen=%u)", cfg.host, cfg.port, cfg.room,
             static_cast<unsigned>(gen));
    return true;
}

void stop() {
    if (!s_run.exchange(false)) return;
    // 网络任务会在 recv 超时(≤500ms)内看到 s_run 变 false 并自行退出；
    // 拨号中的任务最长还要等一个连接超时，靠代际机制保证它退出时绝不
    // 干扰下一轮 start()，所以这里不无限等，避免调用方(app stop)被网络阻塞拖住。
    for (int i = 0; i < 40 && s_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "link 已停止");
}

void set_talking(bool on) {
    if (!joined()) return;
    const uint8_t body = on ? 1 : 0;
    send_frame(proto::kTypeTalkReq, &body, 1);
}

}  // namespace talkie
