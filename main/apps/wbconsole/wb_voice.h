// apps/wbconsole/wb_voice.h —— WorkBuddy 语音录入采集端。
// 帧格式(与 PC 端 wb_app.py _adpcm_decode 逐字节对齐):
//   163B/帧 = 3B 头(int16 预测器 LE + int8 步进索引,均为本帧编码前快照)
//           + 160B nibble(低半字节在前),每帧 320 样本 @16kHz。
// 每帧自带解码状态快照,丢帧不污染后续音频。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WB_VOICE_FRAME_BYTES 163
#define WB_VOICE_FRAME_SAMPLES 320
#define WB_VOICE_SAMPLE_RATE 16000

typedef struct {
    int session;        // 录音会话号(每次开始递增)
    bool recording;     // 采集中(松手后为 false,但缓冲可能未排空)
    uint32_t sequence;  // 本批首帧序号
    int pending;        // 本批携带帧数之后仍待取的帧数
    int peak;           // 波形峰值(0..32767,录音期最大值)
    int dropped;        // 环形缓冲溢出丢弃帧数
    int clipped;        // 削顶样本数(|x|>32000)
    int samples;        // 本次录音累计样本数
    int stop_reason;    // 0 未停 1 正常松开 3 达到上限 4 链路断开
} wb_voice_status_t;

// 采集任务随 app start/stop 启停;空闲时挂起不占 CPU。
esp_err_t wb_voice_init(void);
void wb_voice_deinit(void);

// 开始一次录音(UP 按下);返回新会话号,0=失败(麦克风忙/不可用)。
int wb_voice_begin(void);
// 结束录音(UP 松开/超限/断连)。reason: 1 正常松开 3 上限 4 断链。
void wb_voice_end(int reason);

// 取一行 poll 应答(不含结尾 '\n')。由 BLE 写回调上下文调用:
// 从环形缓冲取出至多 max_frames 帧,base64 后组 JSON;无数据也返回状态行。
// 返回写入长度(0=缓冲不足)。
size_t wb_voice_build_poll_response(char *out, size_t cap, int max_frames);


// 当前录音的实时峰值(0..32767)。录音中持续上升,松手后保留最后一次值,
// 供设备端 VU 表显示真实电平 —— 协议本身不回传 VU,画面不能凭空摆动。
int wb_voice_live_peak(void);

#ifdef __cplusplus
}
#endif
