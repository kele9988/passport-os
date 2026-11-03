// apps/wbconsole/wb_ble.c —— WorkBuddy 伴生控制台 BLE 链路(见 wb_ble.h)。
// 协议逆向自 play 513 PC 端 wb_app.py:service 4fafc201-1fb5-459e-8fcc-c5c9c331914b,
// 特征 beb5483e-36e1-4688-b7f5-ea07361b26a8(write+notify),广播名 FoloPassport。
// 设备接收缓冲 512B(超长帧 PC 会拒发);上行行可能超过单包,PC 按 '\n' 拼装。
#include "wb_ble.h"
#include "wb_gatt.h"
#include "wb_voice.h"
#include "wb_wifi_link.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
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
#include <string.h>

static const char *TAG = "wb_ble";
static const char *DEVICE_NAME = "FoloPassport";
static const uint16_t NO_CONN = BLE_HS_CONN_HANDLE_NONE;

// UUID 小端字节序(canonical 字符串的逆序)。
#define WB_SVC_BYTES  0x4b,0x91,0x33,0xc3,0xc9,0x5c,0xcc,0x8f,0x9e,0x45,0xb5,0x1f,0x01,0xc2,0xfa,0x4f
#define WB_CHR_BYTES  0xa8,0x26,0x1b,0x36,0x07,0xea,0xf5,0xb7,0x88,0x46,0xe1,0x36,0x3e,0x48,0xb5,0xbe

static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(WB_SVC_BYTES);
static const ble_uuid128_t s_char_uuid = BLE_UUID128_INIT(WB_CHR_BYTES);
static uint16_t s_char_handle;
static uint16_t s_conn = NO_CONN;
static uint8_t s_addr_type;
static volatile bool s_subscribed;
static volatile bool s_started;        // app 前台态(控制是否广播)
static bool s_host_booted;             // NimBLE host 引导完成(常驻)
static volatile bool s_app_active;     // 本 app 是否拥有 GAP(广播权)
static volatile bool s_keep_visible;   // 「常可见」:首页调用后,任何应用退出都继续广播

// ---- 内存锚(guard) ----
// BLE 控制器初始化要一整块 ~50-65KB 连续内存,不够时它内部直接 assert
// (Breakpoint panic 整机重启)。此前「释放蓝牙栈后重新引导」的路径在内存
// 被碎片化后必然踩雷。锚的思路:host 释放的瞬间立即把 64KB 大块锚在手里,
// 任何任务都拿不走 → 下次引导时交出锚,控制器必然拿得到连续块。
// 需要大块内存的应用(对讲机/小智)进场时调 wb_ble_guard_loan() 把锚借走;
// 借走后若碎片化导致引导失败,由后台重试任务接力,不再「宁可离线」。
#define BLE_GUARD_SIZE (64 * 1024)
static uint8_t *s_guard = NULL;
static TaskHandle_t s_retry_task = NULL;

static void guard_grab(void)
{
    if (s_guard) return;
    s_guard = (uint8_t *)heap_caps_malloc(BLE_GUARD_SIZE,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGW(TAG, "内存锚: %s (largest=%u)",
             s_guard ? "已持有 64KB" : "分配失败(内存全留给应用)",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void guard_free(const char *why)
{
    if (!s_guard) return;
    heap_caps_free(s_guard);
    s_guard = NULL;
    ESP_LOGW(TAG, "内存锚交出(%s): largest=%u", why,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static wb_ble_callbacks_t s_cb;

static void notify_link(bool connected);   // 定义在 GAP 段
static void rx_reset(void);                // 定义在 proto 段

// 上行拼装:生产者=NimBLE 主机任务(GATT 写回调,只做拷贝),
// 消费者=wb_proto 任务(JSON 解析 + 组包发送)。
// 必须分离:NimBLE 主机任务栈只有 CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE(4~6KB),
// 在里面直接 cJSON 解析/组 2KB 响应包会栈溢出 → panic 重启。
#define RX_CAP 4096
static uint8_t s_rx[RX_CAP];
static volatile size_t s_rx_head;
static volatile size_t s_rx_tail;
static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_proto_task;
static SemaphoreHandle_t s_tx_lock;
static char s_poll_resp[2048];   // 仅供 proto 任务使用(静态,不占任务栈)

// ---- TX:有界重试分片发送(移植 vokie 的 notify_worker 思路) ----

static bool tx_retryable(int rc)
{
    return rc == BLE_HS_ENOMEM || rc == BLE_HS_EAGAIN || rc == BLE_HS_EBUSY;
}

static uint32_t tx_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int notify_chunk(const uint8_t *data, size_t len)
{
    if (s_conn == NO_CONN || !s_subscribed) return BLE_HS_ENOTCONN;
    const size_t mtu = ble_att_mtu(s_conn);
    if (mtu < 23 || len > mtu - 3) return BLE_HS_EMSGSIZE;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return BLE_HS_ENOMEM;
    return ble_gatts_notify_custom(s_conn, s_char_handle, om);
}

// 整行发送:按 MTU 分片,末尾补 '\n'。调用方可以是任意任务(有 tx 互斥)。
// 注意:重试里会 vTaskDelay,所以绝不能包在 taskENTER_CRITICAL 里。
int wb_ble_send_line(const char *data, size_t len)
{
    if (!data || len == 0 || len > 4096) return -1;
    // 路由:WiFi TCP 在线优先(延迟低、不分片、不占 BLE 空口);否则走 BLE notify。
    if (wb_wifi_link_connected()) return wb_wifi_link_send(data, len);
    if (s_conn == NO_CONN || !s_subscribed) return BLE_HS_ENOTCONN;
    if (s_tx_lock) xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(800));
    const uint32_t start = tx_now_ms();
    size_t sent = 0;
    int rc = 0;
    while (sent < len) {
        const size_t mtu_payload = ble_att_mtu(s_conn) - 3;
        size_t chunk = len - sent;
        bool tail = false;
        if (chunk + 1 > mtu_payload) {
            chunk = mtu_payload - 1;      // 留 1 字节给 '\n'
        } else {
            tail = true;
        }
        uint8_t buf[512];
        if (chunk > sizeof(buf) - 1) chunk = sizeof(buf) - 1;
        memcpy(buf, data + sent, chunk);
        if (tail) { buf[chunk] = '\n'; ++chunk; }
        uint32_t attempts = 0;
        do {
            ++attempts;
            rc = notify_chunk(buf, chunk);
            if (rc == 0) break;
            if (!tx_retryable(rc)) break;
            if (tx_now_ms() - start >= 400) break;
            vTaskDelay(pdMS_TO_TICKS(5));
            attempts = attempts;   // 有界重试,超时由上面的时间窗控制
        } while (s_conn != NO_CONN && s_subscribed);
        if (rc != 0) {
            ESP_LOGE(TAG, "TX 分片失败 offset=%u rc=%d", (unsigned)sent, rc);
            if (s_tx_lock) xSemaphoreGive(s_tx_lock);
            return rc;
        }
        sent += tail ? (chunk - 1) : chunk;
    }
    if (s_tx_lock) xSemaphoreGive(s_tx_lock);
    return 0;
}

uint16_t wb_ble_mtu(void)
{
    return s_conn != NO_CONN ? ble_att_mtu(s_conn) : 0;
}

bool wb_ble_connected(void)
{
    return wb_wifi_link_connected() || (s_conn != NO_CONN && s_subscribed);
}

// WiFi 通道连接/断开时由 wb_wifi_link 调用:沿用 BLE 的 on_link 回调链路,
// 应用 UI 的链路状态、语音会话开关无需区分底层通道。
void wb_ble_route_notify(bool up)
{
    if (up) rx_reset();          // 新连接:丢弃上一会话残留字节
    else wb_voice_end(4);        // 与 BLE 断连同款处理:终止进行中的录音
    notify_link(up);
}

// WiFi 通道收到的下行字节从这里注入,与 BLE GATT 写回调共用同一个
// 环形缓冲 + proto 任务 + 暂存队列(协议层完全不区分底层通道)。
static void proto_task(void *arg);   // 前置声明:inject_rx 需要随首帧懒创建它

void wb_ble_inject_rx(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return;
    // 2026-11-03 关键修复:wb_proto 上行解析任务原先只在 host_boot_once()
    // (NimBLE 引导成功)时创建。WiFi 通道启用后,BLE 可能长期引导不起来
    // (largest<56KB 锚线,重试日志实锤 100 次全失败)→ proto 任务不存在 →
    // TCP 收到的 poll/pcmon/积分/会话帧全部堆死在环形缓冲,语音与 HUD 全断。
    // 解析任务只依赖环形缓冲与 s_tx_lock,与蓝牙主机无关,随首帧懒创建。
    if (!s_proto_task) {
        if (!s_tx_lock) s_tx_lock = xSemaphoreCreateMutex();
        if (xTaskCreate(proto_task, "wb_proto", 8192, NULL, 5, &s_proto_task) != pdPASS) {
            static bool s_proto_warned = false;
            if (!s_proto_warned) {
                s_proto_warned = true;
                ESP_LOGE(TAG, "wb_proto 任务创建失败(内存不足),本帧丢弃");
            }
            return;
        }
        ESP_LOGW(TAG, "wb_proto 任务已随首帧懒创建(WiFi 通道无蓝牙也可用)");
    }
    taskENTER_CRITICAL(&s_rx_mux);
    for (size_t i = 0; i < len; ++i) {
        const size_t next = (s_rx_head + 1) % RX_CAP;
        if (next == s_rx_tail) s_rx_tail = (s_rx_tail + 1) % RX_CAP;
        s_rx[s_rx_head] = data[i];
        s_rx_head = next;
    }
    taskEXIT_CRITICAL(&s_rx_mux);
    if (s_proto_task) xTaskNotifyGive(s_proto_task);
}

// BLE「常可见」后连接会先于 app 启动(首页即广播)。回调未注册期间收到的
// 下行帧(积分/会话/审批)不能丢——暂存环形队列,wb_ble_set_callbacks 注册
// 时立即按序回放。否则 PC 连上后立即推的积分会被静默丢弃,设备要等下一个
// 推送周期(最长 1 分钟)才显示数据。
#define PEND_CAP 8
#define PEND_LEN 192
static char s_pend[PEND_CAP][PEND_LEN];
static uint16_t s_pend_len[PEND_CAP];
static int s_pend_head = 0, s_pend_count = 0;

void wb_ble_set_callbacks(const wb_ble_callbacks_t *cb)
{
    s_cb = cb ? *cb : (wb_ble_callbacks_t){0};
    // 回放 app 启动前暂存的下行帧(最旧在前)。逐帧弹出后再调回调,
    // 回调(cJSON 解析)耗时,不持临界区。
    while (s_pend_count > 0 && s_cb.on_line) {
        char line[PEND_LEN];
        uint16_t len;
        taskENTER_CRITICAL(&s_rx_mux);
        const int oldest = (s_pend_head - s_pend_count + PEND_CAP) % PEND_CAP;
        memcpy(line, s_pend[oldest], s_pend_len[oldest] + 1);
        len = s_pend_len[oldest];
        --s_pend_count;
        taskEXIT_CRITICAL(&s_rx_mux);
        s_cb.on_line(line, len);
    }
}

// ---- 上行帧分发 ----

static void dispatch_line(uint8_t *line, size_t len)
{
    // 诊断(2026-11-03):pcmon 帧在网线上有、设备端 handle_line 无日志——
    // 在派发层计数:每 60 帧打一条(约 15s@4Hz poll),看帧到没到、回调在不在。
    static uint32_t s_disp_n = 0;
    if (++s_disp_n % 60 == 1) {
        char head[20];
        const size_t h = len < sizeof(head) - 1 ? len : sizeof(head) - 1;
        memcpy(head, line, h);
        head[h] = '\0';
        ESP_LOGI(TAG, "dispatch#%lu len=%u cb=%d head=\"%.18s\"",
                 (unsigned long)s_disp_n, (unsigned)len,
                 (int)(s_cb.on_line != NULL), head);
    }
    // 语音 poll 直通:{"voice":"poll"} 固定 16 字节,快速比对避免 JSON 解析。
    if (len == 16 && memcmp(line, "{\"voice\":\"poll\"}", 16) == 0) {
        const size_t n = wb_voice_build_poll_response(s_poll_resp, sizeof(s_poll_resp), 8);
        if (n > 0 && n < sizeof(s_poll_resp) - 1) {
            (void)wb_ble_send_line(s_poll_resp, n);
        }
        return;
    }
    if (s_cb.on_line) {
        s_cb.on_line((const char *)line, len);
    } else if (len < PEND_LEN) {
        taskENTER_CRITICAL(&s_rx_mux);
        memcpy(s_pend[s_pend_head], line, len);
        s_pend[s_pend_head][len] = '\0';
        s_pend_len[s_pend_head] = (uint16_t)len;
        s_pend_head = (s_pend_head + 1) % PEND_CAP;
        if (s_pend_count < PEND_CAP) ++s_pend_count;
        taskEXIT_CRITICAL(&s_rx_mux);
    }
}

// 消费者任务:从环形缓冲取字节拼行,再分发。栈给到 8KB(cJSON + 上层组包)。
static void proto_task(void *arg)
{
    (void)arg;
    static char line[RX_CAP];
    size_t len = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        for (;;) {
            int ch = -1;
            taskENTER_CRITICAL(&s_rx_mux);
            if (s_rx_tail != s_rx_head) {
                ch = s_rx[s_rx_tail];
                s_rx_tail = (s_rx_tail + 1) % RX_CAP;
            }
            taskEXIT_CRITICAL(&s_rx_mux);
            if (ch < 0) break;
            if (ch == '\n') {
                if (len) dispatch_line((uint8_t *)line, len);
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = (char)ch;
            } else {
                // 无换行兜底:缓冲满则整段交上层(JSON 解析失败即丢)。
                dispatch_line((uint8_t *)line, len);
                len = 0;
            }
        }
    }
}

static void rx_reset(void)
{
    taskENTER_CRITICAL(&s_rx_mux);
    s_rx_head = s_rx_tail = 0;
    taskEXIT_CRITICAL(&s_rx_mux);
}

// ---- GATT 访问回调 ----

static int wb_access_cb(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    uint8_t tmp[256];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &len) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (len == 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    // 只做入队(短临界区),解析交给 proto_task。溢出时丢最旧字节。
    wb_ble_inject_rx(tmp, len);
    return 0;
}

static const struct ble_gatt_svc_def s_wb_services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = &s_service_uuid.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &s_char_uuid.u,
            .access_cb = wb_access_cb,
            .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_char_handle },
          {0} } },
    {0}
};

const struct ble_gatt_svc_def *wb_gatt_services(void) { return s_wb_services; }

// ---- 广播与 GAP ----

static int gap_event(struct ble_gap_event *event, void *arg);

static int advertise(void)
{
    if (!s_app_active && !s_keep_visible) {
        ESP_LOGW(TAG, "广播跳过:app 不在前台");
        return 0;
    }
    if (!ble_hs_synced()) {
        ESP_LOGW(TAG, "广播跳过:host 尚未同步");
        return 0;
    }
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "广播数据设置失败 rc=%d", rc); return rc; }

    struct ble_hs_adv_fields response = {0};
    response.name = (const uint8_t *)DEVICE_NAME;
    response.name_len = strlen(DEVICE_NAME);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) { ESP_LOGE(TAG, "扫描响应设置失败 rc=%d", rc); return rc; }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) ESP_LOGW(TAG, "广播已启动 name=%s addr_type=%u", DEVICE_NAME, s_addr_type);
    else ESP_LOGE(TAG, "广播启动失败 rc=%d", rc);
    return rc;
}

static void notify_link(bool connected)
{
    if (s_cb.on_link) s_cb.on_link(connected);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_subscribed = false;
            rx_reset();
            ESP_LOGI(TAG, "PC 已连接 conn=%u mtu=%u", s_conn, ble_att_mtu(s_conn));
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d", event->connect.status);
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "断连 reason=%d", event->disconnect.reason);
        s_conn = NO_CONN;
        s_subscribed = false;
        rx_reset();
        wb_voice_end(4);
        notify_link(false);
        if (s_started || s_keep_visible) advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == s_conn &&
            event->subscribe.attr_handle == s_char_handle) {
            s_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "订阅 notify=%u", s_subscribed);
            notify_link(s_subscribed);
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 更新 conn=%u mtu=%u", event->mtu.conn_handle, event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    const int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) {
        uint8_t own[6];
        ble_hs_id_copy_addr(s_addr_type, own, NULL);
        ESP_LOGW(TAG, "host 同步完成 addr_type=%u 本机地址=%02x:%02x:%02x:%02x:%02x:%02x",
                 s_addr_type, own[5], own[4], own[3], own[2], own[1], own[0]);
        advertise();
    } else {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto 失败 rc=%d", rc);
    }
}

static void on_reset(int reason) { ESP_LOGE(TAG, "NimBLE reset %d", reason); }
static void host_task(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }

// 共享引导:两个 BLE 应用(Vokie/WB)谁先进场谁引导;服务表统一注册双方。
// 定义在 vokie_ble.c(供本文件与 app_vokie.cc 使用)。
struct ble_gatt_svc_def;
const struct ble_gatt_svc_def *vokie_gatt_services(void);

static esp_err_t host_boot_once(void)
{
    if (s_host_booted) return ESP_OK;
    // 发送互斥 + 上行解析任务(必须早于 host 启动,否则首帧可能无人接收)。
    if (!s_tx_lock) s_tx_lock = xSemaphoreCreateMutex();
    if (!s_proto_task &&
        xTaskCreate(proto_task, "wb_proto", 8192, NULL, 5, &s_proto_task) != pdPASS) {
        ESP_LOGE(TAG, "wb_proto 任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    // host 已被 Vokie 引导过:服务表早就注册好了,不能重复 init,只接管广播。
    if (ble_host_shared_booted) {
        s_host_booted = true;
        ESP_LOGI(TAG, "复用已引导的 NimBLE host,WB 接管广播");
        return ESP_OK;
    }
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    // 先交出内存锚:锚在手里时 64KB 大块必然可用,控制器不会因碎片化拿不到
    // 连续块(此前该路径直接 assert 炸机,见 2026-11-01 实测 largest=47104 必崩)。
    guard_free("蓝牙引导");
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < 56 * 1024) {
        // 锚已被某应用借走且堆碎片化:此刻引导仍会 assert。返回失败后由
        // wb_ble_start/visible_start 的后台重试接力 —— 应用退出释放内存后
        // 自动补上,而不是永久放弃让应用离线。
        ESP_LOGE(TAG, "连续内存不足(largest=%u),稍后自动重试蓝牙引导",
                 (unsigned)largest);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "host: nimble_port_init (heap=%u largest=%u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init 失败 rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "host: 控制器已就绪,注册服务表");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    if (ble_gatts_count_cfg(s_wb_services) != 0 ||
        ble_gatts_add_svcs(s_wb_services) != 0) return ESP_FAIL;
    const struct ble_gatt_svc_def *vokie = vokie_gatt_services();
    if (vokie && (ble_gatts_count_cfg(vokie) != 0 ||
                  ble_gatts_add_svcs(vokie) != 0)) return ESP_FAIL;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGW(TAG, "host: host_task 已启动");
    ble_host_shared_booted = true;
    s_host_booted = true;
    ESP_LOGI(TAG, "NimBLE host 已引导(WB + Vokie 服务已注册)");
    return ESP_OK;
}

// 蓝牙引导后台重试:锚被应用借走且堆暂时碎片化时,等应用退出释放内存后
// 自动补引导,保证应用不会因为一次引导失败就永久离线。
static void retry_task(void *arg)
{
    for (int i = 0; i < 100; ++i) {          // 每 3 秒一次,最多约 5 分钟
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (s_host_booted) break;            // Vokie 等已把 host 引导起来
        ESP_LOGW(TAG, "重试蓝牙引导(%d): largest=%u", i + 1,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (host_boot_once() == ESP_OK) {
            for (int j = 0; j < 100 && !ble_hs_synced(); ++j)
                vTaskDelay(pdMS_TO_TICKS(10));
            if (ble_hs_synced() && s_conn == NO_CONN) {
                ble_gap_adv_stop();
                advertise();
            }
            ESP_LOGW(TAG, "蓝牙引导重试成功,链路恢复");
            break;
        }
    }
    s_retry_task = NULL;
    vTaskDelete(NULL);
}

static void retry_kick(void)
{
    if (s_retry_task) return;
    if (xTaskCreate(retry_task, "wb_ble_rtry", 4096, NULL, 3, &s_retry_task) != pdPASS) {
        s_retry_task = NULL;
    }
}

esp_err_t wb_ble_start(void)
{
    s_app_active = true;
    s_started = true;
    const esp_err_t err = host_boot_once();
    if (err != ESP_OK) {
        retry_kick();          // 后台接力重试,应用正常进场使用,连上即在线
        return err;
    }
    // 等 host 同步(有界);同步后 on_sync 会广播。
    for (int i = 0; i < 100 && !ble_hs_synced(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGW(TAG, "wb_ble_start: synced=%d conn=%d", (int)ble_hs_synced(), (int)s_conn);
    if (ble_hs_synced() && s_conn == NO_CONN) {
        ble_gap_adv_stop();          // 清掉可能残留的旧广播注册
        advertise();
    }
    return ESP_OK;
}

void wb_ble_stop(void)
{
    if (!s_started) return;
    s_started = false;
    s_app_active = false;
    wb_voice_deinit();
    if (s_conn != NO_CONN) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 30 && s_conn != NO_CONN; ++i) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_conn != NO_CONN) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();
    // 「常可见」模式:退出应用后继续广播,PC 端随时可回连。
    if (s_keep_visible && s_conn == NO_CONN) advertise();
}

// 「常可见」:首页 start() 调用。引导 host(若未引导)并立即广播;
// 之后即使不进任何 BLE 应用,PC 开机即可发现并连上设备。
esp_err_t wb_ble_visible_start(void)
{
    s_keep_visible = true;
    const esp_err_t err = host_boot_once();
    if (err != ESP_OK) {
        retry_kick();
        return err;
    }
    if (ble_hs_synced() && s_conn == NO_CONN) {
        ble_gap_adv_stop();          // 清掉可能残留的旧广播注册
        advertise();
    }
    return ESP_OK;
}

void wb_ble_visible_stop(void)
{
    s_keep_visible = false;
    if (!s_started && s_conn == NO_CONN) ble_gap_adv_stop();
}

// 把 NimBLE 栈真正还给堆。
//
// host_boot_once() 是「一次性引导、永不释放」的:退出 WorkBuddy 时
// wb_ble_stop() 只断连接、停广播,那 ~60KB 协议栈堆一直占着。C3 总共才
// 一百来 KB 堆,对讲机/小智这类要开 Opus 编码器的应用进来时
// largest 只剩 15KB,编码器申请不到连续块直接 ESP_OPUS_ENC 失败(ret=-7),
// 更极端的还会在 BLE 初始化时 panic —— 都是同一笔账。
//
// 这里做完整 deinit,下次 WorkBuddy 进场时 host_boot_once 会重新引导。
// 只应由「确实需要大块连续内存、且自己不用蓝牙」的应用在启动前按需调用。
esp_err_t wb_ble_release_host(void)
{
    if (!ble_host_shared_booted) return ESP_OK;

    ESP_LOGW(TAG, "释放 NimBLE host 前: free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    wb_ble_stop();
    if (s_proto_task) {
        vTaskDelete(s_proto_task);
        s_proto_task = NULL;
    }
    // nimble_port_stop() 会让 host_task 里的 nimble_port_run() 返回,
    // 该任务随后自行 nimble_port_freertos_deinit() 退出;deinit 必须在它之后。
    if (nimble_port_stop() != 0) {
        ESP_LOGW(TAG, "nimble_port_stop 返回非 0,仍尝试 deinit");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    const int rc = nimble_port_deinit();
    ble_host_shared_booted = false;
    s_host_booted = false;
    if (rc != 0) ESP_LOGW(TAG, "nimble_port_deinit rc=%d", rc);

    ESP_LOGW(TAG, "释放 NimBLE host 后: free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    // 立即抓回内存锚:把刚归还的 64KB 大块锚住,防止被后续任意分配碎片化,
    // 保证下次引导蓝牙必然成功。大内存应用进场时经 wb_ble_guard_loan() 借走。
    guard_grab();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

// 大内存应用(对讲机/小智)进场时调用:释放蓝牙 host 并把内存锚一并交出,
// 调用方随即拥有系统里最大的连续块。此后若需要蓝牙,host_boot_once 的
// 后台重试会接力补上(应用退出释放内存后自动连回)。
void wb_ble_guard_loan(void)
{
    wb_ble_release_host();
    guard_free("应用借出");
}
