// live_audio.c —— PCM 流式播放实现（移植自 AI Passport app_audio.c）。
#include "live_audio.h"

#include "bsp_audio.h"
#include "bsp_audio_workspace.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "live_audio";

// 环形缓冲 0.5 秒（16 KB，放在音频工作区静态存储里，不占堆）：
// 桥接端合成速度通常快于播放，0.5s 已足够吸收网络抖动，
// 也符合工作区 realtime 切片（22KB）的容量约束。
#define RING_SAMPLES   (APP_AUDIO_SAMPLE_RATE / 2)
#define CHUNK_SAMPLES  480      // 30 ms @16 kHz：够小以保证 barge-in 及时
#define TASK_STACK     4096
#define TRAILING_SILENCE_MS 60

typedef struct {
    int16_t *ring;
    size_t head;
    size_t tail;
    size_t count;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    uint16_t current_id;
    bool playing;
    bool end_seen;          // 已收到 last 帧
    uint8_t volume;
    bool ready;
    void (*finished)(uint16_t speak_id, bool completed);
    void *finished_user;
} audio_state_t;

static audio_state_t s_audio;
// 溢出日志限流：桥接端异常倾倒时每秒能产生上百条，UART 打印本身会
// 拖垮系统，这里最多 5 秒汇总一条。
static int64_t s_drop_log_ms;
static uint32_t s_drop_pending;
static volatile bool s_shutdown;   // 退出标志：音频任务自旋检查后自行删除

static void notify_task(void)
{
    if (s_audio.task) xTaskNotifyGive(s_audio.task);
}

static size_t ring_pop(int16_t *dst, size_t max_samples)
{
    size_t taken = 0;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    while (taken < max_samples && s_audio.count > 0) {
        dst[taken++] = s_audio.ring[s_audio.tail];
        s_audio.tail = (s_audio.tail + 1) % RING_SAMPLES;
        s_audio.count--;
    }
    xSemaphoreGive(s_audio.mutex);
    return taken;
}

static void deliver_finished(uint16_t speak_id, bool completed)
{
    if (s_audio.finished) s_audio.finished(speak_id, completed);
}

static void audio_task(void *arg)
{
    (void)arg;
    // 分块缓冲也放在工作区 aux 切片，不占堆。
    int16_t *chunk = (int16_t *)audio_workspace::live::chunk();
    if (chunk == NULL) {
        ESP_LOGE(TAG, "音频分块缓冲不可用");
        s_audio.task = NULL;
        s_audio.ready = false;
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        if (s_shutdown) break;

        for (;;) {
            const size_t got = ring_pop(chunk, CHUNK_SAMPLES);
            if (got > 0) {
                if (bsp_audio_write(chunk, got * sizeof(int16_t)) != ESP_OK) {
                    ESP_LOGW(TAG, "写入 I2S 失败，结束当前播报");
                    uint16_t id = s_audio.current_id;
                    s_audio.playing = false;
                    deliver_finished(id, false);
                    break;
                }
                if (got == CHUNK_SAMPLES) continue;
            }

            // 缓冲见底：已经收到 last 帧就补一小段静音再收尾，避免尾音被截断。
            if (s_audio.end_seen) {
                static const int16_t silence[TRAILING_SILENCE_MS * APP_AUDIO_SAMPLE_RATE / 1000] = {0};
                (void)bsp_audio_write(silence, sizeof(silence));
                const uint16_t id = s_audio.current_id;
                s_audio.playing = false;
                s_audio.end_seen = false;
                ESP_LOGD(TAG, "播报完成 id=%u", (unsigned)id);
                deliver_finished(id, true);
            }
            break;
        }
    }

    s_audio.task = NULL;
    vTaskDelete(NULL);
}

void app_audio_deinit(void)
{
    if (!s_audio.ready) return;
    s_shutdown = true;
    if (s_audio.task) xTaskNotifyGive(s_audio.task);
    for (int i = 0; i < 20 && s_audio.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_audio.task) {          // 兑底：极端情况下强制回收
        vTaskDelete(s_audio.task);
        s_audio.task = NULL;
    }
    s_shutdown = false;

    bsp_audio_deinit();
    (void)audio_workspace::release(audio_workspace::Owner::Live);
    if (s_audio.mutex) {
        vSemaphoreDelete(s_audio.mutex);
        s_audio.mutex = NULL;
    }
    s_audio.ring = NULL;
    s_audio.ready = false;
    s_audio.playing = false;
    s_audio.count = s_audio.head = s_audio.tail = 0;
    s_audio.current_id = 0;
    ESP_LOGI(TAG, "音频已释放");
}

esp_err_t app_audio_init(uint8_t volume)
{
    if (s_audio.ready) {
        app_audio_set_volume(volume);
        return ESP_OK;
    }

    s_audio.mutex = xSemaphoreCreateMutex();
    if (s_audio.mutex == NULL) return ESP_ERR_NO_MEM;

    // 环形缓冲与分块缓冲都放在静态音频工作区（认领 Owner::Live），
    // 不再从堆上 malloc —— passport-os 的堆比原项目紧张得多。
    if (!audio_workspace::acquire(audio_workspace::Owner::Live)) {
        ESP_LOGE(TAG, "音频工作区被其他应用占用");
        vSemaphoreDelete(s_audio.mutex);
        s_audio.mutex = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_audio.ring = (int16_t *)audio_workspace::storage;

    s_audio.volume = volume > 100 ? 100 : volume;
    esp_err_t err = bsp_audio_init_playback();
    if (err == ESP_OK) err = bsp_audio_set_format(APP_AUDIO_SAMPLE_RATE, APP_AUDIO_BITS, APP_AUDIO_CHANNELS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec 初始化失败: %s", esp_err_to_name(err));
        s_audio.ring = NULL;
        (void)audio_workspace::release(audio_workspace::Owner::Live);
        vSemaphoreDelete(s_audio.mutex);
        s_audio.mutex = NULL;
        return err;
    }

    bsp_audio_set_volume(s_audio.volume);

    if (xTaskCreate(audio_task, "live_audio", TASK_STACK, NULL, 6, &s_audio.task) != pdPASS) {
        s_audio.ring = NULL;
        (void)audio_workspace::release(audio_workspace::Owner::Live);
        vSemaphoreDelete(s_audio.mutex);
        s_audio.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_audio.ready = true;
    s_shutdown = false;
    ESP_LOGI(TAG, "音频就绪: %d Hz / %d bit / 单声道, 音量 %u%%",
             APP_AUDIO_SAMPLE_RATE, APP_AUDIO_BITS, s_audio.volume);
    return ESP_OK;
}

void app_audio_set_volume(uint8_t volume)
{
    s_audio.volume = volume > 100 ? 100 : volume;
    if (s_audio.ready) bsp_audio_set_volume(s_audio.volume);
}

uint8_t app_audio_volume(void)
{
    return s_audio.volume;
}

void app_audio_set_finished_callback(void (*callback)(uint16_t, bool), void *user)
{
    s_audio.finished = callback;
    s_audio.finished_user = user;
}

bool app_audio_begin(uint16_t speak_id)
{
    if (!s_audio.ready) return false;

    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    s_audio.head = 0;
    s_audio.tail = 0;
    s_audio.count = 0;
    s_audio.current_id = speak_id;
    s_audio.playing = true;
    s_audio.end_seen = false;
    xSemaphoreGive(s_audio.mutex);
    return true;
}

bool app_audio_write(uint16_t speak_id, const int16_t *pcm, size_t samples, bool last)
{
    if (!s_audio.ready || pcm == NULL || samples == 0) return false;

    bool accepted = false;
    bool overflowed = false;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    if (s_audio.playing && s_audio.current_id == speak_id) {
        for (size_t i = 0; i < samples; i++) {
            if (s_audio.count >= RING_SAMPLES) {
                // 缓冲满：说明桥接端明显快于播放。丢弃多余数据并汇总记录。
                s_drop_pending += (uint32_t)(samples - i);
                overflowed = true;
                break;
            }
            s_audio.ring[s_audio.head] = pcm[i];
            s_audio.head = (s_audio.head + 1) % RING_SAMPLES;
            s_audio.count++;
        }
        if (last) s_audio.end_seen = true;
        accepted = true;
    }
    xSemaphoreGive(s_audio.mutex);

    if (overflowed) {
        const int64_t now = esp_timer_get_time() / 1000;
        if (now - s_drop_log_ms > 5000) {
            ESP_LOGW(TAG, "音频缓冲溢出：5 秒累计丢弃 %u 采样", s_drop_pending);
            s_drop_pending = 0;
            s_drop_log_ms = now;
        }
    }

    if (accepted) notify_task();
    return accepted;
}

void app_audio_stop(void)
{
    if (!s_audio.ready) return;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    s_audio.count = 0;
    s_audio.head = 0;
    s_audio.tail = 0;
    s_audio.playing = false;
    s_audio.end_seen = false;
    xSemaphoreGive(s_audio.mutex);
}

bool app_audio_playing(void)
{
    return s_audio.playing;
}

uint16_t app_audio_current_id(void)
{
    return s_audio.current_id;
}
