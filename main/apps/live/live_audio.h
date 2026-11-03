// main/app_audio.h —— 桥接端下发的语音播放。
//
// 语音是"流"而不是"文件"：桥接端合成完一段就推一段 PCM，设备边收边放，
// 这样 6 秒的欢迎词不需要先占 192 KB 内存再开口（C3 没有 PSRAM）。
// 播放任务独占 I2S 写入，网络任务只往环形缓冲里投数据。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 采样格式与桥接端约定为 16 kHz / 16 bit / 单声道。
#define APP_AUDIO_SAMPLE_RATE 16000
#define APP_AUDIO_CHANNELS    1
#define APP_AUDIO_BITS        16

// 初始化 codec 与播放任务。volume 为 0..100。
esp_err_t app_audio_init(uint8_t volume);

void app_audio_set_volume(uint8_t volume);
uint8_t app_audio_volume(void);

// 开始一句话：清空缓冲并绑定 speak_id。返回 false 表示音频子系统不可用。
bool app_audio_begin(uint16_t speak_id);

// 投递一帧 PCM。speak_id 与当前句子不符的帧会被丢弃（barge-in 之后的残留）。
// last=true 表示这句话的最后一帧，播完缓冲区后触发结束回调。
bool app_audio_write(uint16_t speak_id, const int16_t *pcm, size_t samples, bool last);

// 立即停止当前播报并清空缓冲，不再触发完成回调。
void app_audio_stop(void);

// 退出应用时完整释放：停任务 → 关 codec → 归还音频工作区。之后可重新 init。
void app_audio_deinit(void);

bool app_audio_playing(void);
uint16_t app_audio_current_id(void);

// 结束回调在音频任务上下文执行，不能阻塞。
void app_audio_set_finished_callback(void (*callback)(uint16_t speak_id, bool completed), void *user);
#ifdef __cplusplus
}
#endif
