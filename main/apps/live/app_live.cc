// app_live.cc —— 抖音直播助手：passport 应用封装层。
//
// 移植自 AI Passport 项目的 app_core.c：编排逻辑（队列/状态机/播报流程/超时）
// 原样保留；改动点：
//   1. Wi-Fi 由 passport-os 系统统一管理（原项目的 app_wifi/app_prov 配网整块删除）；
//      桥接地址优先取 NVS 手动配置，否则监听 UDP 7812 的桥接广播自动发现。
//   2. 音频走 bsp_audio + 音频工作区认领（Owner::Live），见 live_audio.cc。
//   3. 生命周期对齐 passport_app_t：start/stop 创建与回收任务，on_key 走框架分发。
//      长按"确定"已被启动器占用（返回），设置页改用长按"下"呼出。
#include "apps/app_base.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "wifi_manager.h"

#include "live_audio.h"
#include "live_config.h"
#include "live_discover.h"
#include "live_event.h"
#include "live_policy.h"
#include "live_protocol.h"
#include "live_store.h"
#include "live_ui.h"
#include "live_ui_strings.h"
#include "live_ws.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "live_json.h"
}

static const char *TAG = "live";

#define FIRMWARE_VERSION "1.1.0-passport"
#define CORE_QUEUE_DEPTH 14
#define CORE_TASK_STACK  6144
#define NET_TASK_STACK   5120
#define CORE_TICK_MS     50

// 桥接端在这段时间内没有任何回应就放弃这句话，继续处理后面的弹幕。
#define REPLY_TIMEOUT_MS 3500
// 已经开口却迟迟收不到结束标记（桥接端崩溃/断网）时的兜底。
#define UTTERANCE_TIMEOUT_MS 20000

#define BATTERY_PERIOD_MS 30000

#define SETTINGS_ITEM_COUNT 3

typedef enum {
    CORE_MSG_EVENT = 1,
    CORE_MSG_SAY,
    CORE_MSG_HELLO,
    CORE_MSG_STATS,
    CORE_MSG_KEY,
    CORE_MSG_BYE,
} core_msg_type_t;

typedef struct {
    core_msg_type_t type;
    app_event_t event;        // CORE_MSG_EVENT
    uint16_t say_id;          // CORE_MSG_SAY
    app_kind_t say_kind;
    char text[APP_TEXT_MAX];  // CORE_MSG_SAY / HELLO(room) / BYE
    char user[APP_USER_MAX];
    uint32_t online;
    uint32_t likes;
    uint32_t gifts;
    uint8_t btn;              // CORE_MSG_KEY
    app_btn_event_t btn_event;
} core_msg_t;

namespace {

app_config_t s_config;
app_policy_t s_policy;
QueueHandle_t s_queue;
TaskHandle_t s_core_task;
TaskHandle_t s_net_task;
volatile bool s_run;          // 任务退出标志

volatile bool s_muted;
volatile bool s_settings_open;
volatile int s_settings_index;
volatile app_ws_state_t s_ws_state;

volatile bool s_ws_started;   // net 任务视角：WS 客户端已建立
volatile bool s_ws_reconnect_req;  // 设置页"重连桥接"

uint16_t s_pending_id;        // 已发出 want、等待语音的播报
uint64_t s_pending_since_ms;
uint64_t s_utterance_started_ms;
bool s_audio_started;

uint32_t s_online;
uint32_t s_likes;
uint32_t s_gifts;
uint32_t s_follows;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

void post(const core_msg_t *msg)
{
    if (s_queue == NULL) return;
    if (xQueueSend(s_queue, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "核心队列已满，丢弃消息 type=%d", (int)msg->type);
    }
}

void refresh_counters()
{
    app_ui_set_counters(s_online, s_likes, s_gifts, s_follows);
}

/* ------------------------------ 桥接回调 ------------------------------ */

void on_ws_message(const char *json, void *user)
{
    (void)user;
    app_message_t parsed;
    if (!app_protocol_parse(json, &parsed)) {
        ESP_LOGW(TAG, "无法解析的桥接消息: %.60s", json);
        return;
    }

    core_msg_t msg = {};
    switch (parsed.type) {
    case APP_MSG_EVENT:
        msg.type = CORE_MSG_EVENT;
        msg.event = parsed.event;
        post(&msg);
        break;
    case APP_MSG_SAY:
        msg.type = CORE_MSG_SAY;
        msg.say_id = parsed.say_id;
        msg.say_kind = parsed.say_kind;
        snprintf(msg.text, sizeof(msg.text), "%s", parsed.text);
        snprintf(msg.user, sizeof(msg.user), "%s", parsed.event.user);
        post(&msg);
        break;
    case APP_MSG_HELLO:
        msg.type = CORE_MSG_HELLO;
        snprintf(msg.text, sizeof(msg.text), "%s", parsed.text);
        post(&msg);
        break;
    case APP_MSG_STATS:
        msg.type = CORE_MSG_STATS;
        msg.online = parsed.online;
        msg.likes = parsed.likes;
        msg.gifts = parsed.gifts;
        post(&msg);
        break;
    case APP_MSG_BYE:
        msg.type = CORE_MSG_BYE;
        snprintf(msg.text, sizeof(msg.text), "%s", parsed.text);
        post(&msg);
        break;
    default:
        break;
    }
}

void on_ws_binary(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    app_frame_t frame;
    if (!app_frame_decode(data, len, &frame)) return;
    if (frame.codec != APP_FRAME_CODEC_PCM16) return;
    if (frame.speak_id != s_pending_id) return;      // 迟到的语音帧
    if (frame.payload_len < sizeof(int16_t)) return;

    if (!s_audio_started) {
        if (!app_audio_begin(frame.speak_id)) return;
        s_audio_started = true;
        s_utterance_started_ms = (uint64_t)now_ms();
    }

    const bool last = (frame.flags & APP_FRAME_FLAG_LAST) != 0;
    (void)app_audio_write(frame.speak_id, (const int16_t *)frame.payload,
                          frame.payload_len / sizeof(int16_t), last);
}

void on_ws_state(app_ws_state_t state, void *user)
{
    (void)user;
    s_ws_state = state;
    switch (state) {
    case APP_WS_OPEN: {
        app_ui_set_state(APP_UI_LIVE);
        // 握手完成后立刻自报家门，桥接端据此在界面上区分设备。
        char hello[96];
        if (app_protocol_build_hello(hello, sizeof(hello), FIRMWARE_VERSION) > 0) {
            (void)app_ws_send_text(hello);
        }
        break;
    }
    case APP_WS_CONNECTING: app_ui_set_state(APP_UI_CONNECTING); break;
    case APP_WS_CLOSED:
    case APP_WS_IDLE:
    default:
        app_ui_set_state(APP_UI_OFFLINE);
        break;
    }
}

void on_audio_finished(uint16_t speak_id, bool completed)
{
    if (s_queue == NULL) return;
    (void)completed;
    // 音频任务上下文：只做状态收敛，不直接碰策略/界面（避免跨任务竞态）。
    // 策略释放放回 core 任务：post 一条内部消息太重，这里直接改 volatile 状态，
    // app_policy_finish 本身只操作无锁结构，且 speak 流程是单发单收的。
    app_policy_finish(&s_policy, (uint64_t)now_ms(), true);
    s_pending_id = 0;
    s_audio_started = false;
    app_ui_clear_speech();
}

/* ------------------------------ 播报流程 ------------------------------ */

void request_speech(uint64_t now)
{
    if (s_muted || s_pending_id != 0) return;

    app_event_t event;
    uint16_t id = 0;
    if (!app_policy_next(&s_policy, now, &event, &id)) return;

    char payload[512];
    if (app_protocol_build_want(payload, sizeof(payload), id, &event) == 0) {
        app_policy_finish(&s_policy, now, false);
        return;
    }

    if (app_ws_send_text(payload) != ESP_OK) {
        // 桥接断了：这句话没有意义，直接结束并清空队列，等重连后再从新事件开始。
        app_policy_finish(&s_policy, now, false);
        app_policy_reset_queue(&s_policy, now);
        return;
    }

    s_pending_id = id;
    s_pending_since_ms = now;
    s_audio_started = false;
    ESP_LOGD(TAG, "请求播报 id=%u kind=%s", (unsigned)id, app_kind_name(event.kind));
}

void check_pending_timeout(uint64_t now)
{
    if (s_pending_id == 0) return;

    if (!s_audio_started && now - s_pending_since_ms > REPLY_TIMEOUT_MS) {
        ESP_LOGW(TAG, "桥接端未返回语音，跳过 id=%u", (unsigned)s_pending_id);
        app_ui_toast(LV_TXT_TOAST_SPEAK_FAIL);
        app_policy_finish(&s_policy, now, false);
        s_pending_id = 0;
        return;
    }

    if (s_audio_started && now - s_utterance_started_ms > UTTERANCE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "播报超时，强制结束 id=%u", (unsigned)s_pending_id);
        app_audio_stop();
        app_policy_finish(&s_policy, now, false);
        s_pending_id = 0;
        s_audio_started = false;
    }
}

/* ------------------------------ 设置页 ------------------------------ */

void fill_settings(app_ui_settings_t *view)
{
    // 面板只有 192 px 宽：默认端口和 "ws://" 前缀都不显示。
    static char bridge[APP_CFG_HOST_MAX + 8];
    if (s_config.host[0] == '\0') {
        snprintf(bridge, sizeof(bridge), "--");
    } else if (s_config.port == APP_CFG_DEFAULT_PORT) {
        snprintf(bridge, sizeof(bridge), "%s", s_config.host);
    } else {
        snprintf(bridge, sizeof(bridge), "%s:%u", s_config.host, (unsigned)s_config.port);
    }

    view->bridge = bridge;
    view->wifi = WifiManager::GetInstance().IsConnected() ? "已连接" : "--";
    view->token_hint = s_config.token[0] ? "已配置" : "未配置";
    view->volume = app_audio_volume();
    view->muted = s_muted;
    view->greet_enter = s_config.greet_enter;
    view->thank_gift = s_config.thank_gift;
    view->thank_follow = s_config.thank_follow;
    view->thank_like = s_config.thank_like;
    view->answer_comment = s_config.answer_comment;
}

void open_settings()
{
    app_ui_settings_t view;
    fill_settings(&view);
    s_settings_index = 0;
    s_settings_open = true;
    app_ui_show_settings(&view);
    app_ui_settings_select(0);
}

void toggle_mute()
{
    s_muted = !s_muted;
    app_ui_set_muted(s_muted);
    app_ui_toast(s_muted ? LV_TXT_TOAST_MUTED : LV_TXT_TOAST_UNMUTED);
    if (s_muted) {
        app_audio_stop();
        if (s_pending_id != 0) {
            app_policy_finish(&s_policy, (uint64_t)now_ms(), false);
            s_pending_id = 0;
            s_audio_started = false;
        }
    }
}

void change_volume(int delta)
{
    int volume = (int)app_audio_volume() + delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    app_audio_set_volume((uint8_t)volume);
    s_config.volume = (uint8_t)volume;

    // 桥接地址只来自 UDP 广播，不落盘：否则发现结果会随音量设置一起被
    // 持久化，重启后被误当成“手动配置”而拒绝跟随新的广播。
    app_config_t persist = s_config;
    persist.host[0] = '\0';
    persist.token[0] = '\0';
    (void)app_store_save(&persist);

    char buffer[40];
    snprintf(buffer, sizeof(buffer), "%s %d%%", LV_TXT_TOAST_VOLUME, volume);
    app_ui_toast(buffer);
}

void activate_settings_item()
{
    switch (s_settings_index) {
    case 0:
        s_ws_reconnect_req = true;
        app_ui_toast(LV_TXT_TOAST_RECONNECT);
        break;
    case 1:
        toggle_mute();
        open_settings();
        break;
    case 2:
    default:
        app_ui_hide_settings();
        s_settings_open = false;
        break;
    }
}

void handle_key(uint8_t btn, app_btn_event_t event)
{
    if (event == BTN_EVT_LONG && btn == BSP_BTN_DOWN) {
        if (s_settings_open) {
            app_ui_hide_settings();
            s_settings_open = false;
        } else {
            open_settings();
        }
        return;
    }
    if (event != BTN_EVT_CLICK) return;

    if (s_settings_open) {
        if (btn == BSP_BTN_UP) {
            s_settings_index = (s_settings_index + SETTINGS_ITEM_COUNT - 1) % SETTINGS_ITEM_COUNT;
            app_ui_settings_select(s_settings_index);
        } else if (btn == BSP_BTN_DOWN) {
            s_settings_index = (s_settings_index + 1) % SETTINGS_ITEM_COUNT;
            app_ui_settings_select(s_settings_index);
        } else if (btn == BSP_BTN_OK) {
            activate_settings_item();
        }
        return;
    }

    if (btn == BSP_BTN_OK) {
        toggle_mute();
    } else if (btn == BSP_BTN_UP) {
        change_volume(10);
    } else if (btn == BSP_BTN_DOWN) {
        change_volume(-10);
    }
}

/* ------------------------------ 消息处理 ------------------------------ */

void handle_event(const app_event_t *event, uint64_t now)
{
    switch (event->kind) {
    case APP_KIND_GIFT: s_gifts += event->count ? event->count : 1; break;
    case APP_KIND_FOLLOW: s_follows += 1; break;
    case APP_KIND_LIKE: s_likes += event->count ? event->count : 1; break;
    default: break;
    }

    app_ui_push_event(event->kind, event->user, event->text);
    refresh_counters();

    if (!app_policy_on_event(&s_policy, event, now)) {
        ESP_LOGD(TAG, "事件未进入播报队列: %s", app_kind_name(event->kind));
    }
}

void process_message(core_msg_t *msg, uint64_t now)
{
    switch (msg->type) {
    case CORE_MSG_EVENT:
        handle_event(&msg->event, now);
        break;
    case CORE_MSG_SAY:
        if (msg->say_id != s_pending_id) break;
        app_ui_show_speech(msg->say_kind, msg->user, msg->text);
        break;
    case CORE_MSG_HELLO:
        if (msg->text[0]) {
            app_ui_set_room(msg->text);
            app_ui_set_state(APP_UI_LIVE);
        }
        break;
    case CORE_MSG_STATS:
        s_online = msg->online;
        if (msg->likes > s_likes) s_likes = msg->likes;
        if (msg->gifts > s_gifts) s_gifts = msg->gifts;
        refresh_counters();
        break;
    case CORE_MSG_BYE:
        app_ui_set_state(APP_UI_OFFLINE);
        app_policy_reset_queue(&s_policy, now);
        break;
    case CORE_MSG_KEY:
        handle_key(msg->btn, msg->btn_event);
        break;
    default:
        break;
    }
}

/* ------------------------------ 任务 ------------------------------ */

void core_task(void *arg)
{
    (void)arg;
    uint64_t last_tick = 0;
    uint64_t last_battery = 0;

    while (s_run) {
        core_msg_t msg;
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(CORE_TICK_MS)) == pdTRUE) {
            process_message(&msg, (uint64_t)now_ms());
            // 尽量把同一批消息处理完再进入策略计算，避免一句话被拆成几次判断。
            while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
                process_message(&msg, (uint64_t)now_ms());
            }
        }

        const uint64_t now = (uint64_t)now_ms();
        if (now - last_tick >= 200) {
            last_tick = now;
            app_policy_on_tick(&s_policy, now);
            check_pending_timeout(now);
            request_speech(now);
        }

        if (now - last_battery >= BATTERY_PERIOD_MS) {
            last_battery = now;
            app_ui_set_battery(bsp_battery_soc());
        }
    }
}

void net_task(void *arg)
{
    (void)arg;
    uint64_t ws_closed_since = 0;

    app_ws_callbacks_t callbacks = {
        .on_message = on_ws_message,
        .on_binary = on_ws_binary,
        .on_state = on_ws_state,
        .user = nullptr,
    };

    while (s_run) {
        const uint64_t now = (uint64_t)now_ms();

        // 桥接地址：始终跟随 UDP 广播自动发现。设备没有配网界面，NVS 里
        // 残留的旧地址（比如换过电脑测试）必须能被广播覆盖，否则会永久
        // 卡死在已失效的地址上。
        {
            live_discover_result_t found;
            if (live_discover_get(&found)) {
                if (strcmp(s_config.host, found.host) != 0 ||
                    s_config.port != found.port ||
                    strcmp(s_config.token, found.token) != 0) {
                    snprintf(s_config.host, sizeof(s_config.host), "%s", found.host);
                    s_config.port = found.port;
                    snprintf(s_config.token, sizeof(s_config.token), "%s", found.token);
                    // 发现内容变化：旧连接作废，按新地址重建。
                    if (s_ws_started) {
                        app_ws_stop();
                        s_ws_started = false;
                    }
                    ESP_LOGI(TAG, "跟随广播更新桥接 %s:%u", found.host, (unsigned)found.port);
                }
            } else if (s_config.host[0] != '\0') {
                s_config.host[0] = '\0';
                if (s_ws_started) {
                    app_ws_stop();
                    s_ws_started = false;
                }
            }
        }

        if (!WifiManager::GetInstance().IsConnected()) {
            if (s_ws_started) {
                app_ws_stop();
                s_ws_started = false;
                ws_closed_since = 0;
            }
            app_ui_set_network("等待 Wi-Fi…");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (s_ws_reconnect_req) {
            s_ws_reconnect_req = false;
            if (s_ws_started) {
                app_ws_stop();
                s_ws_started = false;
                ws_closed_since = 0;
            }
        }

        if (s_config.host[0] == '\0') {
            if (s_ws_started) {
                app_ws_stop();
                s_ws_started = false;
            }
            app_ui_set_network(LV_TXT_TOAST_NO_CONFIG);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!s_ws_started) {
            char net[72];
            snprintf(net, sizeof(net), "%s:%u", s_config.host, (unsigned)s_config.port);
            app_ui_set_network(net);
            if (app_ws_start(&s_config, &callbacks) == ESP_OK) {
                s_ws_started = true;
                ws_closed_since = now;
            }
        } else if (s_ws_state == APP_WS_OPEN) {
            ws_closed_since = 0;
        } else if (s_ws_state == APP_WS_CLOSED) {
            // 客户端自己会退避重连；只有长时间连不上才彻底重建，避免半死连接。
            if (ws_closed_since == 0) {
                ws_closed_since = now;
            } else if (now - ws_closed_since > 30000) {
                app_ws_stop();
                s_ws_started = false;
                ws_closed_since = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (s_ws_started) app_ws_stop();
}

/* ------------------------------ 生命周期 ------------------------------ */

void init() {}

void start()
{
    if (s_queue != NULL) return;   // 已在运行

    app_config_defaults(&s_config);
    app_policy_cfg_t policy_cfg;
    app_policy_defaults(&policy_cfg);

    const bool has_config = (app_store_load(&s_config) == ESP_OK);
    (void)has_config;
    policy_cfg.greet_enter = s_config.greet_enter;
    policy_cfg.thank_gift = s_config.thank_gift;
    policy_cfg.thank_follow = s_config.thank_follow;
    policy_cfg.thank_like = s_config.thank_like;
    policy_cfg.answer_comment = s_config.answer_comment;
    app_policy_init(&s_policy, &policy_cfg, (uint64_t)now_ms(), esp_random());

    s_run = true;
    s_muted = false;
    s_settings_open = false;
    s_ws_started = false;
    s_ws_reconnect_req = false;
    s_pending_id = 0;
    s_audio_started = false;
    s_online = s_likes = s_gifts = s_follows = 0;

    s_queue = xQueueCreate(CORE_QUEUE_DEPTH, sizeof(core_msg_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "核心队列创建失败");
        return;
    }

    app_ui_init();
    app_ui_set_battery(bsp_battery_soc());

    if (app_audio_init(s_config.volume) == ESP_OK) {
        app_audio_set_finished_callback(on_audio_finished, NULL);
    } else {
        ESP_LOGE(TAG, "音频初始化失败，将只显示不播报");
    }

    live_discover_start();

    if (xTaskCreate(core_task, "live_core", CORE_TASK_STACK, NULL, 5, &s_core_task) != pdPASS) {
        ESP_LOGE(TAG, "核心任务创建失败");
        return;
    }
    if (xTaskCreate(net_task, "live_net", NET_TASK_STACK, NULL, 4, &s_net_task) != pdPASS) {
        ESP_LOGE(TAG, "网络任务创建失败");
        return;
    }

    app_ui_set_state(APP_UI_CONNECTING);
    ESP_LOGI(TAG, "直播助手启动: bridge=%s:%u", s_config.host, (unsigned)s_config.port);
}

void stop()
{
    s_run = false;
    if (s_core_task) {
        for (int i = 0; i < 30 && s_core_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (s_core_task) { vTaskDelete(s_core_task); s_core_task = NULL; }
    }
    if (s_net_task) {
        for (int i = 0; i < 30 && s_net_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (s_net_task) { vTaskDelete(s_net_task); s_net_task = NULL; }
    }
    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }

    live_discover_stop();
    app_audio_stop();
    app_audio_deinit();
    app_ui_deinit();
    s_pending_id = 0;
    s_audio_started = false;
    s_settings_open = false;
    ESP_LOGI(TAG, "直播助手已退出");
}

void on_key(uint8_t btn, app_btn_event_t event, uint16_t mv)
{
    (void)mv;
    if (s_queue == NULL) return;
    core_msg_t msg = {};
    msg.type = CORE_MSG_KEY;
    msg.btn = btn;
    msg.btn_event = event;
    post(&msg);
}

} // namespace

extern const passport_app_t g_live_app = {
    .id = APP_ID_LIVE,
    .name = "直播助手",
    .en_name = "LIVE",
    .desc = "抖音直播弹幕互动\n礼物/进房/点赞语音致谢",
    .tag = "Wi-Fi",
    .theme_color = 0xFF2D55,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
};
