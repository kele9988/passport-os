// live_discover.h —— 局域网内发现 PC 桥接程序（UDP 广播监听）。
//
// 与 WB 伴生的 wb_announce 同思路：电脑上的直播桥接程序每 2 秒在 UDP 7812
// 广播一条 {"live_bridge":"192.168.2.x","port":8787,"token":"..."}，设备端
// 被动接收即可拿到桥接地址，免配网。手动写进 NVS 的桥接地址优先级更高。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_DISCOVER_PORT 7812

typedef struct {
    bool valid;
    char host[64];
    uint16_t port;
    char token[64];
    uint32_t seq;          // 每条广播自增，用于判断内容是否变化
} live_discover_result_t;

// 启动 UDP 监听任务（幂等）。stop 前一直运行，收到广播就更新内部缓存。
bool live_discover_start(void);
void live_discover_stop(void);

// 取最近一次有效的广播内容（拷贝语义，线程安全）。
bool live_discover_get(live_discover_result_t *out);
#ifdef __cplusplus
}
#endif
