#pragma once

// 对讲机网络层：连中转服务器、加入房间、收发 Opus 帧。
//
// 线程模型：
//   * 一条 talkie_net 任务独占 socket：连接、JOIN、收包、心跳全在它里面。
//   * 收到的音频帧进一个固定槽位的小队列，由 app 主任务取走喂给 AudioService。
//     不在网络任务里直接推音频 —— AudioService 的播放启停必须和录音在
//     同一条任务里做（半双工互斥），混进网络任务会打架。
//   * 发送加互斥量：一帧 240 字节，内核缓冲够，加 SO_SNDTIMEO 兜底，
//     超时就丢帧 —— 对讲机宁可少一帧也不能把界面卡住。
//
// 关于 IPv6：这台机器(C3)的剩余堆只有十几 KB，开 TLS 必挂，所以走裸 TCP。
// 地址解析优先 IPv6（飞牛 DDNS 的方案），拿不到再退回 IPv4（隧道中转方案），
// 两种部署方式都不用改固件。

#include <cstddef>
#include <cstdint>

namespace talkie {

// 服务器与房间配置，存在 NVS 的 talkie 命名空间里，改配置不用重烧固件。
struct Config {
    char host[64];      // 域名 / IPv4 / IPv6 字面量
    uint16_t port;
    char room[5];       // 4 位房间号
    char token[5];      // 4 位口令，空表示服务器不校验
};

// 读 NVS；没有配置时给一组安全默认值（host 为空 = 未配置）。
void config_load(Config *out);
bool config_save(const Config &cfg);
// 串口调试用：host[:port] 形式，直接写进 NVS。
bool config_set_host(const char *text);
bool config_set_room(const char *text);

enum class State : uint8_t {
    kIdle = 0,        // 没连（未配置 / 已停止）
    kConnecting,      // 建连中
    kJoined,          // 已在房间里
    kFailed,          // 连不上或握手失败
};

// ---- 状态查询：任意任务可调，全部是原子读 ----
State state();
bool joined();
int peers();            // 房间在线人数（含自己）
bool remote_talking();  // 房间里别人正在讲话
int rtt_ms();           // 最近一次心跳往返时延，0 = 还没测到
const char *last_error();

// ---- 生命周期：app 的 start/stop 里调 ----
bool start();           // 用 NVS 配置起连接，异步，立刻返回
void stop();            // 断链、停任务

// ---- app 主任务调用 ----
// 取一帧收到的 Opus。返回 0 表示当前没有。不阻塞。
size_t pop_audio(uint8_t *out, size_t cap);
// 发一帧 Opus。失败(含超时/未连接)返回 false，调用方直接丢帧即可。
bool send_audio(const uint8_t *data, size_t len);
// 本端 PTT 状态变化，通知服务器让同房间其他人显示「对方讲话中」。
void set_talking(bool on);

}  // namespace talkie
