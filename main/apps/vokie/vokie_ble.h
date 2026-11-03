// apps/vokie/vokie_ble.h —— Vokie 桌面端 BLE 麦克风外设(passport-os 移植版)。
// 协议移植自社区开源项目 vokie-ai-passport-firmware
// (github.com/alanshen6666-collab/vokie-ai-passport-firmware, MIT License)。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动 BLE 外设与音频任务(幂等)。成功后以 "Vokie Passport" 广播。
// NimBLE host 若未引导则全新引导;stop/释放后重进会重新引导(约 1~2 秒)。
esp_err_t vokie_ble_start(void);

// 退出应用时调用:断开连接、停止广播。可重入。
void vokie_ble_stop(void);

// 彻底释放 Vokie 名下音频资源(采集任务 + codec/I2S DMA),配合
// wb_ble_release_host() 一起把退出后的堆还给后续应用。
void vokie_ble_free_audio(void);

// 主机(Vokie)是否已就绪(已连接且双通道订阅 + host_ready)。
bool vokie_ble_ready(void);

// 应用外壳的按键分发入口(launcher on_key 转发;含长按/松开语义)。
void vokie_ble_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, uint16_t mv);

#ifdef __cplusplus
}
#endif
