// apps/wbconsole/wb_wifi_link.h —— 伴生链路 WiFi TCP 通道。
// 协议与 BLE 通道完全一致(JSON 行 + '\n');PC 守护经 UDP 7811 广播发现设备,
// 连 TCP 7810 通信。BLE 保留为兜底(设备不在 WiFi 网段时)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动 WiFi 链路(幂等):等 WiFi 拿到 IP 后开 TCP 服务 + UDP 广播,常驻。
// 由首页 start() 调一次即可;WiFi 未连接时任务内部轮询等待,连上自动就绪。
void wb_wifi_link_start(void);

// TCP 通道是否已有 PC 客户端。
bool wb_wifi_link_connected(void);

// 发送一行(自动补 '\n';TCP 自分段)。返回 0 成功,负数失败。
int wb_wifi_link_send(const char *data, size_t len);

#ifdef __cplusplus
}
#endif
