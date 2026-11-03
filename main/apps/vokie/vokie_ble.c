// apps/vokie/vokie_ble.c —— Vokie BLE 麦克风外设(passport-os 移植版)。
// 协议与帧格式移植自 vokie-ai-passport-firmware (alanshen6666-collab, MIT)。
// 移植差异:按键由应用外壳转发(不再自注册回调);UI 走 apps/vokie/ui_status;
// NimBLE host 常驻,stop 仅断连/停广播/挂起音频任务。
#include "vokie_ble.h"

#include "bsp_audio.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ui_status.h"
#include "wbconsole/wb_gatt.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "vokie_ble";
static const char *DEVICE_NAME = "Vokie Passport";
static const uint16_t NO_CONN = BLE_HS_CONN_HANDLE_NONE;

#define AUDIO_SAMPLES 320           // 20 ms @ 16 kHz
#define ADPCM_BYTES 166
#define MAX_CONTROL 182
#define EMPTY_FINAL_SEQUENCE 0xffffffffUL
#define AUDIO_TX_TIMEOUT_MS 80
#define CONTROL_TX_TIMEOUT_MS 200
#define TX_RETRY_INTERVAL_MS 5
#define SERVICE_BYTES 0x00,0x45,0x49,0x4b,0x4f,0x56,0x1a,0x9d,0x6f,0x4b,0x7b,0x6a,0x01,0x00,0x0e,0x7f
#define CONTROL_BYTES 0x00,0x45,0x49,0x4b,0x4f,0x56,0x1a,0x9d,0x6f,0x4b,0x7b,0x6a,0x02,0x00,0x0e,0x7f
#define AUDIO_BYTES 0x00,0x45,0x49,0x4b,0x4f,0x56,0x1a,0x9d,0x6f,0x4b,0x7b,0x6a,0x03,0x00,0x0e,0x7f
#define INFO_BYTES 0x00,0x45,0x49,0x4b,0x4f,0x56,0x1a,0x9d,0x6f,0x4b,0x7b,0x6a,0x04,0x00,0x0e,0x7f

static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(SERVICE_BYTES);
static const ble_uuid128_t s_control_uuid = BLE_UUID128_INIT(CONTROL_BYTES);
static const ble_uuid128_t s_audio_uuid = BLE_UUID128_INIT(AUDIO_BYTES);
static const ble_uuid128_t s_info_uuid = BLE_UUID128_INIT(INFO_BYTES);
static uint16_t s_control_handle;
static uint16_t s_audio_handle;
static uint16_t s_info_handle;
static uint16_t s_conn = NO_CONN;
static uint8_t s_addr_type;
static volatile bool s_control_subscribed;
static volatile bool s_audio_subscribed;
static volatile bool s_host_ready;
static volatile bool s_recording;
static volatile bool s_stop_requested;
static volatile uint32_t s_session;
static volatile uint32_t s_control_seq;
static volatile uint32_t s_button_seq;
static TaskHandle_t s_audio_task;
static bool s_started;
static bool s_host_running;
static esp_timer_handle_t s_ok_hold_timer;
static volatile uint32_t s_connection_epoch = 1;
static bool s_hello_sent;
static bool s_link_parameters_requested;
static uint32_t s_tx_retries;
static int32_t s_ok_hold_ms = 500;
static volatile bool s_app_active = true;   // app 前台态(广播权守卫)

// 共享的 NimBLE host 引导标志(见 wb_gatt.h):Vokie 与 WB 控制台共用 host。
bool ble_host_shared_booted = false;

static bool host_is_ready(void)
{
    return s_conn != NO_CONN && s_host_ready;
}

static int notify_control(const char *json);

static const char *button_name(bsp_btn_t button)
{
    switch (button) {
    case BSP_BTN_DOWN: return "down";
    case BSP_BTN_OK: return "ok";
    default: return "up";
    }
}

static void send_button_event(bsp_btn_t button, const char *event,
                              uint32_t duration_ms)
{
    if (s_conn == NO_CONN || !s_host_ready) return;
    char json[MAX_CONTROL + 1];
    snprintf(json, sizeof(json),
             "{\"v\":1,\"type\":\"button_event\",\"button\":\"%s\",\"event\":\"%s\",\"durationMs\":%lu,\"seq\":%lu}",
             button_name(button), event, (unsigned long)duration_ms,
             (unsigned long)++s_button_seq);
    (void)notify_control(json);
}

static void ok_hold_expired(void *arg)
{
    (void)arg;
    // 该定时器与按键驱动回调同在 ESP_TIMER_TASK 上执行。
    vokie_ble_on_key(BSP_BTN_OK, BSP_BTN_LONG, 0);
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gap_event(struct ble_gap_event *event, void *arg);

static int notify_bytes(uint16_t handle, const uint8_t *data, size_t len)
{
    if (s_conn == NO_CONN) return BLE_HS_ENOTCONN;
    if (len > ble_att_mtu(s_conn) - 3) return BLE_HS_EMSGSIZE;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return BLE_HS_ENOMEM;
    return ble_gatts_notify_custom(s_conn, handle, om);
}

static int notify_control(const char *json)
{
    size_t len = strlen(json);
    if (len == 0 || len > MAX_CONTROL) return BLE_HS_EMSGSIZE;
    return notify_bytes(s_control_handle, (const uint8_t *)json, len);
}

// --- 有界重试通知:ENOMEM/EAGAIN/EBUSY 每 5ms 重试,直到超时或会话失效。
typedef struct {
    uint16_t conn;
    uint16_t handle;
    uint32_t epoch;
    uint32_t session;
    const uint8_t *data;
    size_t len;
    bool require_recording;
} tx_context_t;

static bool tx_valid(const tx_context_t *tx)
{
    return tx->conn != NO_CONN && tx->conn == s_conn &&
           tx->epoch == s_connection_epoch && tx->session == s_session &&
           (!tx->require_recording || s_recording) &&
           (tx->handle == s_audio_handle ? s_audio_subscribed : s_control_subscribed);
}

static bool tx_retryable(int rc)
{
    return rc == BLE_HS_ENOMEM || rc == BLE_HS_EAGAIN || rc == BLE_HS_EBUSY;
}

static uint32_t tx_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int notify_worker(uint16_t handle, const uint8_t *data, size_t len,
                         uint32_t session, uint32_t epoch, bool require_recording,
                         uint32_t timeout_ms)
{
    tx_context_t tx = {s_conn, handle, epoch, session, data, len, require_recording};
    const uint32_t start = tx_now_ms();
    uint32_t attempts = 0;
    int rc;
    do {
        ++attempts;
        rc = BLE_HS_ENOTCONN;
        if (tx_valid(&tx)) {
            if (tx.len > ble_att_mtu(tx.conn) - 3) return BLE_HS_EMSGSIZE;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(tx.data, tx.len);
            rc = om ? ble_gatts_notify_custom(tx.conn, tx.handle, om) : BLE_HS_ENOMEM;
        }
        if (rc == 0) break;
        if (!tx_retryable(rc)) break;
        if (tx_now_ms() - start >= timeout_ms) break;
        vTaskDelay(pdMS_TO_TICKS(TX_RETRY_INTERVAL_MS) ? pdMS_TO_TICKS(TX_RETRY_INTERVAL_MS) : 1);
    } while (tx_valid(&tx));
    if (attempts > 1) s_tx_retries += attempts - 1;
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGE(TAG, "TX 失败 session=%lu handle=%u rc=%d attempts=%lu mtu=%u heap=%lu",
                 (unsigned long)session, handle, rc, (unsigned long)attempts,
                 ble_att_mtu(tx.conn), (unsigned long)esp_get_free_heap_size());
    }
    return rc;
}

static int notify_control_worker(const char *json, uint32_t session, uint32_t epoch,
                                 bool require_recording)
{
    const size_t len = strlen(json);
    if (!len || len > MAX_CONTROL) return BLE_HS_EMSGSIZE;
    return notify_worker(s_control_handle, (const uint8_t *)json, len, session, epoch,
                         require_recording, CONTROL_TX_TIMEOUT_MS);
}

static void log_link(const char *event)
{
    struct ble_gap_conn_desc desc;
    if (s_conn != NO_CONN && ble_gap_conn_find(s_conn, &desc) == 0) {
        ESP_LOGI(TAG, "%s conn=%u mtu=%u interval_us=%lu latency=%u supervision_ms=%lu",
                 event, s_conn, ble_att_mtu(s_conn), (unsigned long)desc.conn_itvl * 1250,
                 desc.conn_latency, (unsigned long)desc.supervision_timeout * 10);
    }
}

static void request_audio_link(void)
{
    if (s_link_parameters_requested || s_conn == NO_CONN) return;
    s_link_parameters_requested = true;
    const struct ble_gap_upd_params params = {
        .itvl_min = 12, .itvl_max = 24, .latency = 0,
        .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(s_conn, &params);
    ESP_LOGI(TAG, "音频链路参数请求 rc=%d 目标间隔=15..30ms", rc);
    rc = ble_gap_set_data_len(s_conn, 251, 2120);
    ESP_LOGI(TAG, "链路数据长度请求 rc=%d tx_octets=251", rc);
}

static void send_error(uint32_t session, const char *message)
{
    char json[MAX_CONTROL + 1];
    snprintf(json, sizeof(json), "{\"v\":1,\"type\":\"device_error\",\"sessionId\":%lu,\"message\":\"%s\"}",
             (unsigned long)session, message);
    (void)notify_control(json);
}

// --- IMA-ADPCM 编码(与 Vokie 主机端约定一致:4 bit/样本,首帧头 6 字节) ---
static int ima_index_table[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static int ima_step_table[89] = {
 7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};

static size_t encode_adpcm(const int16_t *pcm, uint8_t out[ADPCM_BYTES])
{
    int predictor = pcm[0];
    int index = 0;
    out[0] = (uint8_t)predictor; out[1] = (uint8_t)(predictor >> 8);
    out[2] = 0; out[3] = AUDIO_SAMPLES & 0xff; out[4] = AUDIO_SAMPLES >> 8; out[5] = 0;
    int nibble = 0;
    for (int i = 1; i < AUDIO_SAMPLES; ++i) {
        int diff = pcm[i] - predictor;
        int sign = diff < 0 ? 8 : 0;
        if (diff < 0) diff = -diff;
        int step = ima_step_table[index], delta = 0, vpdiff = step >> 3;
        if (diff >= step) { delta |= 4; diff -= step; vpdiff += step; }
        if (diff >= (step >> 1)) { delta |= 2; diff -= step >> 1; vpdiff += step >> 1; }
        if (diff >= (step >> 2)) { delta |= 1; vpdiff += step >> 2; }
        int code = delta | sign;
        predictor += sign ? -vpdiff : vpdiff;
        if (predictor > 32767) predictor = 32767;
        if (predictor < -32768) predictor = -32768;
        index += ima_index_table[code];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        if (nibble == 0) { out[6 + (i - 1) / 2] = (uint8_t)code; nibble = 1; }
        else { out[6 + (i - 1) / 2] |= (uint8_t)(code << 4); nibble = 0; }
    }
    return ADPCM_BYTES;
}

static int notify_audio(uint32_t session, uint32_t epoch, uint32_t sequence,
                        const uint8_t *payload, size_t len)
{
    if (s_conn == NO_CONN || epoch != s_connection_epoch || session != s_session) return BLE_HS_ENOTCONN;
    const size_t mtu_payload = ble_att_mtu(s_conn) - 3;
    if (mtu_payload <= 16) return BLE_HS_EMSGSIZE;
    const size_t chunk = mtu_payload - 16;
    uint8_t packet[16 + ADPCM_BYTES];
    uint8_t count = (uint8_t)((len + chunk - 1) / chunk);
    if (count == 0 || count > 64) return BLE_HS_EMSGSIZE;
    for (uint8_t part = 0; part < count; ++part) {
        size_t offset = part * chunk;
        size_t amount = len - offset < chunk ? len - offset : chunk;
        packet[0] = 0x41; packet[1] = 0x50; packet[2] = 1; packet[3] = 0;
        memcpy(packet + 4, &session, 4); memcpy(packet + 8, &sequence, 4);
        packet[12] = part; packet[13] = count; packet[14] = amount & 0xff; packet[15] = amount >> 8;
        memcpy(packet + 16, payload + offset, amount);
        int rc = notify_worker(s_audio_handle, packet, 16 + amount, session, epoch,
                               true, AUDIO_TX_TIMEOUT_MS);
        if (rc != 0) return rc;
    }
    return 0;
}

static void audio_task(void *arg)
{
    (void)arg;
    esp_err_t err = bsp_audio_set_format(16000, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置 16k 采样失败: %s;录音请求时重试", esp_err_to_name(err));
    }
    int16_t pcm[AUDIO_SAMPLES];
    uint8_t encoded[ADPCM_BYTES];
    uint32_t sequence = 0;
    uint32_t capture_session = 0;
    uint32_t capture_epoch = 0;
    uint32_t stats_ms = 0;
    bool capturing = false;
    for (;;) {
        if (!s_recording) {
            capturing = false;
            sequence = 0;
            // 空闲期挂起;app 停止时也停在这里,不占用 CPU。
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!capturing || capture_session != s_session || capture_epoch != s_connection_epoch) {
            capture_session = s_session;
            capture_epoch = s_connection_epoch;
            sequence = 0;
            s_tx_retries = 0;
            char start_json[MAX_CONTROL + 1];
            snprintf(start_json, sizeof(start_json),
                     "{\"v\":1,\"type\":\"ptt_down\",\"sessionId\":%lu,\"seq\":0}",
                     (unsigned long)capture_session);
            int start_rc = notify_control_worker(start_json, capture_session, capture_epoch, true);
            if (start_rc != 0) {
                if (s_recording && capture_session == s_session && capture_epoch == s_connection_epoch) {
                    s_recording = false;
                    s_stop_requested = false;
                    ui_status_set_state(UI_STATUS_ERROR, "BLE 发送失败");
                }
                continue;
            }
            stats_ms = tx_now_ms();
            ESP_LOGI(TAG, "开始采集 session=%lu epoch=%lu", (unsigned long)capture_session,
                     (unsigned long)capture_epoch);
            log_link("采集链路");
            if (bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
                if (s_recording && capture_session == s_session && capture_epoch == s_connection_epoch) {
                    s_recording = false;
                    s_stop_requested = false;
                    send_error(capture_session, "audio_open");
                    ui_status_set_state(UI_STATUS_ERROR, "麦克风不可用");
                }
                continue;
            }
            capturing = true;
        }
        if (!s_recording || capture_session != s_session || capture_epoch != s_connection_epoch) continue;
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            if (s_recording && capture_session == s_session && capture_epoch == s_connection_epoch) {
                s_stop_requested = false;
                send_error(capture_session, "audio_read");
                s_recording = false;
                ui_status_set_state(UI_STATUS_ERROR, "麦克风不可用");
            }
            continue;
        }
        // 阻塞 I/O 期间发生断连或新一轮请求,当前帧作废。
        if (!s_recording || capture_session != s_session || capture_epoch != s_connection_epoch) continue;
        if (!s_audio_subscribed || s_conn == NO_CONN || ble_att_mtu(s_conn) < 185) {
            s_stop_requested = false; send_error(s_session, "mtu");
            s_recording = false;
            ui_status_set_state(UI_STATUS_ERROR, "BLE 音频不可用");
            continue;
        }
        size_t encoded_len = encode_adpcm(pcm, encoded);
        int tx_rc = notify_audio(capture_session, capture_epoch, sequence, encoded, encoded_len);
        if (capture_session != s_session || capture_epoch != s_connection_epoch || !s_recording) continue;
        if (tx_rc != 0) {
            s_stop_requested = true;
            ESP_LOGE(TAG, "采集传输失败 session=%lu next_sequence=%lu rc=%d retries=%lu",
                     (unsigned long)capture_session, (unsigned long)sequence, tx_rc,
                     (unsigned long)s_tx_retries);
            send_error(capture_session, "transport");
            s_recording = false;
            ui_status_set_state(UI_STATUS_ERROR, "BLE 发送失败");
        } else {
            ++sequence;
        }
        if (tx_now_ms() - stats_ms >= 1000) {
            ESP_LOGI(TAG, "采集统计 session=%lu queued_frames=%lu retries=%lu",
                     (unsigned long)capture_session, (unsigned long)sequence, (unsigned long)s_tx_retries);
            stats_ms = tx_now_ms();
        }
        if (s_stop_requested) {
            char json[MAX_CONTROL + 1];
            snprintf(json, sizeof(json), "{\"v\":1,\"type\":\"ptt_up\",\"sessionId\":%lu,\"seq\":%lu,\"finalSequence\":%lu}",
                     (unsigned long)capture_session, (unsigned long)++s_control_seq,
                     sequence ? (unsigned long)(sequence - 1) : EMPTY_FINAL_SEQUENCE);
            int stop_rc = notify_control_worker(json, capture_session, capture_epoch, false);
            ESP_LOGI(TAG, "采集结束 session=%lu queued_frames=%lu retries=%lu control_rc=%d",
                     (unsigned long)capture_session, (unsigned long)sequence,
                     (unsigned long)s_tx_retries, stop_rc);
            if (capture_session != s_session || capture_epoch != s_connection_epoch) continue;
            if (stop_rc != 0) {
                send_error(capture_session, "transport");
                ui_status_set_state(UI_STATUS_ERROR, "BLE 发送失败");
            }
            s_recording = false; s_stop_requested = false; sequence = 0;
            ui_status_set_state(UI_STATUS_PROCESSING, "Recognizing...");
        }
    }
}

// 按键语义(参考实现):UP 按住说话/松开结束;DOWN 发送;OK 撤销(录音中=取消)。
void vokie_ble_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, uint16_t mv)
{
    (void)mv;
    if (btn < BSP_BTN_UP || btn > BSP_BTN_OK) return;
    if (ev != BSP_BTN_PRESS && ev != BSP_BTN_RELEASE && ev != BSP_BTN_LONG) return;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t duration_ms = now_ms % 1000000; // 展示用时长
    if (btn == BSP_BTN_OK && ev == BSP_BTN_PRESS) {
        (void)esp_timer_stop(s_ok_hold_timer);
        esp_err_t err = esp_timer_start_once(s_ok_hold_timer, (uint64_t)s_ok_hold_ms * 1000);
        if (err != ESP_OK) ESP_LOGW(TAG, "OK 长按定时器: %s", esp_err_to_name(err));
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_RELEASE) {
        (void)esp_timer_stop(s_ok_hold_timer);
    }
    if (ev == BSP_BTN_PRESS) {
        switch (btn) {
        case BSP_BTN_UP: ui_status_touch(UI_STATUS_HINT_VOICE); break;
        case BSP_BTN_DOWN: ui_status_touch(UI_STATUS_HINT_SEND); break;
        case BSP_BTN_OK: ui_status_touch(UI_STATUS_HINT_UNDO); break;
        default: break;
        }
    }
    if (btn == BSP_BTN_UP && ev == BSP_BTN_RELEASE) {
        ui_status_touch(UI_STATUS_HINT_VOICE);
        if (s_conn == NO_CONN || !s_host_ready) return;
        if (!s_recording) {
            s_session = esp_random();
            if (s_session == 0) s_session = 1;
            s_control_seq = 0;
            s_stop_requested = false;
            s_recording = true;
            ui_status_set_state(UI_STATUS_RECORDING, "Listening...");
            if (s_audio_task) xTaskNotifyGive(s_audio_task);
        } else {
            s_stop_requested = true;
            ui_status_set_state(UI_STATUS_PROCESSING, "Recognizing...");
        }
        return;
    }
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_RELEASE) {
        ui_status_touch(UI_STATUS_HINT_SEND);
        if (!host_is_ready()) return;
        send_button_event(btn, "click", 0);
        return;
    }
    if (btn == BSP_BTN_OK) {
        ui_status_touch(UI_STATUS_HINT_UNDO);
        if (!host_is_ready()) return;
        if (ev == BSP_BTN_LONG) {
            if (s_recording) {
                s_stop_requested = true;
                send_button_event(btn, "long", duration_ms);
                ui_status_set_state(UI_STATUS_PROCESSING, "Cancelling...");
            } else {
                send_button_event(btn, "long", duration_ms);
                ui_status_set_state(UI_STATUS_READY, "Input cleared");
            }
        } else if (ev == BSP_BTN_RELEASE) {
            if (s_recording) {
                s_stop_requested = true;
                send_button_event(btn, "click", duration_ms);
                ui_status_set_state(UI_STATUS_PROCESSING, "Cancelling...");
            } else {
                send_button_event(btn, "click", duration_ms);
                ui_status_set_state(UI_STATUS_READY, "Backspace");
            }
        }
    }
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    int kind = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && kind == 3) {
        const char info[] = "{\"v\":1,\"type\":\"hello\",\"device\":\"ai-passport\",\"fw\":\"1.0.0\",\"codec\":\"ima-adpcm\",\"sampleRate\":16000,\"channels\":1,\"frameMs\":20}";
        return os_mbuf_append(ctxt->om, info, strlen(info));
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || kind != 1) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    char value[MAX_CONTROL + 1] = {0}; uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, value, MAX_CONTROL, &len) != 0 || len == 0 || len > MAX_CONTROL) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    cJSON *root = cJSON_ParseWithLength(value, len);
    if (!root || !cJSON_IsObject(root) || !cJSON_IsNumber(cJSON_GetObjectItem(root, "v")) || cJSON_GetObjectItem(root, "v")->valueint != 1) {
        cJSON_Delete(root);
        return BLE_ATT_ERR_UNLIKELY;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "host_ready") == 0) {
        if (conn != s_conn || !s_control_subscribed || !s_audio_subscribed || ble_att_mtu(conn) < 185) {
            ESP_LOGW(TAG, "host_ready 拒绝 conn=%u control=%u audio=%u mtu=%u",
                     conn, s_control_subscribed, s_audio_subscribed, ble_att_mtu(conn));
            cJSON_Delete(root);
            return BLE_ATT_ERR_UNLIKELY;
        }
        s_host_ready = true;
        ESP_LOGI(TAG, "主机就绪 epoch=%lu", (unsigned long)s_connection_epoch);
        log_link("主机链路");
        request_audio_link();
        ui_status_set_state(UI_STATUS_READY, "Vokie is ready");
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "host_state") == 0) {
        const cJSON *state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state)) {
            if (strcmp(state->valuestring, "ready") == 0) {
                s_host_ready = true;
                ui_status_set_state(UI_STATUS_READY, "Vokie is ready");
            } else if (strcmp(state->valuestring, "recording") == 0) {
                ui_status_set_state(UI_STATUS_RECORDING, "Listening...");
            } else if (strcmp(state->valuestring, "processing") == 0) {
                ui_status_set_state(UI_STATUS_PROCESSING, "Recognizing...");
            } else if (strcmp(state->valuestring, "success") == 0) {
                ui_status_set_state(UI_STATUS_SUCCESS, "Sent");
            } else if (strcmp(state->valuestring, "error") == 0) {
                ESP_LOGW(TAG, "主机报错 session=%lu recording=%u;取消采集",
                         (unsigned long)s_session, s_recording);
                s_recording = false;
                s_stop_requested = false;
                ui_status_set_state(UI_STATUS_ERROR, "Retry");
            }
        }
    }
    cJSON_Delete(root); (void)conn; return 0;
}

static const struct ble_gatt_svc_def services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_service_uuid.u, .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = &s_control_uuid.u, .access_cb = access_cb, .arg = (void *)(intptr_t)1, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_control_handle },
        { .uuid = &s_audio_uuid.u, .access_cb = access_cb, .arg = (void *)(intptr_t)2, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_audio_handle },
        { .uuid = &s_info_uuid.u, .access_cb = access_cb, .arg = (void *)(intptr_t)3, .flags = BLE_GATT_CHR_F_READ, .val_handle = &s_info_handle },
        {0} } }, {0}
};

// 供 WB 控制台的 host 引导路径统一注册(服务表只能在 host 启动前注册)。
const struct ble_gatt_svc_def *vokie_gatt_services(void) { return services; }

static int advertise(void)
{
    // 广播权守卫:本 app 退出后(WB 控制台接管 GAP)不再抢占。
    if (!s_app_active) return 0;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_hs_adv_fields response = {0};
    response.name = (const uint8_t *)DEVICE_NAME;
    response.name_len = strlen(DEVICE_NAME);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_addr_type) == 0) {
        (void)advertise();
    }
}
static void on_reset(int reason) { ESP_LOGE(TAG, "NimBLE reset %d", reason); }
static void host_task(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {
            ++s_connection_epoch;
            s_conn = event->connect.conn_handle;
            s_host_ready = false; s_control_subscribed = false; s_audio_subscribed = false;
            s_hello_sent = false; s_link_parameters_requested = false;
            log_link("已连接");
            ui_status_set_state(UI_STATUS_STARTING, "Connected, waiting host...");
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d", event->connect.status);
            ui_status_set_state(UI_STATUS_DISCONNECTED, "Waiting for Vokie...");
            advertise();
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        ESP_LOGW(TAG, "断连 reason=%d session=%lu recording=%u", event->disconnect.reason,
                 (unsigned long)s_session, s_recording);
        ++s_connection_epoch;
        s_recording = false; s_stop_requested = false; s_host_ready = false;
        s_control_subscribed = false; s_audio_subscribed = false; s_conn = NO_CONN;
        s_hello_sent = false; s_link_parameters_requested = false;
        ui_status_set_state(UI_STATUS_DISCONNECTED, "Waiting for Vokie...");
        if (s_started) advertise();
    } else if (event->type == BLE_GAP_EVENT_SUBSCRIBE && event->subscribe.conn_handle == s_conn) {
        if (event->subscribe.attr_handle == s_control_handle) {
            s_control_subscribed = event->subscribe.cur_notify;
        }
        if (event->subscribe.attr_handle == s_audio_handle) {
            s_audio_subscribed = event->subscribe.cur_notify;
        }
        ESP_LOGI(TAG, "订阅 handle=%u notify=%u control=%u audio=%u", event->subscribe.attr_handle,
                 event->subscribe.cur_notify, s_control_subscribed, s_audio_subscribed);
        if (!s_control_subscribed || !s_audio_subscribed) {
            s_hello_sent = false;
            if (s_recording) ESP_LOGW(TAG, "采集期间订阅被移除 session=%lu", (unsigned long)s_session);
        } else if (!s_hello_sent) {
            const char hello[] = "{\"v\":1,\"type\":\"hello\",\"device\":\"ai-passport\",\"fw\":\"1.0.0\",\"codec\":\"ima-adpcm\",\"sampleRate\":16000,\"channels\":1,\"frameMs\":20}";
            s_hello_sent = notify_control(hello) == 0;
        }
    } else if (event->type == BLE_GAP_EVENT_MTU) {
        ESP_LOGI(TAG, "MTU 更新 conn=%u mtu=%u", event->mtu.conn_handle, event->mtu.value);
    } else if (event->type == BLE_GAP_EVENT_CONN_UPDATE) {
        log_link("链路更新");
    } else if (event->type == BLE_GAP_EVENT_DATA_LEN_CHG) {
        ESP_LOGI(TAG, "数据长度更新 conn=%u tx_octets=%u rx_octets=%u",
                 event->data_len_chg.conn_handle, event->data_len_chg.max_tx_octets,
                 event->data_len_chg.max_rx_octets);
    }
    return 0;
}

esp_err_t vokie_ble_start(void)
{
    if (s_started) {
        // 已常驻:恢复广播与 UI 状态即可。
        s_app_active = true;
        ui_status_set_state(UI_STATUS_DISCONNECTED, "Waiting for Vokie...");
        if (s_conn == NO_CONN) advertise();
        return ESP_OK;
    }
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    ESP_LOGI(TAG, "BLE 音频传输:有界 TX 重试 + 链路调优");
    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) return err;
    if (!s_ok_hold_timer) {
        const esp_timer_create_args_t timer = {
            .callback = ok_hold_expired, .name = "vokie_ok_hold",
        };
        err = esp_timer_create(&timer, &s_ok_hold_timer);
        if (err != ESP_OK) return err;
    }
    if (!s_audio_task &&
        xTaskCreate(audio_task, "vokie_audio", 4096, NULL, 5, &s_audio_task) != pdPASS)
        return ESP_ERR_NO_MEM;

    // host 已被 WB 控制台引导过:NimBLE 不允许二次 init,只接管广播。
    if (ble_host_shared_booted) {
        s_started = true; s_app_active = true;
        ble_svc_gap_device_name_set(DEVICE_NAME);
        if (s_conn == NO_CONN) advertise();
        ESP_LOGI(TAG, "复用已引导的 NimBLE host,Vokie 接管广播");
        return ESP_OK;
    }

    int rc = nimble_port_init(); if (rc != ESP_OK) return ESP_FAIL;
    ble_svc_gap_init(); ble_svc_gatt_init(); ble_svc_gap_device_name_set(DEVICE_NAME);
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0) return ESP_FAIL;
    // 同时注册 WB 控制台服务表(两个 BLE 应用共用 host,后进的应用无法再注册)。
    if (ble_gatts_count_cfg(wb_gatt_services()) != 0 ||
        ble_gatts_add_svcs(wb_gatt_services()) != 0) return ESP_FAIL;
    ble_hs_cfg.reset_cb = on_reset; ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ble_host_shared_booted = true;
    s_started = true; s_app_active = true; ESP_LOGI(TAG, "Vokie Passport BLE 就绪"); return ESP_OK;
}

void vokie_ble_stop(void)
{
    if (!s_started) return;
    s_app_active = false;   // 让出广播权(WB 控制台可能接管 GAP)
    // 断连 + 停广播;NimBLE host 由 wb_ble_release_host() 统一释放。
    if (s_recording) { s_stop_requested = true; }
    if (s_conn != NO_CONN) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        // 等待断连完成(有界);gap_event 会停止采集并更新状态。
        for (int i = 0; i < 30 && s_conn != NO_CONN; ++i) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_conn != NO_CONN) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();
    ui_status_set_state(UI_STATUS_DISCONNECTED, "Stopped");
}

// 彻底释放 Vokie 名下的音频资源:采集任务 + codec/I2S DMA。
// 此前这些常驻(为了「重进秒级可用」),实测退出后 largest 连续块只剩
// ~34KB,小智接着画一个字形就 OOM panic——大块连续内存比秒进重要。
// 重进 Vokie 时 bsp_audio_init/任务会自动重建,代价约几百毫秒。
void vokie_ble_free_audio(void)
{
    if (s_audio_task) {
        s_recording = false;            // 让任务回到空闲挂起点
        vTaskDelay(pdMS_TO_TICKS(100)); // 有界等它脱离采集循环(连接已断,很快)
        vTaskDelete(s_audio_task);
        s_audio_task = NULL;
    }
    bsp_audio_deinit();
}

bool vokie_ble_ready(void) { return s_control_subscribed && s_audio_subscribed && s_host_ready; }
