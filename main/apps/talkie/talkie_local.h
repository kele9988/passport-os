// 对讲机「近场模式」引擎：ESP-NOW 直连 + IMA-ADPCM + 发言权仲裁。
// 移植自 espnow-walkie-talkie（MIT, LeslieLeung）的 app_main 主循环，裁剪点：
//   * 无 VOX（WebRTC VAD 不移植），只保留按住说话；
//   * 无 cue 提示音、无息屏策略（这些由 Passport OS 的 UI/电源框架负责）；
//   * 音频不经 xiaozhi AudioService（那是 Opus/60ms 专用架构），直接驱动
//     bsp_audio——调用方保证进入本地模式前 AudioService 已 Deinit。
// 协议与原版完全兼容（WKT1，射频信道一致即可互通）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int channel;      // 逻辑频道 1..4
    int radio_channel;  // ESP-NOW 射频信道 1..13（连 WiFi 跟随 AP，未连固定 6）
    int peers;        // 在线设备数(不含本机)
    int tx;           // 本机讲话中
    int rx;           // 正在收听
    int busy;         // 频道被占(对方在讲)
    int requesting;   // 正在申请发言权
    int weak;         // 信号弱/丢帧
    char speaker[9];  // 当前发言人名(收听时)，"AP-XXXX"
} talkie_local_snapshot_t;

// 启动本地模式（ESP-NOW + 音频任务）。失败返回 false（UI 提示）。
bool talkie_local_start(void);
void talkie_local_stop(void);

// PTT：按下/松开（上键）。仲裁结果由内部状态机处理。
void talkie_local_set_ptt(bool pressed);

// 切换逻辑频道 CH1..CH4，写入 NVS。
void talkie_local_cycle_channel(void);          // +1（下一频道）
void talkie_local_shift_channel(int delta);     // 步进：+1 下一频道 / -1 上一频道

// 音量 0..100，立即生效。
void talkie_local_set_volume(int percent);

// UI 渲染数据。
bool talkie_local_get_snapshot(talkie_local_snapshot_t *out);

// 波形柱数据：最近一帧音频载荷字节数（收发都算），取走即清零。
int talkie_local_pop_wave(void);

#ifdef __cplusplus
}
#endif
