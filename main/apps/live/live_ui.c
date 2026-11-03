// live_ui.c —— 直播助手界面实现（移植自 AI Passport app_ui.c）。
#include "live_ui.h"

#include "app_fonts.h"
#include "live_ui_strings.h"
#include "live_text.h"

#include "bsp_battery.h"
#include "bsp_display.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// 配色：深色底 + 抖音式的红/青强调色。240x320 的小屏上只用两级灰度做层级，
// 避免细线在 IPS 面板上发灰。
#define COL_BG     0x0B0F14
#define COL_PANEL  0x161C24
#define COL_PANEL2 0x1E2631
#define COL_LINE   0x2A3441
#define COL_TEXT   0xE9EEF5
#define COL_MUTED  0x8A97A8
#define COL_ACCENT 0xFF2D55
#define COL_CYAN   0x25F4EE
#define COL_GOLD   0xFFC53D
#define COL_GREEN  0x3FB950

#define DANMAKU_ROWS 4
#define SETTINGS_ACTIONS 3

static lv_obj_t *s_dash;
static lv_obj_t *s_dot;
static lv_obj_t *s_state_label;
static lv_obj_t *s_net_label;
static lv_obj_t *s_batt_label;
static lv_obj_t *s_room_label;
static lv_obj_t *s_banner;
static lv_obj_t *s_banner_tag;
static lv_obj_t *s_banner_text;
static lv_obj_t *s_rows[DANMAKU_ROWS];
static lv_obj_t *s_bars[DANMAKU_ROWS];
static lv_obj_t *s_counter_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_toast;
static lv_obj_t *s_toast_label;
static lv_timer_t *s_toast_timer;

static lv_obj_t *s_settings;
static lv_obj_t *s_settings_action_rows[SETTINGS_ACTIONS];
static lv_obj_t *s_settings_info;

static app_ui_state_t s_state = APP_UI_BOOT;
static bool s_muted;
static uint32_t s_likes;
static uint32_t s_gifts;
static uint32_t s_follows;
static uint32_t s_online;

static bool ui_lock(int timeout_ms)
{
    return bsp_lvgl_lock(timeout_ms);
}

static void ui_unlock(void)
{
    bsp_lvgl_unlock();
}

static const char *safe(const char *src, char *dst, size_t cap)
{
    // 不做字库覆盖过滤：passport-os 的 buddy_font_16 覆盖广，缺字交给 LVGL。
    app_text_sanitize(src, dst, cap, NULL, NULL, NULL);
    return dst;
}

// 原项目的 ui_font_title()/ui_font_body()：对应 passport-os 的 20px/16px 中文字库。
static const lv_font_t *ui_font_title(void) { return &ui_font_20; }
static const lv_font_t *ui_font_body(void) { return &buddy_font_16; }

static void style_panel(lv_obj_t *obj, uint32_t color, int radius)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    return screen;
}

static void toast_hide(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
}

static void build_dashboard(void)
{
    s_dash = make_screen();

    // 顶部：状态点 + 应用名 + 网络 + 电量（电量放右上角，不遮挡内容）
    s_dot = lv_obj_create(s_dash);
    lv_obj_set_size(s_dot, 8, 8);
    style_panel(s_dot, COL_MUTED, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_dot, 10, 12);

    s_state_label = make_label(s_dash, ui_font_title(), COL_TEXT);
    lv_label_set_text(s_state_label, LV_TXT_APP_TITLE);
    lv_obj_set_pos(s_state_label, 24, 4);

    s_batt_label = make_label(s_dash, ui_font_body(), COL_MUTED);
    lv_label_set_text(s_batt_label, LV_TXT_BATTERY_UNKNOWN);
    lv_obj_set_width(s_batt_label, 52);
    lv_label_set_long_mode(s_batt_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_batt_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_batt_label, 178, 5);

    s_net_label = make_label(s_dash, ui_font_body(), COL_MUTED);
    lv_obj_set_width(s_net_label, 226);
    lv_label_set_long_mode(s_net_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_net_label, LV_TXT_STATE_CONNECTING);
    lv_obj_set_pos(s_net_label, 10, 30);

    // 房间名
    s_room_label = make_label(s_dash, ui_font_body(), COL_CYAN);
    lv_obj_set_width(s_room_label, 220);
    lv_label_set_long_mode(s_room_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_room_label, LV_TXT_ROOM_UNKNOWN);
    lv_obj_set_pos(s_room_label, 10, 52);

    // "正在说"横幅
    s_banner = lv_obj_create(s_dash);
    lv_obj_set_size(s_banner, 220, 62);
    style_panel(s_banner, COL_PANEL, 10);
    lv_obj_set_pos(s_banner, 10, 76);
    lv_obj_set_style_border_side(s_banner, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(s_banner, 3, 0);
    lv_obj_set_style_border_color(s_banner, lv_color_hex(COL_ACCENT), 0);

    s_banner_tag = make_label(s_banner, ui_font_body(), COL_ACCENT);
    lv_label_set_text(s_banner_tag, LV_TXT_SPEAKING_TAG);
    lv_obj_set_pos(s_banner_tag, 10, 4);

    s_banner_text = make_label(s_banner, ui_font_body(), COL_TEXT);
    lv_obj_set_width(s_banner_text, 196);
    lv_label_set_long_mode(s_banner_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_banner_text, LV_TXT_IDLE);
    lv_obj_set_pos(s_banner_text, 10, 24);

    // 弹幕列表
    for (int i = 0; i < DANMAKU_ROWS; i++) {
        const int y = 146 + i * 30;
        lv_obj_t *row = lv_obj_create(s_dash);
        lv_obj_set_size(row, 220, 26);
        style_panel(row, COL_PANEL, 6);
        lv_obj_set_pos(row, 10, y);

        s_bars[i] = lv_obj_create(row);
        lv_obj_set_size(s_bars[i], 3, 18);
        style_panel(s_bars[i], COL_LINE, 2);
        lv_obj_set_pos(s_bars[i], 6, 4);

        s_rows[i] = make_label(row, ui_font_body(), COL_TEXT);
        lv_obj_set_width(s_rows[i], 198);
        lv_label_set_long_mode(s_rows[i], LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_rows[i], 14, 3);
    }

    // 互动计数
    s_counter_label = make_label(s_dash, ui_font_body(), COL_MUTED);
    lv_obj_set_width(s_counter_label, 220);
    lv_label_set_long_mode(s_counter_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_counter_label, 10, 268);

    // 底部提示
    s_hint_label = make_label(s_dash, ui_font_body(), COL_LINE);
    lv_obj_set_width(s_hint_label, 220);
    lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_hint_label, LV_TXT_HINT_MAIN);
    lv_obj_set_pos(s_hint_label, 10, 296);

    // 浮层提示
    s_toast = lv_obj_create(s_dash);
    lv_obj_set_size(s_toast, 168, 34);
    style_panel(s_toast, COL_PANEL2, 8);
    lv_obj_set_pos(s_toast, 36, 216);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    s_toast_label = make_label(s_toast, ui_font_body(), COL_TEXT);
    lv_obj_set_width(s_toast_label, 152);
    lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_toast_label, 8, 6);

    lv_screen_load(s_dash);
}

void app_ui_init(void)
{
    if (!ui_lock(200)) return;
    build_dashboard();
    ui_unlock();
}

void app_ui_deinit(void)
{
    if (!ui_lock(-1)) return;
    // toast 定时器与设置页先于主屏删除；主屏一删，仪表盘全部子对象随之消失。
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_settings) {
        lv_obj_delete(s_settings);
        s_settings = NULL;
        s_settings_info = NULL;
        memset(s_settings_action_rows, 0, sizeof(s_settings_action_rows));
    }
    if (s_dash) {
        lv_obj_delete(s_dash);
        s_dash = NULL;
    }
    s_dot = s_state_label = s_net_label = s_batt_label = s_room_label = NULL;
    s_banner = s_banner_tag = s_banner_text = NULL;
    s_counter_label = s_hint_label = NULL;
    s_toast = s_toast_label = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_bars, 0, sizeof(s_bars));
    ui_unlock();
}

void app_ui_set_state(app_ui_state_t state)
{
    s_state = state;
    if (!ui_lock(200)) return;
    if (s_state_label) {
        const char *text = LV_TXT_STATE_CONNECTING;
        uint32_t color = COL_MUTED;
        switch (state) {
        case APP_UI_LIVE: text = LV_TXT_STATE_ONLINE; color = COL_GREEN; break;
        case APP_UI_OFFLINE: text = LV_TXT_STATE_OFFLINE; color = COL_ACCENT; break;
        case APP_UI_CONNECTING: text = LV_TXT_STATE_CONNECTING; color = COL_GOLD; break;
        case APP_UI_BOOT:
        default: text = LV_TXT_STATE_CONNECTING; color = COL_MUTED; break;
        }
        style_panel(s_dot, color, LV_RADIUS_CIRCLE);
        lv_label_set_text(s_net_label, text);
    }
    ui_unlock();
}

void app_ui_set_room(const char *room)
{
    if (room == NULL || room[0] == '\0') return;
    char clean[APP_TEXT_MAX];
    if (!ui_lock(200)) return;
    if (s_room_label) {
        lv_label_set_text(s_room_label, safe(room, clean, sizeof(clean)));
    }
    ui_unlock();
}

void app_ui_set_network(const char *text)
{
    char clean[64];
    if (text == NULL) return;
    if (!ui_lock(200)) return;
    if (s_net_label) {
        lv_label_set_text(s_net_label, safe(text, clean, sizeof(clean)));
    }
    ui_unlock();
}

void app_ui_set_battery(int percent)
{
    char buffer[24];
    if (!ui_lock(200)) return;
    if (s_batt_label == NULL) {
        ui_unlock();
        return;
    }
    if (percent < 0) {
        lv_label_set_text(s_batt_label, LV_TXT_BATTERY_UNKNOWN);
    } else {
        snprintf(buffer, sizeof(buffer), "%d%%", percent);
        lv_label_set_text(s_batt_label, buffer);
    }
    ui_unlock();
}

void app_ui_set_muted(bool muted)
{
    s_muted = muted;
    if (!ui_lock(200)) return;
    if (s_hint_label) {
        lv_label_set_text(s_hint_label, muted ? LV_TXT_TOAST_MUTED : LV_TXT_HINT_MAIN);
        lv_obj_set_style_text_color(s_hint_label,
                                    lv_color_hex(muted ? COL_ACCENT : COL_LINE), 0);
    }
    ui_unlock();
}

void app_ui_set_counters(uint32_t online, uint32_t likes, uint32_t gifts, uint32_t follows)
{
    s_online = online;
    s_likes = likes;
    s_gifts = gifts;
    s_follows = follows;

    char buffer[112];
    (void)online;   // 在线人数依赖桥接端采集，取不到时不显示，避免长期显示 0
    if (!ui_lock(200)) return;
    if (s_counter_label) {
        snprintf(buffer, sizeof(buffer), "%s %lu  ·  %s %lu  ·  %s %lu",
                 LV_TXT_LABEL_LIKE, (unsigned long)likes,
                 LV_TXT_LABEL_GIFT, (unsigned long)gifts,
                 LV_TXT_LABEL_FOLLOW, (unsigned long)follows);
        lv_label_set_text(s_counter_label, buffer);
    }
    ui_unlock();
}

static uint32_t kind_color(app_kind_t kind)
{
    switch (kind) {
    case APP_KIND_GIFT: return COL_ACCENT;
    case APP_KIND_ENTER: return COL_CYAN;
    case APP_KIND_FOLLOW: return COL_GOLD;
    case APP_KIND_LIKE: return COL_GREEN;
    default: return COL_LINE;
    }
}

void app_ui_push_event(app_kind_t kind, const char *user, const char *text)
{
    char user_clean[APP_USER_MAX];
    char text_clean[APP_TEXT_MAX];
    char line[APP_USER_MAX + APP_TEXT_MAX + 32];

    safe(user ? user : "", user_clean, sizeof(user_clean));
    safe(text ? text : "", text_clean, sizeof(text_clean));

    const char *label = NULL;
    switch (kind) {
    case APP_KIND_ENTER: label = LV_TXT_EVENT_ENTER; break;
    case APP_KIND_GIFT: label = LV_TXT_EVENT_GIFT; break;
    case APP_KIND_FOLLOW: label = LV_TXT_EVENT_FOLLOW; break;
    case APP_KIND_LIKE: label = LV_TXT_EVENT_LIKE; break;
    default: label = NULL; break;
    }

    if (label) {
        snprintf(line, sizeof(line), "%s %s%s%s", user_clean, label,
                 text_clean[0] ? " " : "", text_clean);
    } else {
        snprintf(line, sizeof(line), "%s：%s", user_clean, text_clean);
    }

    if (!ui_lock(200)) return;
    if (s_rows[0] == NULL) {
        ui_unlock();
        return;
    }
    // 向上滚动：最旧的一行被覆盖，最新的一行落在底部。
    for (int i = 0; i < DANMAKU_ROWS - 1; i++) {
        lv_label_set_text(s_rows[i], lv_label_get_text(s_rows[i + 1]));
        lv_obj_set_style_bg_color(s_bars[i],
                                  lv_obj_get_style_bg_color(s_bars[i + 1], 0), 0);
    }
    lv_label_set_text(s_rows[DANMAKU_ROWS - 1], line);
    style_panel(s_bars[DANMAKU_ROWS - 1], kind_color(kind), 2);
    ui_unlock();
}

void app_ui_show_speech(app_kind_t kind, const char *user, const char *text)
{
    char user_clean[APP_USER_MAX];
    char text_clean[APP_TEXT_MAX];
    safe(user ? user : "", user_clean, sizeof(user_clean));
    safe(text ? text : "", text_clean, sizeof(text_clean));

    if (!ui_lock(200)) return;
    if (s_banner_text) {
        if (user_clean[0]) {
            lv_label_set_text_fmt(s_banner_text, "%s：%s", user_clean, text_clean);
        } else {
            lv_label_set_text(s_banner_text, text_clean);
        }
        lv_obj_set_style_border_color(s_banner, lv_color_hex(kind_color(kind)), 0);
    }
    ui_unlock();
}

void app_ui_clear_speech(void)
{
    if (!ui_lock(200)) return;
    if (s_banner_text) lv_label_set_text(s_banner_text, LV_TXT_IDLE);
    ui_unlock();
}

void app_ui_toast(const char *text)
{
    char clean[48];
    if (text == NULL) return;
    safe(text, clean, sizeof(clean));

    if (!ui_lock(200)) return;
    if (s_toast && s_toast_label) {
        lv_label_set_text(s_toast_label, clean);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        if (s_toast_timer) {
            lv_timer_reset(s_toast_timer);
        } else {
            s_toast_timer = lv_timer_create(toast_hide, 1400, NULL);
        }
    }
    ui_unlock();
}

void app_ui_show_settings(const app_ui_settings_t *settings)
{
    if (settings == NULL) return;
    if (!ui_lock(200)) return;

    if (s_settings == NULL) {
        s_settings = make_screen();

        lv_obj_t *title = make_label(s_settings, ui_font_title(), COL_TEXT);
        lv_label_set_text(title, LV_TXT_SETTINGS_TITLE);
        lv_obj_set_pos(title, 12, 10);

        for (int i = 0; i < SETTINGS_ACTIONS; i++) {
            lv_obj_t *row = lv_obj_create(s_settings);
            lv_obj_set_size(row, 216, 34);
            style_panel(row, COL_PANEL, 8);
            lv_obj_set_pos(row, 12, 168 + i * 40);

            lv_obj_t *label = make_label(row, ui_font_body(), COL_TEXT);
            lv_obj_set_pos(label, 12, 6);
            s_settings_action_rows[i] = row;
            lv_obj_set_user_data(row, label);
        }
    }

    const char *actions[SETTINGS_ACTIONS] = {
        LV_TXT_ACTION_RECONNECT,
        LV_TXT_ACTION_MUTE,
        LV_TXT_ACTION_BACK,
    };
    for (int i = 0; i < SETTINGS_ACTIONS; i++) {
        lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(s_settings_action_rows[i]);
        if (label) lv_label_set_text(label, actions[i]);
    }

    // 详情面板只有 5 行高，因此把四个开关两两并排，避免溢出被截断。
    char detail[340];
    snprintf(detail, sizeof(detail),
             "%s  %s\n%s  %s\n%s  %u%%  %s\n%s %s  %s %s\n%s %s  %s %s",
             LV_TXT_SETTINGS_BRIDGE, settings->bridge,
             LV_TXT_SETTINGS_WIFI, settings->wifi,
             LV_TXT_SETTINGS_VOLUME, (unsigned)settings->volume,
             settings->muted ? LV_TXT_TOAST_MUTED : LV_TXT_IDLE,
             LV_TXT_EVENT_ENTER, settings->greet_enter ? "ON" : "OFF",
             LV_TXT_EVENT_GIFT, settings->thank_gift ? "ON" : "OFF",
             LV_TXT_EVENT_FOLLOW, settings->thank_follow ? "ON" : "OFF",
             LV_TXT_EVENT_LIKE, settings->thank_like ? "ON" : "OFF");

    // 详情放在标题下方的独立面板里，避免和操作行抢空间。
    if (s_settings_info == NULL) {
        lv_obj_t *info_panel = lv_obj_create(s_settings);
        lv_obj_set_size(info_panel, 216, 118);
        style_panel(info_panel, COL_PANEL2, 10);
        lv_obj_set_pos(info_panel, 12, 42);

        s_settings_info = make_label(info_panel, ui_font_body(), COL_MUTED);
        lv_obj_set_width(s_settings_info, 192);
        lv_label_set_long_mode(s_settings_info, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s_settings_info, 12, 10);
    }
    lv_label_set_text(s_settings_info, detail);

    lv_screen_load(s_settings);
    ui_unlock();
}

void app_ui_settings_select(int index)
{
    if (!ui_lock(200)) return;
    for (int i = 0; i < SETTINGS_ACTIONS; i++) {
        if (s_settings_action_rows[i] == NULL) continue;
        const bool selected = (i == index);
        style_panel(s_settings_action_rows[i], selected ? COL_PANEL2 : COL_PANEL, 8);
        lv_obj_set_style_border_side(s_settings_action_rows[i], LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(s_settings_action_rows[i], selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(s_settings_action_rows[i],
                                      lv_color_hex(COL_ACCENT), 0);
    }
    ui_unlock();
}

void app_ui_hide_settings(void)
{
    if (!ui_lock(200)) return;
    if (s_settings) {
        lv_obj_delete(s_settings);
        s_settings = NULL;
        s_settings_info = NULL;
        memset(s_settings_action_rows, 0, sizeof(s_settings_action_rows));
    }
    if (s_dash) lv_screen_load(s_dash);
    ui_unlock();
}

