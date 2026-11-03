// apps/atc/app_atc.cc —— 网络航空波段收音机。
// 复用收音机的流媒体引擎(radio_player):MP3 直播流 + 音谱电平。
// 交互:上下键切换频道,OK 播放/停止,长按 OK 进入音量调节。
// 声音来自公开航空语音网络直播,本机不接收无线电信号。
#include "apps/app_base.h"
#include "apps/atc/atc_catalog.h"
#include "apps/radio/radio_player.h"
#include "launcher/app_registry.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

static const char *TAG = "app_atc";

namespace {

constexpr int kMeterCount = 24;
constexpr uint32_t ATC_BG = 0x0B1018;
constexpr uint32_t ATC_CYAN = 0x67D8FF;
constexpr uint32_t ATC_GREEN = 0x78E0A4;
constexpr uint32_t ATC_ORANGE = 0xFFB454;
constexpr uint32_t ATC_MUTED = 0x7F8B9E;
constexpr uint32_t ATC_RED = 0xFF7180;

lv_obj_t *s_screen;
lv_timer_t *s_timer;
lv_obj_t *s_channel_name, *s_channel_detail, *s_state, *s_meter_caption;
lv_obj_t *s_bars[kMeterCount];
lv_obj_t *s_wifi_bars[3] = {nullptr, nullptr, nullptr};
ui_pixel_battery_t s_battery{};

std::atomic<bool> s_want_play{false};
bool s_player_ready = false;   // radio_player_init 成功与否(决定 stop 是否 deinit)
bool s_volume_mode = false;
uint8_t s_volume_backup = 0;
uint8_t s_volume_mode_value = 0;
int64_t s_state_hint_until_us = 0;

portMUX_TYPE s_level_lock = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_levels[kMeterCount] = {};
uint32_t s_level_generation = 0;

void level_sink(const uint8_t *levels, std::size_t count) {
    if (!levels) return;
    taskENTER_CRITICAL(&s_level_lock);
    for (int i = 0; i < kMeterCount; ++i) {
        s_levels[i] = levels[i * static_cast<int>(count) / kMeterCount];
    }
    ++s_level_generation;
    taskEXIT_CRITICAL(&s_level_lock);
}

lv_obj_t *make_label(const char *text, const lv_font_t *font, uint32_t color,
                     int x, int y, int w, int h, lv_align_t align = LV_ALIGN_TOP_LEFT,
                     int bx = 0, int by = 0) {
    lv_obj_t *o = lv_label_create(s_screen);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    if (align == LV_ALIGN_TOP_LEFT) {
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, w, h);
        lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    } else {
        lv_obj_align_to(o, nullptr, align, bx, by);
    }
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    return o;
}

void set_text(lv_obj_t *obj, const char *text) {
    if (obj && std::strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
}

void set_state_text(const char *text, uint32_t color, int64_t hold_ms = 0) {
    set_text(s_state, text);
    lv_obj_set_style_text_color(s_state, lv_color_hex(color), 0);
    if (hold_ms) s_state_hint_until_us = esp_timer_get_time() + hold_ms * 1000;
}

// radio_player_station() 按值返回,直接返回 st.name 会指向已销毁的栈帧
// (悬垂指针:LVGL 去渲染那串地址上的垃圾数据)。这里拷进静态缓冲。
char s_name_buf[64] = {};
char s_detail_buf[96] = {};

const char *current_name() {
    const RadioStation st = radio_player_station(radio_player_station_index());
    std::snprintf(s_name_buf, sizeof(s_name_buf), "%s", st.name);
    return s_name_buf;
}

const char *current_detail() {
    const RadioStation st = radio_player_station(radio_player_station_index());
    std::snprintf(s_detail_buf, sizeof(s_detail_buf), "%s", st.description);
    return s_detail_buf;
}

void refresh_channel() {
    set_text(s_channel_name, current_name());
    set_text(s_channel_detail, current_detail());
    char index[24];
    std::snprintf(index, sizeof(index), "%u / %u",
                  static_cast<unsigned>(radio_player_station_index() + 1),
                  static_cast<unsigned>(radio_player_station_count()));
    set_text(s_meter_caption, index);
}

void refresh_state() {
    if (esp_timer_get_time() < s_state_hint_until_us) return;
    if (s_volume_mode) {
        set_state_text("音量调节", ATC_CYAN);
        return;
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        set_state_text("需要 Wi-Fi", ATC_RED);
        return;
    }
    set_state_text(s_want_play.load() ? "接收中" : "待机",
                   s_want_play.load() ? ATC_GREEN : ATC_MUTED);
}

void refresh_meter() {
    uint8_t levels[kMeterCount];
    uint32_t gen;
    taskENTER_CRITICAL(&s_level_lock);
    std::memcpy(levels, s_levels, sizeof(levels));
    gen = s_level_generation;
    taskEXIT_CRITICAL(&s_level_lock);
    const bool live = s_want_play.load() && gen != 0;
    for (int i = 0; i < kMeterCount; ++i) {
        const int h = live ? 4 + levels[i] * 76 / 100 : 4;
        lv_obj_set_height(s_bars[i], h);
        lv_obj_set_y(s_bars[i], 226 - h);
        lv_obj_set_style_bg_color(s_bars[i],
            lv_color_hex(live ? ATC_GREEN : ATC_MUTED), 0);
    }
}

void tick(lv_timer_t *timer) {
    (void)timer;
    if (!s_screen) return;
    const bool visible = app_registry_screen_is_on();
    lv_timer_set_period(timer, visible ? 100 : 1000);
    if (!visible) return;
    refresh_state();
    refresh_meter();
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    const bool wifi_ok = WifiManager::GetInstance().IsConnected();
    int bars = 0;
    uint32_t wifi_col = ATC_MUTED;
    if (wifi_ok) {
        const int rssi = WifiManager::GetInstance().GetRssi();
        bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
        wifi_col = ATC_CYAN;
    }
    ui_pixel_wifi_bars_set(s_wifi_bars, bars, wifi_col);
}

void init() {}

void start() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) { bsp_lvgl_unlock(); return; }
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(ATC_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_size(s_screen, BSP_LCD_W, BSP_LCD_H);

    lv_obj_t *top = lv_obj_create(s_screen);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, BSP_LCD_W, 36);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "AIRBAND");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(ATC_CYAN), 0);
    lv_obj_set_pos(title, 12, 10);
    ui_pixel_wifi_bars_create(top, BSP_LCD_W - 60, 13, s_wifi_bars);
    s_battery = ui_pixel_battery_create(top, BSP_LCD_W - 46, 8, ATC_MUTED, &lv_font_montserrat_14);

    s_channel_name = make_label("", &buddy_font_16, 0xF6F8FC, 12, 64, 216, 48);
    s_channel_detail = make_label("", &lv_font_montserrat_14, ATC_MUTED, 12, 116, 216, 22);
    s_state = make_label("待机", &buddy_font_16, ATC_MUTED, 12, 148, 216, 30);
    s_meter_caption = make_label("", &lv_font_montserrat_14, ATC_MUTED, 12, 184, 216, 18);

    // 音谱条:24 根,底部对齐 y=226。
    const int bar_w = 5, gap = 4;
    const int total = kMeterCount * bar_w + (kMeterCount - 1) * gap;
    const int x0 = (BSP_LCD_W - total) / 2;
    for (int i = 0; i < kMeterCount; ++i) {
        lv_obj_t *b = lv_obj_create(s_screen);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, bar_w, 4);
        lv_obj_set_pos(b, x0 + i * (bar_w + gap), 222);
        lv_obj_set_style_bg_color(b, lv_color_hex(ATC_MUTED), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 2, 0);
        s_bars[i] = b;
    }

    ui_pixel_footer(s_screen, "确定 播放/停止", "上下 换台 · 长按确定 音量", &buddy_font_16);
    lv_screen_load(s_screen);

    s_want_play = false;
    s_volume_mode = false;
    s_player_ready = false;
    refresh_state();
    s_timer = lv_timer_create(tick, 100, nullptr);
    bsp_lvgl_unlock();

    // 先初始化播放器(它会把声道目录重置为内置预设),再刷频道信息,否则
    // refresh_channel() 读到的是未初始化的空目录。
    ESP_LOGI(TAG, "radio_player_init: free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!radio_player_init()) {
        ESP_LOGE(TAG, "radio_player_init 失败");
        if (bsp_lvgl_lock(500)) {
            set_state_text("音频初始化失败", ATC_RED, 8000);
            bsp_lvgl_unlock();
        }
        return;
    }
    s_player_ready = true;
    radio_player_set_level_sink(level_sink);
    std::vector<RadioStation> stations;
    stations.reserve(kAtcChannelCount);
    for (std::size_t i = 0; i < kAtcChannelCount; ++i) {
        RadioStation st{};
        std::snprintf(st.name, sizeof(st.name), "%s", kAtcChannels[i].name);
        std::snprintf(st.description, sizeof(st.description), "%s", kAtcChannels[i].detail);
        std::snprintf(st.url, sizeof(st.url), "%s", kAtcChannels[i].url);
        stations.push_back(st);
    }
    radio_player_replace_stations(stations.data(), stations.size());
    radio_player_set_network(WifiManager::GetInstance().IsConnected());
    if (bsp_lvgl_lock(300)) { refresh_channel(); bsp_lvgl_unlock(); }
}

bool prepare_stop() {
    s_want_play = false;
    if (!s_player_ready) return true;
    if (radio_player_is_playing()) radio_player_toggle();
    return radio_player_wait_idle(3000);
}

void stop() {
    radio_player_set_level_sink(nullptr); // 恢复默认(收音机 UI)
    if (s_player_ready) {
        radio_player_deinit();
        s_player_ready = false;
    }
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    for (auto &b : s_bars) b = nullptr;
    s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
    s_channel_name = s_channel_detail = s_state = s_meter_caption = nullptr;
    s_battery = {};
    bsp_lvgl_unlock();
}

void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (event == BTN_EVT_CLICK) {
        if (s_volume_mode) {
            if (btn == BSP_BTN_OK) {
                s_volume_mode = false;
                radio_player_set_volume(s_volume_mode_value);
            } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                const int delta = btn == BSP_BTN_UP ? 5 : -5;
                int next = static_cast<int>(s_volume_mode_value) + delta;
                next = next < 0 ? 0 : (next > 100 ? 100 : next);
                s_volume_mode_value = static_cast<uint8_t>(next);
                radio_player_set_volume(s_volume_mode_value);
                char text[32];
                std::snprintf(text, sizeof(text), "音量 %u", s_volume_mode_value);
                set_state_text(text, ATC_CYAN, 0);
            }
        } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            radio_player_select_relative(btn == BSP_BTN_UP ? -1 : +1);
            refresh_channel();
        } else if (btn == BSP_BTN_OK) {
            if (!WifiManager::GetInstance().IsConnected()) {
                set_state_text("先在设置里连 Wi-Fi", ATC_RED, 4000);
            } else {
                s_want_play.store(!s_want_play.load());
                radio_player_toggle();
                refresh_state();
            }
        }
    } else if (event == BTN_EVT_LONG && btn == BSP_BTN_OK && !s_volume_mode) {
        s_volume_mode = true;
        s_volume_mode_value = radio_player_volume();
        char text[32];
        std::snprintf(text, sizeof(text), "音量 %u", s_volume_mode_value);
        set_state_text(text, ATC_CYAN, 0);
    }
    bsp_lvgl_unlock();
}
} // namespace

extern const passport_app_t g_atc_app = {
    .id = APP_ID_ATC,
    .name = "航空波段收音机",
    .en_name = "AIRBAND",
    .desc = "收听全球机场塔台实况\n网络直播,需 Wi-Fi",
    .tag = "Wi-Fi",
    .theme_color = 0x67D8FF,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .prepare_stop = prepare_stop,
};
