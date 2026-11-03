// apps/wbconsole/wb_gatt.h —— WorkBuddy 伴生控制台 GATT 服务表声明。
// NimBLE 服务表只能在 host 启动前注册一次;Vokie 与 WB 控制台共用 host,
// 两边引导时都必须把双方服务表一起注册,否则先进入的应用会把后进入的
// 应用锁死(ble_gatts_add_svcs 在 host sync 后不再可用)。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ble_gatt_svc_def;

// 返回 WB 控制台服务表(实现在 wb_ble.c),以 {0} 结尾;进程内不变。
const struct ble_gatt_svc_def *wb_gatt_services(void);

// 共享 host 引导标志(定义在 vokie_ble.c)。NimBLE host 只能初始化一次:
// 谁先进场谁 nimble_port_init(),另一方只接管广播,否则重复 init 直接崩。
extern bool ble_host_shared_booted;

// 把 NimBLE 协议栈整块还给堆(约 60KB)。host 是「一次引导、永不释放」的,
// 退出 WorkBuddy 并不会回收;对讲机这类要开 Opus 编码器、需要大块连续内存
// 的应用进场前可调用,下次 WorkBuddy 进场会自动重新引导。
// 返回 ESP_OK 表示已释放或本来就没起;只在不用蓝牙的应用里调用。
esp_err_t wb_ble_release_host(void);

// 大内存应用(对讲机/小智)进场时用这个,而不是直接调 wb_ble_release_host:
// = 释放 host + 交出「内存锚」(64KB 连续块,host 释放后由 wb_ble 层锚住防
// 碎片化)。调用方随即拥有系统里最大的连续块;之后若再需要蓝牙,wb_ble 层
// 后台自动重试补引导,不需要调用方操心。
void wb_ble_guard_loan(void);

#ifdef __cplusplus
}
#endif
