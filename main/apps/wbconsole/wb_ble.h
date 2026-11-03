// apps/wbconsole/wb_ble.h —— WorkBuddy 伴生控制台 BLE 链路。
// 协议:PC(wb_app.py) ⇄ 设备,行式 JSON + '\n',写+通知共用一个特征。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // 一行完整上行(PC→设备,已按 '\n' 拼装;可能是 JSON 也可能是裸帧)。
    // 注意:语音 poll 不走这里(wb_ble 内部直通 wb_voice)。
    void (*on_line)(const char *line, size_t len);
    // 连接状态变化(connected=true 表示 PC 已连上)。
    void (*on_link)(bool connected);
} wb_ble_callbacks_t;

// 启动/停止链路。host 常驻;stop 仅断连+停广播。
esp_err_t wb_ble_start(void);
void wb_ble_stop(void);
bool wb_ble_connected(void);

// 「常可见」模式:引导 host(若未引导)并持续广播,让 PC 端开机即可发现并
// 连上设备,无需先进 WorkBuddy/Vokie 应用。供首页 start() 调用;
// 应用退出(wb_ble_stop)后也会继续广播。visible_stop 恢复旧行为。
esp_err_t wb_ble_visible_start(void);
void wb_ble_visible_stop(void);

void wb_ble_set_callbacks(const wb_ble_callbacks_t *cb);

// 大内存应用(对讲机/小智)进场时调用:释放蓝牙 host 并把「内存锚」(64KB
// 连续块,host 释放后由 wb_ble 层锚住防碎片化)一并交出,调用方随即拥有
// 系统里最大的连续块。之后若再需要蓝牙,wb_ble 层后台自动重试补引导。
void wb_ble_guard_loan(void);

// 发送一行(自动补 '\n',按 MTU 分片 notify;调用方线程,内部有界重试)。
// 返回 0 成功。len 不含结尾符,超 4KB 拒发。
int wb_ble_send_line(const char *data, size_t len);

// 当前 MTU(带 3 字节 ATT 头扣除前);未连接返回 0。
uint16_t wb_ble_mtu(void);

// ---- 双通道共用入口(WiFi TCP / BLE 底层完全对应用层透明) ----

// WiFi 通道收到的下行字节注入(与 BLE GATT 写回调共用环形缓冲与分发)。
void wb_ble_inject_rx(const uint8_t *data, size_t len);

// WiFi 通道连接状态变化时回调:沿用 BLE 的 on_link 链路通知应用 UI。
void wb_ble_route_notify(bool up);

#ifdef __cplusplus
}
#endif
