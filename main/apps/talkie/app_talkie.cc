// 网络对讲机：两台(或多台)设备连同一台中转服务，按上键就说话。
//
// 设计要点：
//   * 严格半双工，和真对讲机一样 —— 麦克风和喇叭同时开就必然啸叫，
//     AudioService 本身就是半双工的，这里只是把状态机摆正：
//        待机 → 按住上键 → 讲话中(录音+发送) → 松手 → 待机
//        待机 → 收到对方帧 → 收听(播放) → 静默 3 秒 → 待机
//   * 网络和音频不在一条任务里。网络/心跳在 talkie_link 自己的任务，
//     音频启停必须在 app 主任务里做(AudioService 的半双工约束)。
//   * 波形柱高度取自 Opus 帧长度。Opus 是变码率编码，说话响时帧更长，
//     所以这不是随机动画，是真实载荷的映射；只是它反映"编码后字节数"
//     而不是样本峰值 —— 要精确峰值得让 AudioService 额外暴露电平，
//     那会把编码路径也改掉，这一版先不动。

#include "apps/talkie/app_talkie.h"

#include "apps/talkie/talkie_link.h"
#include "apps/talkie/talkie_local.h"
#include "apps/wbconsole/wb_gatt.h"
#include "apps/xiaozhi/xiaozhi_audio.h"

#include "app_fonts.h"
#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "launcher/app_registry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "wifi_manager.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const char *TAG = "app_talkie";

// 深色仪表配色：冷底 + 青绿"通话中"。不用主题色是因为对讲机是常驻功能页，
// 跟着 Launcher 的纸感主题走会看不清户外强光下的电平。
constexpr uint32_t COL_BG = 0x0A0F14;
constexpr uint32_t COL_PANEL = 0x121A22;
constexpr uint32_t COL_EDGE = 0x223040;
constexpr uint32_t COL_TEXT = 0xE6EFF7;
constexpr uint32_t COL_MUTED = 0x7E909F;
constexpr uint32_t COL_DIM = 0x1A2531;
constexpr uint32_t COL_GREEN = 0x39D98A;
constexpr uint32_t COL_CYAN = 0x45C8E8;
constexpr uint32_t COL_AMBER = 0xF0A93B;
constexpr uint32_t COL_RED = 0xE5545A;

constexpr int SCR_W = 240;
constexpr int SCR_H = 320;
constexpr int M = 6;
constexpr int CW = SCR_W - 2 * M;

constexpr int WAVE_N = 12;
constexpr int VOL_N = 8;
constexpr int VOLUME_DEFAULT = 70;
// 收完最后一帧多久关掉播放通道。关掉才能让麦克风独占 codec(半双工)。
constexpr int RX_HOLD_MS = 3000;
// 切信道要按两下。三个键共用一路 ADC 分压，握持时手掌蹭到下键就会产生
// 一次合法 CLICK（电压落在 150~450mV 窗口达 180ms 即成立），单击直接切
// 会毫无征兆地掉线重连。改成"第一下待命、第二下生效"，误触最坏只是闪一行提示。
constexpr int64_t SWITCH_ARM_US = 2000 * 1000;

// ---------------------------------------------------------------- 控件

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_id_lbl = nullptr;
lv_obj_t *s_net_lbl = nullptr;
lv_obj_t *s_rtt_lbl = nullptr;
lv_obj_t *s_sig[3] = {};
lv_obj_t *s_chan_lbl = nullptr;
lv_obj_t *s_online_lbl = nullptr;
lv_obj_t *s_state_lbl = nullptr;
lv_obj_t *s_hint_lbl = nullptr;
lv_obj_t *s_wave[WAVE_N] = {};
uint8_t s_wave_h[WAVE_N] = {};
lv_obj_t *s_sig_txt = nullptr;
lv_obj_t *s_sig_pct = nullptr;
lv_obj_t *s_vol_txt = nullptr;
lv_obj_t *s_vol_cell[VOL_N] = {};
lv_obj_t *s_ptt = nullptr;
lv_obj_t *s_ptt_lbl = nullptr;
lv_obj_t *s_foot = nullptr;

// ---------------------------------------------------------------- 状态

std::atomic<bool> s_active{false};
std::atomic<bool> s_want_tx{false};    // 按键请求：想说话
std::atomic<int> s_last_len{0};        // 最近一帧 Opus 的字节数，波形用
bool s_tx = false;                     // 实际在录音
bool s_playing = false;
bool s_audio_inited = false;           // AudioService 是否已 Init（仅异地模式用）
int64_t s_rx_last_us = 0;
int s_volume = VOLUME_DEFAULT;
talkie::Config s_cfg{};
// 双模式：false=异地(服务器中转，Opus) / true=近场(ESP-NOW 直连，ADPCM)。
// NVS 持久化，重启后保留；长按下键切换。
std::atomic<bool> s_mode_local{false};
std::atomic<bool> s_mode_switch_req{false};
// 下键第一下的时刻(0=未待命)。32 位核上 int64 写入非原子，键盘线程与
// LVGL 定时器都会碰它，必须用 atomic。
std::atomic<int64_t> s_switch_arm_us{0};
std::atomic<int64_t> s_vol_arm_us{0};  // OK 键双击调音量的待命时刻（与切信道独立，互不干扰）

// app 主任务句柄，stop 时要它退干净
TaskHandle_t s_task = nullptr;
lv_timer_t *s_timer = nullptr;

// ---------------------------------------------------------------- 小工具

void set_text(lv_obj_t *o, const char *s) {
    if (o) lv_label_set_text(o, s);
}

void set_color(lv_obj_t *o, uint32_t c) {
    if (o) lv_obj_set_style_text_color(o, lv_color_hex(c), 0);
}

lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t border, int radius,
                    uint32_t bg, int bw = 1) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w,
                     int h, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(o, 1, 0);
    lv_obj_set_style_text_align(o, align, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    return o;
}

lv_obj_t *make_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius = 0) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// ---------------------------------------------------------------- 模式切换

constexpr char kModeNvsNs[] = "talkie";
constexpr char kModeNvsKey[] = "lmode";

void mode_load(void) {
    nvs_handle_t h;
    if (nvs_open(kModeNvsNs, NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0;
        if (nvs_get_u8(h, kModeNvsKey, &m) == ESP_OK) s_mode_local.store(m == 1);
        nvs_close(h);
    }
}

void mode_save(bool local) {
    nvs_handle_t h;
    if (nvs_open(kModeNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, kModeNvsKey, local ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// 异地模式音频/网络资源启停（本地模式完全不碰 AudioService，省下 ~40KB 连续堆）
bool remote_audio_start(void) {
    auto &audio = xiaozhi::AudioService::GetInstance();
    const esp_err_t err = audio.Init(audio_workspace::Owner::Talkie);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s free=%u largest=%u", esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    // 设备播放 16k（ES8311 在 24k 下 set_format 失败=无声，详见 start 处注释）
    audio.SetPlaybackSampleRate(16000);
    if (!audio.PrepareSessionResources()) {
        ESP_LOGE(TAG, "音频会话缓冲准备失败");
        audio.Deinit();
        return false;
    }
    s_audio_inited = true;
    bsp_audio_set_volume(static_cast<uint8_t>(s_volume));
    talkie::start();
    return true;
}

void remote_audio_stop(void) {
    talkie::stop();
    if (s_audio_inited) {
        xiaozhi::AudioService::GetInstance().Deinit();
        s_audio_inited = false;
    }
    s_tx = false;
    s_playing = false;
}

// ---------------------------------------------------------------- 界面

// 布局(240x320)：
//   4.. 28  顶栏：房间胶囊 / 连接状态 / 往返时延 / 信号三格
//   32.. 50 信道行
//   54..188 主卡：状态大字 / 副标题 / 波形
//  194..238 仪表卡：信号强度、音量
//  244..288 PTT 大按钮
//  292..312 页脚操作提示
void build_ui(lv_obj_t *scr) {
    // ---- 顶栏 ----
    lv_obj_t *top = make_card(scr, M, 4, CW, 24, COL_EDGE, 6, COL_PANEL);
    (void)top;

    lv_obj_t *pill = make_card(scr, M + 2, 7, 50, 18, COL_GREEN, 4, COL_DIM);
    (void)pill;
    // 只放房间号。之前是 "ID 1240",16px 字体下这五个字符比 62px 的胶囊还宽,
    // 居中之后首尾各被裁掉一截,顶栏显示成 "D 1240"。
    s_id_lbl = make_label(scr, &buddy_font_16, COL_GREEN, M + 2, 7, 50, 18, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_id_lbl, "----");

    s_net_lbl = make_label(scr, &buddy_font_16, COL_MUTED, M + 58, 7, 58, 18);
    lv_label_set_text(s_net_lbl, "离线");

    s_rtt_lbl = make_label(scr, &lv_font_montserrat_14, COL_MUTED, M + 120, 8, 62, 16,
                           LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_rtt_lbl, "--ms");

    // 三根递增的信号柱，按 RSSI 点亮；贴顶栏右沿
    for (int i = 0; i < 3; ++i) {
        const int h = 6 + i * 4;
        s_sig[i] = make_block(scr, M + 202 + i * 7, 7 + (14 - h), 5, h, COL_DIM, 1);
    }

    // ---- 信道行 ----
    // 左侧「信道 CHx」文字已去掉：近场模式顶栏胶囊已有 CHx 徽标，重复显示。
    // 标签保留占位，供异地模式/提示复用。
    s_chan_lbl = make_label(scr, &buddy_font_16, COL_TEXT, M + 2, 32, 90, 20);
    lv_label_set_text(s_chan_lbl, "");
    s_online_lbl = make_label(scr, &buddy_font_16, COL_CYAN, M + 92, 32, CW - 94, 20,
                              LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_online_lbl, "在线 0 人");

    // ---- 主卡 ----
    lv_obj_t *card = make_card(scr, M, 54, CW, 134, COL_EDGE, 6, COL_PANEL);

    s_state_lbl = make_label(card, &buddy_font_16, COL_TEXT, 0, 20, CW, 22, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_state_lbl, "未连接");

    s_hint_lbl = make_label(card, &buddy_font_16, COL_MUTED, 4, 46, CW - 8, 20,
                            LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_hint_lbl, "还没设置服务器");

    // 波形：12 根，宽 8 间距 6 → 12*8+11*6 = 162，居中
    constexpr int ww = 8, wgap = 6;
    constexpr int total = WAVE_N * ww + (WAVE_N - 1) * wgap;
    constexpr int x0 = (CW - total) / 2;
    constexpr int wave_cy = 99;      // 相对卡片(54)的中心线
    for (int i = 0; i < WAVE_N; ++i) {
        s_wave_h[i] = 4;
        s_wave[i] = make_block(card, x0 + i * (ww + wgap), wave_cy - 2, ww, 4, COL_EDGE, 4);
    }

    // ---- 仪表卡 ----
    lv_obj_t *meter = make_card(scr, M, 194, CW, 44, COL_EDGE, 6, COL_PANEL);

    lv_obj_t *l1 = make_label(meter, &buddy_font_16, COL_MUTED, 8, 4, 40, 18);
    lv_label_set_text(l1, "信号");
    s_sig_txt = make_label(meter, &lv_font_montserrat_14, COL_TEXT, 46, 6, 92, 16);
    lv_label_set_text(s_sig_txt, "-- dBm");
    s_sig_pct = make_label(meter, &lv_font_montserrat_14, COL_CYAN, CW - 52, 6, 44, 16,
                           LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_sig_pct, "--");

    lv_obj_t *l2 = make_label(meter, &buddy_font_16, COL_MUTED, 8, 23, 40, 18);
    lv_label_set_text(l2, "音量");
    s_vol_txt = make_label(meter, &lv_font_montserrat_14, COL_TEXT, 46, 25, 52, 16);
    lv_label_set_text(s_vol_txt, "70%");
    for (int i = 0; i < VOL_N; ++i) {
        s_vol_cell[i] = make_block(meter, CW - 8 - (VOL_N - i) * 12, 26, 9, 12, COL_DIM, 2);
    }

    // ---- PTT ----
    s_ptt = make_card(scr, M, 244, CW, 44, COL_GREEN, 8, COL_DIM, 2);
    s_ptt_lbl = make_label(s_ptt, &buddy_font_16, COL_GREEN, 0, 11, CW, 22, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_ptt_lbl, "按住上键讲话");

    s_foot = make_label(scr, &buddy_font_16, COL_MUTED, M, 292, CW, 20, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_foot, "上键说话 · 双击下键切频道 · 双击OK调音 · 长按下键切模式");
}

// ---------------------------------------------------------------- 渲染

const char *state_text(uint32_t *color) {
    using talkie::State;
    if (s_mode_local.load()) {
        talkie_local_snapshot_t snap{};
        talkie_local_get_snapshot(&snap);
        if (snap.tx) {
            *color = COL_GREEN;
            return "讲话中";
        }
        if (snap.rx) {
            *color = COL_CYAN;
            return snap.speaker[0] ? snap.speaker : "收听中";
        }
        if (snap.busy) {
            *color = COL_AMBER;
            return "频道占用";
        }
        if (snap.requesting) {
            *color = COL_AMBER;
            return "申请发言";
        }
        if (snap.weak) {
            *color = COL_RED;
            return "信号弱";
        }
        if (snap.peers == 0) {
            *color = COL_MUTED;
            return "无设备";
        }
        *color = COL_TEXT;
        return "待机";
    }
    if (!talkie::joined()) {
        switch (talkie::state()) {
            case State::kConnecting:
                *color = COL_AMBER;
                return "连接中…";
            case State::kFailed:
                *color = COL_RED;
                return "连接失败";
            default:
                *color = COL_MUTED;
                return "未连接";
        }
    }
    if (s_tx) {
        *color = COL_GREEN;
        return "讲话中";
    }
    if (talkie::remote_talking()) {
        *color = COL_CYAN;
        return "对方讲话";
    }
    *color = COL_TEXT;
    return "待机";
}

void render() {
    char buf[48];
    talkie_local_snapshot_t lsnap{};
    const bool local = s_mode_local.load();
    if (local) talkie_local_get_snapshot(&lsnap);

    // 顶栏
    if (local) {
        std::snprintf(buf, sizeof(buf), "CH%d", lsnap.channel);
        set_text(s_id_lbl, buf);
        set_text(s_net_lbl, "近场");
        set_color(s_net_lbl, COL_CYAN);
        set_text(s_rtt_lbl, "");
    } else {
        std::snprintf(buf, sizeof(buf), "%s", s_cfg.room);
        set_text(s_id_lbl, buf);

        const bool conn = talkie::joined();
        if (conn) {
            set_text(s_net_lbl, "联网");
            set_color(s_net_lbl, COL_GREEN);
        } else if (talkie::state() == talkie::State::kConnecting) {
            set_text(s_net_lbl, "连接中");
            set_color(s_net_lbl, COL_AMBER);
        } else {
            set_text(s_net_lbl, "离线");
            set_color(s_net_lbl, COL_MUTED);
        }

        const int rtt = talkie::rtt_ms();
        if (conn && rtt > 0) {
            std::snprintf(buf, sizeof(buf), "%dms", rtt);
            set_text(s_rtt_lbl, buf);
            set_color(s_rtt_lbl, rtt < 150 ? COL_GREEN : (rtt < 400 ? COL_AMBER : COL_RED));
        } else {
            set_text(s_rtt_lbl, "--ms");
            set_color(s_rtt_lbl, COL_MUTED);
        }
    }

    // 信号柱：只用真实 RSSI，连不上就全灭
    int rssi = -127;
    if (WifiManager::GetInstance().IsConnected()) rssi = WifiManager::GetInstance().GetRssi();
    int bars = 0;
    if (rssi > -60) bars = 3;
    else if (rssi > -72) bars = 2;
    else if (rssi > -85) bars = 1;
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_color(s_sig[i], lv_color_hex(i < bars ? COL_GREEN : COL_DIM), 0);
    }

    // 在线人数
    if (local) {
        std::snprintf(buf, sizeof(buf), "在线 %d 台", lsnap.peers);
        set_text(s_online_lbl, buf);
        set_text(s_chan_lbl, "");   // 近场模式信道已在顶栏 CHx 胶囊显示，不再重复
    } else {
        const int peers = talkie::joined() ? talkie::peers() : 0;
        std::snprintf(buf, sizeof(buf), "在线 %d 人", peers);
        set_text(s_online_lbl, buf);
        set_text(s_chan_lbl, "");   // 异地模式没有"信道"概念，房间号已在顶栏显示（原信道行已废弃）
    }

    // 状态
    uint32_t sc = COL_TEXT;
    const char *st = state_text(&sc);
    set_text(s_state_lbl, st);
    set_color(s_state_lbl, sc);

    // 副标题：优先报错，其次操作提示
    // 切信道/调音量的"待命"提示只在 2 秒内有效，过期就清掉，避免一直挂在屏幕上。
    const int64_t arm_at = s_switch_arm_us.load();
    const bool arm_pending = arm_at != 0 && esp_timer_get_time() - arm_at < SWITCH_ARM_US;
    if (!arm_pending && arm_at != 0) s_switch_arm_us.store(0);
    const int64_t vol_at = s_vol_arm_us.load();
    const bool vol_pending = vol_at != 0 && esp_timer_get_time() - vol_at < SWITCH_ARM_US;
    if (!vol_pending && vol_at != 0) s_vol_arm_us.store(0);
    set_color(s_hint_lbl, COL_MUTED);      // 先归位，否则上帧的高亮会残留到下个状态

    if (local) {
        if (lsnap.tx) {
            set_text(s_hint_lbl, "松手结束");
        } else if (lsnap.rx) {
            set_text(s_hint_lbl, "正在收听");
        } else if (lsnap.weak) {
            set_text(s_hint_lbl, "信号弱，可能丢字");
            set_color(s_hint_lbl, COL_RED);
        } else if (lsnap.peers == 0) {
            // 区分两个"频道"：CH1-4 是逻辑频道(双击下键切)，射频信道由 WiFi 决定
            std::snprintf(buf, sizeof(buf), "无同频设备 射频信道%d", lsnap.radio_channel);
            set_text(s_hint_lbl, buf);
        } else if (arm_pending) {
            set_text(s_hint_lbl, "下键+1 上键-1 切频道");
            set_color(s_hint_lbl, COL_AMBER);
        } else if (vol_pending) {
            set_text(s_hint_lbl, "再按一次OK键调音量");
            set_color(s_hint_lbl, COL_AMBER);
        } else {
            set_text(s_hint_lbl, "按住上键讲话");
        }
    } else if (s_cfg.host[0] == '\0') {
        set_text(s_hint_lbl, "还没设置服务器");
    } else if (!talkie::joined() && talkie::state() == talkie::State::kFailed) {
        set_text(s_hint_lbl, talkie::last_error());
    } else if (!talkie::joined()) {
        set_text(s_hint_lbl, "正在连接中转服务…");
    } else if (s_tx) {
        set_text(s_hint_lbl, "松手结束");
    } else if (talkie::remote_talking()) {
        set_text(s_hint_lbl, "正在收听");
    } else if (arm_pending) {
        set_text(s_hint_lbl, "下键+1 上键-1 切房间");
        set_color(s_hint_lbl, COL_AMBER);
    } else if (vol_pending) {
        set_text(s_hint_lbl, "再按一次OK键调音量");
        set_color(s_hint_lbl, COL_AMBER);
    } else {
        set_text(s_hint_lbl, "按住上键讲话");
    }

    // 波形：静止时压成一条线，有活动时按帧长起伏。
    // 波形数据源按模式分流：近场=ADPCM 帧载荷，异地=Opus 帧长。
    for (int i = WAVE_N - 1; i > 0; --i) s_wave_h[i] = s_wave_h[i - 1];
    const int last_len = local ? talkie_local_pop_wave() : s_last_len.exchange(0);
    uint8_t target = 4;
    if (last_len > 0) {
        // Opus 帧 100~500 字节映射到柱高 8~46
        int h = 8 + (last_len - 100) * 38 / 400;
        if (h < 8) h = 8;
        if (h > 46) h = 46;
        target = static_cast<uint8_t>(h);
    }
    s_wave_h[0] = target;
    constexpr int wave_cy = 92;      // 相对主卡(54)的中心线
    constexpr int wave_total = WAVE_N * 8 + (WAVE_N - 1) * 6;
    (void)wave_total;
    const uint32_t wc = (local ? (lsnap.tx != 0) : s_tx) ? COL_GREEN
                        : (local ? (lsnap.rx != 0) : s_playing) ? COL_CYAN : COL_EDGE;
    for (int i = 0; i < WAVE_N; ++i) {
        const int h = s_wave_h[i];
        lv_obj_set_y(s_wave[i], wave_cy - h / 2);
        lv_obj_set_height(s_wave[i], h);
        lv_obj_set_style_bg_color(s_wave[i], lv_color_hex(h > 6 ? wc : COL_DIM), 0);
    }

    // 仪表卡
    if (rssi > -127) {
        std::snprintf(buf, sizeof(buf), "%d dBm", rssi);
        const int pct = rssi <= -90 ? 5 : (rssi >= -50 ? 100 : (rssi + 90) * 100 / 40);
        set_text(s_sig_txt, buf);
        std::snprintf(buf, sizeof(buf), "%d%%", pct);
        set_text(s_sig_pct, buf);
        set_color(s_sig_pct, bars >= 2 ? COL_GREEN : COL_AMBER);
    } else {
        set_text(s_sig_txt, "-- dBm");
        set_text(s_sig_pct, "--");
        set_color(s_sig_pct, COL_MUTED);
    }

    std::snprintf(buf, sizeof(buf), "%d%%", s_volume);
    set_text(s_vol_txt, buf);
    const int lit = (s_volume * VOL_N + 99) / 100;
    for (int i = 0; i < VOL_N; ++i) {
        lv_obj_set_style_bg_color(s_vol_cell[i], lv_color_hex(i < lit ? COL_CYAN : COL_DIM), 0);
    }

    // PTT 按钮跟随实际状态
    const bool ptt_active = local ? (lsnap.tx != 0) : s_tx;
    const bool peer_talking = local ? (lsnap.rx != 0 || lsnap.busy != 0) : talkie::remote_talking();
    const bool connected = local ? true : talkie::joined();
    if (ptt_active) {
        lv_obj_set_style_bg_color(s_ptt, lv_color_hex(COL_GREEN), 0);
        set_text(s_ptt_lbl, "讲话中 · 松手结束");
        set_color(s_ptt_lbl, COL_BG);
    } else if (!connected) {
        lv_obj_set_style_bg_color(s_ptt, lv_color_hex(COL_DIM), 0);
        set_text(s_ptt_lbl, "等待连接");
        set_color(s_ptt_lbl, COL_MUTED);
    } else if (peer_talking) {
        lv_obj_set_style_bg_color(s_ptt, lv_color_hex(COL_DIM), 0);
        set_text(s_ptt_lbl, "对方讲话中");
        set_color(s_ptt_lbl, COL_CYAN);
    } else {
        lv_obj_set_style_bg_color(s_ptt, lv_color_hex(COL_DIM), 0);
        set_text(s_ptt_lbl, "按住上键讲话");
        set_color(s_ptt_lbl, COL_GREEN);
    }
}

void tick(lv_timer_t *t) {
    (void)t;
    if (!s_active.load()) return;
    render();
}

// ---------------------------------------------------------------- 音频

// 进入收听：只有真正要播才开通道，因为半双工下它会挡住麦克风
bool ensure_playback() {
    auto &audio = xiaozhi::AudioService::GetInstance();
    if (audio.IsPlaying()) {
        s_playing = true;
        return true;
    }
    if (!audio.StartPlayback()) {
        ESP_LOGW(TAG, "StartPlayback 失败 free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_playing = false;
        return false;
    }
    // StartPlayback 内部按小智的语义把音量打到 100，对讲机要跟随本机音量设置，
    // 所以启动成功后立刻覆盖回来（此时 codec 已开，设置立即生效）。
    bsp_audio_set_volume(static_cast<uint8_t>(s_volume));
    s_playing = true;
    return true;
}

void stop_playback() {
    if (!s_playing) return;
    auto &audio = xiaozhi::AudioService::GetInstance();
    audio.AbortPlayback();
    s_playing = false;
}

bool begin_tx() {
    auto &audio = xiaozhi::AudioService::GetInstance();
    // 半双工：先让出播放通道
    stop_playback();
    if (!audio.StartRecording()) {
        ESP_LOGW(TAG, "StartRecording 失败 free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    talkie::set_talking(true);
    ESP_LOGI(TAG, "开始讲话");
    return true;
}

void end_tx() {
    auto &audio = xiaozhi::AudioService::GetInstance();
    audio.StopRecording();
    talkie::set_talking(false);
    s_rx_last_us = 0;
    ESP_LOGI(TAG, "结束讲话");
}

// ---------------------------------------------------------------- 主任务

void app_task(void *arg) {
    (void)arg;
    uint8_t frame[512];
    auto &audio = xiaozhi::AudioService::GetInstance();

    while (s_active.load()) {
        // 0) 模式切换请求（长按下键）：异地 ⇄ 近场，双方资源互斥，逐套启停
        if (s_mode_switch_req.exchange(false)) {
            const bool to_local = !s_mode_local.load();
            s_want_tx.store(false);
            if (s_tx) {
                end_tx();
                s_tx = false;
            }
            stop_playback();
            bool ok = false;
            if (to_local) {
                remote_audio_stop();
                s_mode_local.store(true);
                mode_save(true);
                talkie_local_set_volume(s_volume);
                ok = talkie_local_start();
                if (!ok) {
                    ESP_LOGE(TAG, "近场模式启动失败，退回异地模式");
                    s_mode_local.store(false);
                    mode_save(false);
                    ok = remote_audio_start();
                }
            } else {
                talkie_local_stop();
                s_mode_local.store(false);
                mode_save(false);
                // 近场进场时没归还蓝牙栈(进场只在异地模式才归还,见 start()),
                // 运行时切异地必须在这里补一次:否则 NimBLE host 占着 ~64KB
                // 连续块,AudioService 的 Opus 工作区申请必 NO_MEM → 永远退回近场。
                if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 32 * 1024) {
                    ESP_LOGW(TAG, "切异地:连续内存不足(largest=%u)，归还蓝牙栈",
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                    wb_ble_guard_loan();
                }
                ok = remote_audio_start();
                if (!ok) {
                    ESP_LOGE(TAG, "异地模式音频启动失败，退回近场模式");
                    s_mode_local.store(true);
                    mode_save(true);
                    talkie_local_set_volume(s_volume);
                    talkie_local_start();
                    ok = true;
                }
            }
            ESP_LOGI(TAG, "模式切换完成(%s) ok=%d", to_local ? "近场" : "异地", (int)ok);
            if (bsp_lvgl_lock(500)) {
                render();
                bsp_lvgl_unlock();
            }
        }

        // 近场模式：引擎在 talkie_local 自有任务里跑，这里只喂波形数据
        if (s_mode_local.load()) {
            s_last_len.store(talkie_local_pop_wave());
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        // 1) 按键请求 → 录音状态切换（必须在本任务里做）
        const bool want = s_want_tx.load();
        if (want != s_tx) {
            if (want) {
                if (begin_tx()) {
                    s_tx = true;
                } else {
                    s_want_tx.store(false);   // 起不来就别一直重试
                }
            } else {
                end_tx();
                s_tx = false;
            }
        }

        // 2) 录音中：把编好的帧发出去
        if (s_tx) {
            size_t len = 0;
            int guard = 0;
            while (audio.PopEncodedFrame(frame, sizeof(frame), &len) && guard++ < 8) {
                s_last_len.store(len > 0 ? static_cast<int>(len) : 1);
                // 链路断了自己会重连，这里丢帧即可，不打断录音
                talkie::send_audio(frame, len);
            }
        }

        // 3) 收听的帧喂给播放器（说话时不收，避免把对方声音塞进半双工通道）
        size_t got = 0;
        while ((got = talkie::pop_audio(frame, sizeof(frame))) > 0) {
            if (s_tx) continue;
            if (ensure_playback()) {
                s_last_len.store(static_cast<int>(got));
                audio.PushIncomingAudio(frame, got);
                s_rx_last_us = esp_timer_get_time();
            }
        }

        // 4) 静默超时就关掉播放通道，把 codec 还回去
        if (s_playing && s_rx_last_us) {
            const int64_t idle_ms = (esp_timer_get_time() - s_rx_last_us) / 1000;
            if (idle_ms > RX_HOLD_MS && !audio.HasPendingPlayback()) {
                stop_playback();
                s_rx_last_us = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(12));
    }

    if (s_mode_local.load()) {
        talkie_local_stop();
    } else {
        if (s_tx) end_tx();
        stop_playback();
    }
    s_task = nullptr;
    ESP_LOGI(TAG, "任务退出");
    vTaskDelete(nullptr);
}

}  // namespace

// ---------------------------------------------------------------- 生命周期

extern "C" void talkie_app_init(void) {
    talkie::config_load(&s_cfg);
    ESP_LOGI(TAG, "init: server=%s:%u room=%s", s_cfg.host, s_cfg.port, s_cfg.room);
}

extern "C" void talkie_app_start(void) {
    if (s_active.load()) return;
    talkie::config_load(&s_cfg);
    mode_load();
    const bool local = s_mode_local.load();
    s_active.store(true);

    // 蓝牙协议栈是「一次引导、永不释放」的：只要进过 WorkBuddy，那约 60KB
    // 就一直占着。异地模式的 Opus 编码器需要连续内存，进场前先把这块要回来；
    // 近场模式用 ADPCM(纯整数运算)，不需要归还也能跑。
    // guard_loan = 释放 host + 交出内存锚(64KB 防碎片化锚块)。
    if (!local && heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 32 * 1024) {
        ESP_LOGW(TAG, "连续内存不足(largest=%u)，先归还蓝牙栈",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        wb_ble_guard_loan();
    }

    // 音频服务是全局独占的，而且必须抢在建 UI 之前。
    // 反过来做(先 build_ui 再 Init)时，LVGL 的一堆小对象把内部堆切碎，
    // Init 要的连续块就没了：实测建完 UI 后 largest 只剩 20KB，
    // 哪怕 free 还有 57KB，opus 编码器分配照样 ESP_ERR_NO_MEM。
    // 近场模式完全不碰 AudioService(直驱 bsp_audio)，把 ~40KB 连续堆留给系统。
    auto &audio = xiaozhi::AudioService::GetInstance();
    const esp_err_t err = local ? ESP_OK : audio.Init(audio_workspace::Owner::Talkie);
    if (!local && err != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s free=%u largest=%u", esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else if (!local) {
        // 小智是"服务端 hello 之后"才准备会话缓冲的，对讲机没有那个阶段，
        // 必须在这里自己调 —— 少了它 play_ring/tx_ring 是空的，
        // StartPlayback/StartRecording 会静默返回 false。
        // 对讲机上下行都是 16kHz Opus，设备播放也保持 16kHz。
        // 曾试过把【解码/播放】速率设成 24kHz 想走"原生干净链路"，但本板 ES8311(I2S
        // slave, MCLK 由 ESP32 提供)在 24k 下 bsp_audio_set_format 配置失败 → InitPlayback
        // 直接返回 false → 播放任务不启动 → 设备彻底无声。故回归 16k(能稳定出声)。
        // 注意 16k 直放偏"尖/薄"主要是设备小喇叭的硬件频响特性(高频多、低频少)，
        // 不是码流或采样率问题；要从听感上改善需用设备端 EQ(低频提升+高频衰减)，另行处理。
        // (设备→手机那路不受此影响：它只编码 16k 发出去，由手机端播放。)
        audio.SetPlaybackSampleRate(16000);
        if (!audio.PrepareSessionResources()) {
            ESP_LOGE(TAG, "音频会话缓冲准备失败 free=%u largest=%u",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        s_audio_inited = true;
    }
    bsp_audio_set_volume(static_cast<uint8_t>(s_volume));

    // 音频抢到手之后才建界面
    if (!bsp_lvgl_lock(1000)) {
        if (err == ESP_OK) audio.Deinit();
        s_active.store(false);
        return;
    }
    if (!s_screen) {
        s_screen = lv_obj_create(nullptr);
        lv_obj_remove_style_all(s_screen);
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        lv_obj_set_size(s_screen, SCR_W, SCR_H);
        build_ui(s_screen);
    }
    lv_screen_load(s_screen);
    if (!s_timer) s_timer = lv_timer_create(tick, 120, nullptr);
    bsp_lvgl_unlock();
    render();

    // 起网络/引擎（按模式分流）
    if (local) {
        talkie_local_set_volume(s_volume);
        if (!talkie_local_start()) {
            ESP_LOGE(TAG, "近场模式启动失败(ESP-NOW)");
        }
    } else {
        if (!WifiManager::GetInstance().IsConnected()) {
            ESP_LOGI(TAG, "Wi-Fi 未连接，先拉起 STA");
            WifiManager::GetInstance().StartStation();
        }
        talkie::start();
    }

    // PTT 是「按下即生效」的交互：屏幕熄着时第一下按键不能只用来亮屏，
    // 否则用户按下去没反应、松手也没反应，会以为按键坏了。
    app_registry_set_wake_key_passthrough(true);

    xTaskCreate(app_task, "talkie_app", 6144, nullptr, 6, &s_task);
    ESP_LOGI(TAG, "start done free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

extern "C" void talkie_app_stop(void) {
    if (!s_active.exchange(false)) return;
    s_want_tx.store(false);

    for (int i = 0; i < 60 && s_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_mode_local.load()) {
        talkie_local_stop();
    } else {
        talkie::stop();
    }

    if (s_timer) {
        if (bsp_lvgl_lock(500)) {
            lv_timer_del(s_timer);
            s_timer = nullptr;
            bsp_lvgl_unlock();
        }
    }
    app_registry_set_wake_key_passthrough(false);
    // 释放音频工作区，别的应用(小智)才能拿到
    if (s_audio_inited) {
        xiaozhi::AudioService::GetInstance().Deinit();
        s_audio_inited = false;
    }
    s_tx = false;
    s_playing = false;
    ESP_LOGI(TAG, "stop done free=%u", (unsigned)esp_get_free_heap_size());
}

extern "C" bool talkie_app_prepare_stop(void) {
    // 没有不可中断的写盘/上传，直接允许切换
    return true;
}

extern "C" void talkie_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (!s_active.load()) return;
    const bool local = s_mode_local.load();

    // 下键待命窗口（切频道/房间流程中）：下键=+1，上键=-1。
    // 窗口内 PTT 暂被屏蔽（否则快速点按上键会误发一次发言申请）。
    const int64_t now_us = esp_timer_get_time();
    const int64_t arm_at = s_switch_arm_us.load();
    const bool chan_arm = arm_at != 0 && now_us - arm_at < SWITCH_ARM_US;

    if (btn == BSP_BTN_UP) {
        if (local) {
            // 待命窗口内上键单击 = 频道 -1
            if (event == BTN_EVT_CLICK && chan_arm) {
                s_switch_arm_us.store(0);
                talkie_local_shift_channel(-1);
                ESP_LOGI(TAG, "上键：频道 -1（空闲时生效）");
                return;
            }
            // 近场 PTT：多收 LONG/LONG_UP 两个事件，兼容按键组件长按态不报 RELEASE 的情况
            if (event == BTN_EVT_PRESS || event == BTN_EVT_LONG) {
                if (!chan_arm) talkie_local_set_ptt(true);
            } else if (event == BTN_EVT_RELEASE || event == BTN_EVT_LONG_UP) {
                talkie_local_set_ptt(false);
            }
            return;
        }
        // 待命窗口内上键单击 = 房间号 -1
        if (event == BTN_EVT_CLICK && chan_arm) {
            s_switch_arm_us.store(0);
            char next[8];
            const int v = (atoi(s_cfg.room) + 9999) % 10000;
            std::snprintf(next, sizeof(next), "%04d", v);
            talkie::config_set_room(next);
            talkie::config_load(&s_cfg);
            talkie::stop();
            talkie::start();
            ESP_LOGI(TAG, "上键：房间切到 %s", s_cfg.room);
            return;
        }
        if (event == BTN_EVT_PRESS) {
            // 对方正在讲话时不抢话，跟真对讲机一样
            if (!chan_arm && talkie::joined() && !talkie::remote_talking()) s_want_tx.store(true);
            return;
        }
        if (event == BTN_EVT_RELEASE) {
            s_want_tx.store(false);
            return;
        }
    }

    if (event == BTN_EVT_CLICK) {
        if (btn == BSP_BTN_DOWN) {
            // 下键：切信道/频道。两段式 —— 第一下只待命，2 秒内再按一下才真生效。
            // 原因见 SWITCH_ARM_US 注释：单击就切会让握持时的无意蹭碰直接掉线。
            const int64_t now_us = esp_timer_get_time();
            const int64_t arm_at = s_switch_arm_us.load();
            if (arm_at != 0 && now_us - arm_at < SWITCH_ARM_US) {
                s_switch_arm_us.store(0);
                if (local) {
                    // 近场：CH1..CH4 循环（写入 NVS，空闲时生效）
                    talkie_local_cycle_channel();
                    ESP_LOGI(TAG, "双击切频道（空闲时生效）");
                } else {
                    // 异地：切服务器房间号并重连
                    char next[8];
                    int v = atoi(s_cfg.room);
                    v = (v + 1) % 10000;
                    std::snprintf(next, sizeof(next), "%04d", v);
                    talkie::config_set_room(next);
                    talkie::config_load(&s_cfg);
                    talkie::stop();
                    talkie::start();
                    ESP_LOGI(TAG, "双击切到信道 %s", s_cfg.room);
                }
            } else {
                s_switch_arm_us.store(now_us);
                ESP_LOGI(TAG, "下键待命：2 秒内再按一次切%s", local ? "频道" : "信道");
            }
        } else if (btn == BSP_BTN_OK) {
            // OK 键：双击调音量。单击误触率高（握持蹭碰就变音量），
            // 两段式：第一下只待命，2 秒内再按一下才生效。
            const int64_t vol_at = s_vol_arm_us.load();
            if (vol_at != 0 && now_us - vol_at < SWITCH_ARM_US) {
                s_vol_arm_us.store(0);
                s_volume = s_volume >= 100 ? 30 : (s_volume >= 90 ? 100 : s_volume + 20);
                bsp_audio_set_volume(static_cast<uint8_t>(s_volume));
                if (local) talkie_local_set_volume(s_volume);
                ESP_LOGI(TAG, "双击调音量 %d%%", s_volume);
            } else {
                s_vol_arm_us.store(now_us);
                ESP_LOGI(TAG, "OK 键待命：2 秒内再按一次调音量");
            }
        }
        return;
    }

    // 长按下键：切换 近场(ESP-NOW) ⇄ 异地(服务器中转)，由 app_task 异步执行
    if (event == BTN_EVT_LONG && btn == BSP_BTN_DOWN) {
        s_switch_arm_us.store(0);
        s_vol_arm_us.store(0);
        s_mode_switch_req.store(true);
        ESP_LOGI(TAG, "长按下键：请求切换到%s模式", local ? "异地" : "近场");
    }
}

extern "C" const passport_app_t g_talkie_app = {
    .id = APP_ID_TALKIE,
    .name = "对讲机",
    .en_name = "TALKIE",
    .desc = "近场ESP-NOW直连\n异地服务器中转",
    .tag = "直连",
    .theme_color = 0x39D98A,
    .init = talkie_app_init,
    .start = talkie_app_start,
    .stop = talkie_app_stop,
    .on_key = talkie_app_on_key,
    .prepare_stop = talkie_app_prepare_stop,
};
