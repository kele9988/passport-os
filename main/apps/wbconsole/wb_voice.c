// apps/wbconsole/wb_voice.c —— WorkBuddy 语音录入采集端(见 wb_voice.h)。
// 采集:20ms 块读 bsp_audio → IMA-ADPCM → 环形缓冲;poll 时成批取走。
// 丢帧策略:缓冲满丢最旧(计 dropped),保证最新语音优先。
// sequence 语义(与 PC 端 VoiceSession.add 对齐):每帧一个递增序号,
// 应答行里的 sequence = 本批首帧的序号;PC 用"上批首帧+上批帧数"校验连续。
#include "wb_voice.h"

#include "bsp_audio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wb_voice";

#define WB_RING_FRAMES 48            // ~0.96s @50fps ≈ 7.8KB。原 120 帧(19.5KB)在对讲机
                                     // 等大块常驻内存的应用切换后，堆最大连续块只剩
                                     // ~8.7KB，分配必败 → 录音任务未创建 → 僵尸会话。
                                     // 轮询抽取 80帧/s > 产出 50帧/s，48 帧余量足够；
                                     // PC 停止轮询 >0.96s 才会丢帧，且有丢弃计数可观测。
#define WB_MAX_RECORD_MS 120000      // 官方上限 120s
#define WB_POLL_BATCH_MAX 8          // 单次 poll 最多带走 8 帧(160ms)

static const uint16_t k_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};
static const int8_t k_index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                         -1, -1, -1, -1, 2, 4, 6, 8};

static TaskHandle_t s_task;
static volatile bool s_capturing;    // 麦克风采集循环活跃态
static volatile bool s_recording;    // 松手前的"按住"态
static volatile bool s_stop_request;
static int s_session;
static int s_stop_reason;
static int64_t s_capture_start_us;

// 环形缓冲:帧定长,head=写位 tail=读位(取走即前进)。
// 按需分配(65KB):静态分配会把启动后空闲堆从 137KB 砸到 67KB。
static uint8_t *s_ring;
static volatile int s_ring_count;
static int s_ring_head;              // 下一写入位
static int s_ring_tail;              // 下一读出位
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

// 统计(采集任务写,poll 上下文读)。
static volatile uint32_t s_dropped;
static volatile uint32_t s_clipped;
static volatile uint32_t s_samples;
static volatile uint32_t s_peak;
static volatile uint32_t s_frames_total;   // 已入缓冲的帧数(序号基数)
static int s_adpcm_index;                  // 跨帧延续的编码器步进

static void ring_push(const uint8_t *frame)
{
    if (!s_ring) return;
    taskENTER_CRITICAL(&s_ring_lock);
    if (s_ring_count >= WB_RING_FRAMES) {
        s_ring_tail = (s_ring_tail + 1) % WB_RING_FRAMES;   // 丢最旧
        --s_ring_count;
        ++s_dropped;
    }
    memcpy(s_ring + s_ring_head * WB_VOICE_FRAME_BYTES, frame, WB_VOICE_FRAME_BYTES);
    s_ring_head = (s_ring_head + 1) % WB_RING_FRAMES;
    ++s_ring_count;
    ++s_frames_total;
    taskEXIT_CRITICAL(&s_ring_lock);
}

static int ring_pop(uint8_t *frame)
{
    int got = 0;
    if (!s_ring) return 0;
    taskENTER_CRITICAL(&s_ring_lock);
    if (s_ring_count > 0) {
        memcpy(frame, s_ring + s_ring_tail * WB_VOICE_FRAME_BYTES, WB_VOICE_FRAME_BYTES);
        s_ring_tail = (s_ring_tail + 1) % WB_RING_FRAMES;
        --s_ring_count;
        got = 1;
    }
    taskEXIT_CRITICAL(&s_ring_lock);
    return got;
}

static int ring_pending(void)
{
    taskENTER_CRITICAL(&s_ring_lock);
    const int n = s_ring_count;
    taskEXIT_CRITICAL(&s_ring_lock);
    return n;
}

// 编码 320 样本 → 163B 帧:3B 头(首样本 + 起始步进,均为解码起点快照)
// + 160B nibble。319 个样本 nibble + 1 个填充 nibble(高半字节 0);
// PC 端每帧从帧头重建解码器状态,多解出的 1 个样本不影响音频。
static void wb_encode_frame(const int16_t *pcm, uint8_t out[WB_VOICE_FRAME_BYTES])
{
    int predictor = pcm[0];
    int index = s_adpcm_index;
    out[0] = (uint8_t)((uint16_t)pcm[0] & 0xff);
    out[1] = (uint8_t)(((uint16_t)pcm[0] >> 8) & 0xff);
    out[2] = (uint8_t)index;
    uint8_t *payload = out + 3;
    int nibble = 0;
    for (int i = 1; i < WB_VOICE_FRAME_SAMPLES; ++i) {
        int diff = pcm[i] - predictor;
        int sign = diff < 0 ? 8 : 0;
        if (diff < 0) diff = -diff;
        const int step = k_step_table[index];
        int delta = 0, vpdiff = step >> 3;
        if (diff >= step) { delta |= 4; diff -= step; vpdiff += step; }
        if (diff >= (step >> 1)) { delta |= 2; diff -= step >> 1; vpdiff += step >> 1; }
        if (diff >= (step >> 2)) { delta |= 1; vpdiff += step >> 2; }
        const int code = delta | sign;
        predictor += sign ? -vpdiff : vpdiff;
        if (predictor > 32767) predictor = 32767;
        if (predictor < -32768) predictor = -32768;
        index += k_index_table[code];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        const int n = i - 1;
        if (nibble == 0) { payload[n / 2] = (uint8_t)code; nibble = 1; }
        else { payload[n / 2] |= (uint8_t)(code << 4); nibble = 0; }
    }
    if (nibble) payload[(WB_VOICE_FRAME_SAMPLES - 1) / 2] &= 0x0f; // 填充 nibble 清零
    s_adpcm_index = index;   // 下一帧的起始步进
}

static void finish_capture(void)
{
    s_capturing = false;
    s_recording = false;
    ESP_LOGI(TAG, "录音结束 session=%d 样本=%lu 帧=%lu 丢弃=%lu 削顶=%lu 峰值=%lu",
             s_session, (unsigned long)s_samples, (unsigned long)s_frames_total,
             (unsigned long)s_dropped, (unsigned long)s_clipped,
             (unsigned long)s_peak);
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t pcm[WB_VOICE_FRAME_SAMPLES];
    uint8_t frame[WB_VOICE_FRAME_BYTES];
    for (;;) {
        if (!s_capturing) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            ESP_LOGW(TAG, "麦克风读取失败,结束录音");
            s_stop_reason = 4;
            finish_capture();
            continue;
        }
        if (!s_capturing) continue;   // 读阻塞期间被结束
        int peak = (int)s_peak;
        for (int i = 0; i < WB_VOICE_FRAME_SAMPLES; ++i) {
            int32_t v = pcm[i];
            if (v < 0) v = -v;
            if (v > peak) peak = (int)v;
            if (v > 32000) ++s_clipped;
        }
        s_peak = (uint32_t)peak;
        s_samples += WB_VOICE_FRAME_SAMPLES;
        wb_encode_frame(pcm, frame);
        ring_push(frame);
        if (s_stop_request) {
            if (!s_stop_reason) s_stop_reason = 1;
            finish_capture();
            continue;
        }
        if (esp_timer_get_time() - s_capture_start_us >= WB_MAX_RECORD_MS * 1000LL) {
            ESP_LOGW(TAG, "达到 120 秒上限,自动停录");
            s_stop_reason = 3;
            s_stop_request = true;
            finish_capture();
        }
    }
}

int wb_voice_begin(void)
{
    if (s_capturing || s_recording) return 0;
    // 兜底:bringup 时环形缓冲/录音任务可能因堆碎片分配失败(app 未检查
    // wb_voice_init 返回值，失败即僵尸——报「开始录音」后无人采样、永不结束)。
    // 每次按住说话前重试一次初始化；仍失败则干净拒绝，不留半开状态。
    if (!s_ring || !s_task) {
        wb_voice_init();
        if (!s_ring || !s_task) {
            ESP_LOGE(TAG, "语音初始化不可用: 最大连续块=%uB, 需要 %dB",
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     WB_RING_FRAMES * WB_VOICE_FRAME_BYTES);
            return 0;
        }
    }
    // 麦克风采音需要先把编解码器切到采集模式(其它应用退出时可能处于播放态)。
    if (bsp_audio_init_capture() != ESP_OK) {
        ESP_LOGW(TAG, "切换采集模式失败");
        return 0;
    }
    if (bsp_audio_set_format(WB_VOICE_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "设置 16k 采样失败");
        return 0;
    }
    s_session = s_session >= 1000 ? 1 : s_session + 1;
    s_stop_reason = 0;
    s_stop_request = false;
    s_adpcm_index = 0;
    s_dropped = 0;
    s_clipped = 0;
    s_samples = 0;
    s_peak = 0;
    s_frames_total = 0;
    taskENTER_CRITICAL(&s_ring_lock);
    s_ring_count = 0;
    s_ring_head = s_ring_tail = 0;
    taskEXIT_CRITICAL(&s_ring_lock);
    s_capture_start_us = esp_timer_get_time();
    s_recording = true;
    s_capturing = true;
    if (s_task) xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "开始录音 session=%d", s_session);
    return s_session;
}

void wb_voice_end(int reason)
{
    if (!s_recording && !s_capturing) return;
    if (reason > 0) s_stop_reason = reason;
    s_stop_request = true;
}

size_t wb_voice_build_poll_response(char *out, size_t cap, int max_frames)
{
    if (!out || cap < 256) return 0;
    if (max_frames <= 0 || max_frames > WB_POLL_BATCH_MAX) max_frames = WB_POLL_BATCH_MAX;

    // 本批首帧序号 = 已入帧总数 - 待取帧数。
    const uint32_t first_seq = s_frames_total - (uint32_t)ring_pending();
    uint8_t batch[WB_POLL_BATCH_MAX * WB_VOICE_FRAME_BYTES];
    int count = 0;
    for (int i = 0; i < max_frames; ++i) {
        if (!ring_pop(batch + count * WB_VOICE_FRAME_BYTES)) break;
        ++count;
    }
    // recording 的下降沿是 PC 端唯一的"收完"信号:松手后到缓冲排空前仍报 true。
    const bool rec = s_recording || s_capturing || ring_pending() > 0 || count > 0;

    char b64[(WB_POLL_BATCH_MAX * WB_VOICE_FRAME_BYTES + 2) / 3 * 4 + 1];
    size_t b64_len = 0;
    b64[0] = '\0';
    if (count > 0 &&
        mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &b64_len,
                              batch, (size_t)count * WB_VOICE_FRAME_BYTES) != 0) {
        b64_len = 0;
        b64[0] = '\0';
    }
    return (size_t)snprintf(out, cap,
        "{\"voice\":1,\"session\":%d,\"recording\":%s,\"sequence\":%lu,"
        "\"pending\":%d,\"peak\":%lu,\"dropped\":%lu,\"stop_reason\":%d,"
        "\"clipped\":%lu,\"samples\":%lu,\"adpcm\":\"%s\"}",
        s_session, rec ? "true" : "false",
        (unsigned long)(count ? first_seq : s_frames_total),
        ring_pending(), (unsigned long)s_peak, (unsigned long)s_dropped,
        s_stop_reason, (unsigned long)s_clipped, (unsigned long)s_samples, b64);
}

esp_err_t wb_voice_init(void)
{
    if (!s_ring) {
        s_ring = (uint8_t *)heap_caps_malloc(
            (size_t)WB_RING_FRAMES * WB_VOICE_FRAME_BYTES, MALLOC_CAP_INTERNAL);
        if (!s_ring) {
            ESP_LOGE(TAG, "环形缓冲分配失败(%d B)", WB_RING_FRAMES * WB_VOICE_FRAME_BYTES);
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_task) return ESP_OK;
    if (xTaskCreate(audio_task, "wb_voice", 3072, NULL, 5, &s_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void wb_voice_deinit(void)
{
    // host 常驻设计:app 停止只停采集并释放缓冲,任务保留。
    if (s_capturing || s_recording) {
        s_stop_reason = 4;
        wb_voice_end(4);
    }
    bsp_audio_deinit();   // 归还音频硬件(下次进入再重建)
    if (s_ring) {
        heap_caps_free(s_ring);
        s_ring = NULL;
    }
}

int wb_voice_live_peak(void)
{
    return (int)s_peak;
}
