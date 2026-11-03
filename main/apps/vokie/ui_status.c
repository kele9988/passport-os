// apps/vokie/ui_status.c —— Vokie 状态屏(passport-os 移植版)。
// 视觉与交互移植自 vokie-ai-passport-firmware 的 ui_status.c (MIT):
// 深色底 + Vokie 标志 + 大状态字 + 按键引导线 + 电量。
// 移植差异:息屏/背光由 launcher 统一管理;渲染用 LVGL timer 轮询快照。
#include "ui_status.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "vokie_symbol_asset.h"
#include <stdio.h>
#include <string.h>

#define UI_BG 0x0B1018
#define UI_PANEL 0x151D28
#define UI_MUTED 0x7F8B9E
#define UI_BATTERY_MUTED 0x646C78
#define UI_BLUE 0x67D8FF
#define UI_GREEN 0x78E0A4
#define UI_ORANGE 0xFFB454
#define UI_RED 0xFF7180
#define UI_CONTENT_W BSP_LCD_W
#define UI_BATTERY_W 66
#define UI_BATTERY_RIGHT_MARGIN 16
#define UI_BATTERY_X (BSP_LCD_W - UI_BATTERY_RIGHT_MARGIN - UI_BATTERY_W)
#define UI_BATTERY_Y 21
#define UI_LOGO_SIZE 64
#define UI_LOGO_X ((BSP_LCD_W - UI_LOGO_SIZE) / 2)
#define UI_LOGO_Y 132
#define UI_STATE_Y 53
#define UI_DETAIL_Y 86
#define UI_HINT_FADE_IN_MS 160
#define UI_HINT_FADE_OUT_MS 220
#define UI_HINT_GROUP_X 166
#define UI_HINT_GROUP_Y 126
#define UI_HINT_GROUP_W 58
#define UI_HINT_ROW_H 22
#define UI_HINT_TIMEOUT_US (3LL * 1000 * 1000)
#define UI_BATTERY_POLL_US (30LL * 1000 * 1000)
#define RENDER_POLL_MS 200

typedef struct {
    ui_status_state_t state;
    ui_status_hint_t hints;
    int64_t hints_deadline_us;
    uint32_t revision;
    char message[96];
} vokie_idle_t;

static lv_obj_t *s_screen;
static lv_obj_t *s_state;
static lv_obj_t *s_detail;
static lv_obj_t *s_button_hints;
static lv_obj_t *s_hint_panel;
static lv_obj_t *s_hint_lines[3];
static lv_obj_t *s_hint_labels[3];
static lv_obj_t *s_battery;
static lv_timer_t *s_timer;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static vokie_idle_t s_idle;
static bool s_hints_visible;
static ui_status_hint_t s_displayed_hints;
static uint32_t s_rendered_revision;
static int64_t s_next_battery_us;

// LVGL retains these arrays. Startup keeps all three guide paths;
// a single hint starts at the SEND row and points to its physical key.
static const lv_point_precise_t s_hint_all_lines[3][4] = {
    {{224, 137}, {229, 137}, {234, 57}, {239, 57}},
    {{224, 159}, {229, 159}, {234, 159}, {239, 159}},
    {{224, 181}, {229, 181}, {234, 261}, {239, 261}},
};
static const lv_point_precise_t s_hint_single_lines[3][4] = {
    {{224, 159}, {229, 159}, {234, 57}, {239, 57}},
    {{224, 159}, {229, 159}, {234, 159}, {239, 159}},
    {{224, 159}, {229, 159}, {234, 261}, {239, 261}},
};

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                     int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color, int width)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    if (width > 0) {
        lv_obj_set_width(obj, width);
        lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    }
    return obj;
}

static const char *state_name(ui_status_state_t state)
{
    switch (state) {
    case UI_STATUS_RECORDING: return "LISTENING";
    case UI_STATUS_PROCESSING: return "THINKING";
    case UI_STATUS_SUCCESS: return "SENT";
    case UI_STATUS_ERROR: return "ERROR";
    case UI_STATUS_DISCONNECTED: return "OFFLINE";
    case UI_STATUS_STARTING: return "STARTING";
    default: return "READY";
    }
}

static uint32_t state_color(ui_status_state_t state)
{
    switch (state) {
    case UI_STATUS_RECORDING: return UI_ORANGE;
    case UI_STATUS_PROCESSING: return UI_BLUE;
    case UI_STATUS_SUCCESS: return UI_GREEN;
    case UI_STATUS_ERROR: return UI_RED;
    case UI_STATUS_DISCONNECTED: return UI_MUTED;
    default: return UI_BLUE;
    }
}

static const lv_image_dsc_t s_vokie_symbol = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_NATIVE,
        .w = UI_LOGO_SIZE,
        .h = UI_LOGO_SIZE,
        .stride = UI_LOGO_SIZE * 2,
    },
    .data_size = sizeof(s_vokie_symbol_data),
    .data = s_vokie_symbol_data,
};

static void draw_vokie(lv_obj_t *parent)
{
    // 64x64 RGB565 raster of the official vokie-symbol.svg path.
    // NOTE: 品牌资产,LicenseRef-Vokie-Brand-Asset(非 MIT),仅限个人设备使用。
    lv_obj_t *mark = lv_image_create(parent);
    lv_image_set_src(mark, &s_vokie_symbol);
    lv_obj_set_pos(mark, UI_LOGO_X, UI_LOGO_Y);
}

static void draw_button_rail(lv_obj_t *parent)
{
    // Button hints and leader lines appear after a physical-key action.
    const uint32_t colors[3] = {UI_ORANGE, UI_BLUE, UI_GREEN};
    const char *actions[3] = {"VOICE", "SEND", "UNDO"};
    s_button_hints = lv_obj_create(parent);
    lv_obj_remove_style_all(s_button_hints);
    lv_obj_set_pos(s_button_hints, 0, 0);
    lv_obj_set_size(s_button_hints, BSP_LCD_W, BSP_LCD_H);
    lv_obj_remove_flag(s_button_hints, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *line = lv_line_create(s_button_hints);
        s_hint_lines[i] = line;
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_set_size(line, BSP_LCD_W, BSP_LCD_H);
        lv_line_set_points(line, s_hint_all_lines[i], 4);
        lv_obj_set_style_line_width(line, 1, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_line_opa(line, LV_OPA_50, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
    }
    s_hint_panel = box(s_button_hints, UI_HINT_GROUP_X, UI_HINT_GROUP_Y,
                       UI_HINT_GROUP_W, UI_HINT_ROW_H * 3, UI_PANEL, 7);
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *text = label(s_button_hints, actions[i], &lv_font_montserrat_14,
                               colors[i], UI_HINT_GROUP_W);
        s_hint_labels[i] = text;
        lv_obj_set_pos(text, UI_HINT_GROUP_X, UI_HINT_GROUP_Y +
            i * UI_HINT_ROW_H + (UI_HINT_ROW_H - lv_font_montserrat_14.line_height) / 2);
    }
    lv_obj_add_flag(s_button_hints, LV_OBJ_FLAG_HIDDEN);
}

static void draw_battery(lv_obj_t *parent)
{
    s_battery = label(parent, LV_SYMBOL_BATTERY_EMPTY " --%",
                      &lv_font_montserrat_14, UI_BATTERY_MUTED, UI_BATTERY_W);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_battery, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_battery, UI_BATTERY_X, UI_BATTERY_Y);
}

static void set_button_hints_visible(ui_status_hint_t hints)
{
    if (!s_button_hints) return;
    const bool visible = hints != UI_STATUS_HINT_NONE;
    if (visible && hints != s_displayed_hints) {
        const bool all = hints == UI_STATUS_HINT_ALL;
        for (int i = 0; i < 3; ++i) {
            if (hints & (1 << i)) {
                lv_obj_set_y(s_hint_labels[i], UI_HINT_GROUP_Y +
                    (all ? i : 1) * UI_HINT_ROW_H +
                    (UI_HINT_ROW_H - lv_font_montserrat_14.line_height) / 2);
                lv_line_set_points(s_hint_lines[i],
                    all ? s_hint_all_lines[i] : s_hint_single_lines[i], 4);
                lv_obj_remove_flag(s_hint_labels[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_hint_lines[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_hint_labels[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_hint_lines[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_set_y(s_hint_panel, UI_HINT_GROUP_Y + (all ? 0 : UI_HINT_ROW_H));
        lv_obj_set_height(s_hint_panel, (all ? 3 : 1) * UI_HINT_ROW_H);
        s_displayed_hints = hints;
    }
    if (visible == s_hints_visible) return;
    s_hints_visible = visible;
    lv_anim_delete(s_button_hints, NULL);
    if (visible) {
        lv_obj_remove_flag(s_button_hints, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_button_hints);
        lv_obj_set_style_opa(s_button_hints, LV_OPA_TRANSP, 0);
        lv_obj_fade_in(s_button_hints, UI_HINT_FADE_IN_MS, 0);
    } else {
        lv_obj_fade_out(s_button_hints, UI_HINT_FADE_OUT_MS, 0);
    }
}

static void render_state(void)
{
    const uint32_t color = state_color(s_idle.state);
    lv_label_set_text(s_state, state_name(s_idle.state));
    lv_obj_set_style_text_color(s_state, lv_color_hex(color), 0);
    lv_label_set_text(s_detail, s_idle.message[0] ? s_idle.message : "Vokie is ready");
    set_button_hints_visible(s_idle.hints);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_screen) return;
    const int64_t now = esp_timer_get_time();

    // 过期的按键提示自动隐藏(激活状态除外)。
    taskENTER_CRITICAL(&s_state_lock);
    const bool active = s_idle.state == UI_STATUS_RECORDING ||
                        s_idle.state == UI_STATUS_PROCESSING;
    if (s_idle.hints != UI_STATUS_HINT_NONE && !active &&
        now >= s_idle.hints_deadline_us) {
        s_idle.hints = UI_STATUS_HINT_NONE;
        ++s_idle.revision;
    }
    const uint32_t revision = s_idle.revision;
    taskEXIT_CRITICAL(&s_state_lock);

    if (revision != s_rendered_revision) {
        render_state();
        s_rendered_revision = revision;
    }
    if (now >= s_next_battery_us) {
        static const char *const icons[] = {
            LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1,
            LV_SYMBOL_BATTERY_2, LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_FULL,
        };
        const int soc = bsp_battery_soc();
        char text[16];
        if (soc >= 0 && soc <= 100) {
            snprintf(text, sizeof(text), "%d%%", soc);
            lv_label_set_text_fmt(s_battery, "%s %s", icons[(soc + 24) / 25], text);
        } else {
            lv_label_set_text(s_battery, LV_SYMBOL_BATTERY_EMPTY " --%");
        }
        lv_obj_set_style_text_color(s_battery,
            lv_color_hex((soc >= 0 && soc <= 20) ? UI_RED : UI_BATTERY_MUTED), 0);
        s_next_battery_us = now + UI_BATTERY_POLL_US;
    }
}

void ui_status_init(void)
{
    if (s_screen || !bsp_lvgl_lock(1000)) return;
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_state_lock);
    memset(&s_idle, 0, sizeof(s_idle));
    s_idle.state = UI_STATUS_STARTING;
    s_idle.hints = UI_STATUS_HINT_ALL;
    s_idle.hints_deadline_us = now + UI_HINT_TIMEOUT_US;
    s_idle.revision = 1;
    taskEXIT_CRITICAL(&s_state_lock);
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    draw_vokie(s_screen);
    draw_button_rail(s_screen);
    draw_battery(s_screen);

    s_state = label(s_screen, "", &lv_font_montserrat_20, UI_BLUE, UI_CONTENT_W);
    lv_obj_set_pos(s_state, 0, UI_STATE_Y);
    s_detail = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_detail, UI_CONTENT_W);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_detail, 0, UI_DETAIL_Y);

    lv_screen_load(s_screen);
    s_rendered_revision = 0;
    s_next_battery_us = 0;
    render_state();
    bsp_lvgl_unlock();
    s_timer = lv_timer_create(tick, RENDER_POLL_MS, NULL);
}

void ui_status_deinit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = NULL; }
    s_battery = NULL; s_state = NULL; s_detail = NULL;
    s_button_hints = NULL; s_hint_panel = NULL;
    for (int i = 0; i < 3; ++i) { s_hint_lines[i] = NULL; s_hint_labels[i] = NULL; }
    s_hints_visible = false;
    s_displayed_hints = UI_STATUS_HINT_NONE;
}

void ui_status_touch(ui_status_hint_t hint)
{
    if (hint != UI_STATUS_HINT_VOICE && hint != UI_STATUS_HINT_SEND &&
        hint != UI_STATUS_HINT_UNDO && hint != UI_STATUS_HINT_ALL) return;
    taskENTER_CRITICAL(&s_state_lock);
    s_idle.hints_deadline_us = esp_timer_get_time() + UI_HINT_TIMEOUT_US;
    s_idle.hints = hint;
    ++s_idle.revision;
    taskEXIT_CRITICAL(&s_state_lock);
}

void ui_status_set_state(ui_status_state_t state, const char *message)
{
    taskENTER_CRITICAL(&s_state_lock);
    char text[sizeof(s_idle.message)] = {0};
    if (message) strncpy(text, message, sizeof(text) - 1);
    const bool changed = s_idle.state != state || strcmp(s_idle.message, text) != 0;
    if (changed) {
        s_idle.state = state;
        memcpy(s_idle.message, text, sizeof(text));
        ++s_idle.revision;
    }
    taskEXIT_CRITICAL(&s_state_lock);
}
