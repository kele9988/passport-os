// apps/wbconsole/app_wbconsole.cc —— WorkBuddy 伴生控制台(play 513)。
// 与 PC 端 wb_app.py 协同:状态/积分展示、审批/提问遥控、按住说话语音录入。
// 协议逆向自 wb_app.py(闭源固件的 PC 伴生端);设备侧行为对齐官方
// demo_workbuddy.c 的语义(2s poll 窗口、90s 提问页超时、OK/DOWN 审批)。
//
// 界面为 5 页 HUD(按官方设计稿 1:1 复刻:TELEMETRY / SESSION RACK /
// CO-PILOT RUN / FIELD REC / ALERT):
//   页0 额度  环形仪表 + 余额卡 + 额度用量刻度(顶栏页码徽章已恢复)
//   页1 会话  会话卡组(CH 编号 / 状态胶囊 / TOK)
//   页2 运行  像素机器人 + 任务名 + 已跑时长
//   页3 录音  波形 + VU 表 + 通道参数
//   页4 提醒  红/紫警示卡 + 选项
// 视觉语言:深底 + 描边卡片 + 亮青/绿/琥珀强调;顶栏 = 徽标胶囊 + 英文页名 +
// 链路胶囊 + 电量块;底部三个按键胶囊,中间胶囊兼作页码。
// 按键:UP 按住=说话/松开=结束;DOWN 点按=翻页(页4且有提醒时为拒绝/移动选项),
//      DOWN 长按=跳过提问;OK 点按=允许/提交/切投递目标;长按 OK=系统返回(全局拦截)。
#include "apps/app_base.h"
#include "apps/wbconsole/wb_ble.h"
#include "apps/wbconsole/wb_voice.h"
#include "apps/wbconsole/wb_wifi_link.h"
#include "launcher/app_registry.h"
#include "app_fonts.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ui_pixel.h"
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

static const char *TAG = "wb_console";

namespace {

// ---- 配色(取自设计稿)----
constexpr uint32_t COL_BG = 0x070C13;        // 近黑底
constexpr uint32_t COL_PANEL = 0x0D1622;     // 卡片面
constexpr uint32_t COL_PANEL2 = 0x111C2A;    // 卡内嵌块
constexpr uint32_t COL_EDGE = 0x1D2C40;      // 常态描边
constexpr uint32_t COL_EDGE_HI = 0x2E6E9B;   // 主卡高亮描边
constexpr uint32_t COL_CYAN = 0x4FC3E8;
constexpr uint32_t COL_CYAN_D = 0x1F4F6B;
constexpr uint32_t COL_GREEN = 0x5FD98C;
constexpr uint32_t COL_AMBER = 0xFFA33C;
constexpr uint32_t COL_RED = 0xFF6B6B;
constexpr uint32_t COL_VIOLET = 0xA98BFA;
constexpr uint32_t COL_TEXT = 0xE6EDF5;
constexpr uint32_t COL_MUTED = 0x7C8CA3;
constexpr uint32_t COL_DIM = 0x22303F;       // 未点亮格 / 波形暗柱

// ---- 布局 ----
constexpr int SCR_W = BSP_LCD_W;
constexpr int SCR_H = BSP_LCD_H;
constexpr int M = 5;                             // 页面左右边距
constexpr int TOP_Y = 5;                         // 顶栏
constexpr int TOP_H = 26;
constexpr int BODY_Y = 34;                       // 内容区
constexpr int BODY_H = 254;                      // 34..288
constexpr int FOOT_Y = 291;                      // 底部按键胶囊
constexpr int FOOT_H = 24;
constexpr int CW = SCR_W - M * 2;                // 卡片标准宽 230
constexpr int PAGE_COUNT = 6;
constexpr int MAX_TASKS = 3;
constexpr int MAX_ASK_OPTS = 6;
constexpr int WAVE_N = 16;                       // 录音波形柱数(每根约 400B 堆)

constexpr int ASK_TIMEOUT_MS = 90000;    // 官方:提问页 90s 无操作自动退出
constexpr int BATTERY_PERIOD_MS = 30000;
constexpr int MSG_TOAST_MS = 4000;
constexpr int REC_MAX_SEC = 120;         // 与 wb_voice 的 WB_MAX_RECORD_MS 一致

enum class Mode { kStatus, kApproval, kAsk, kRecording, kMsg };

enum Page { kPgQuota = 0, kPgSessions, kPgRun, kPgRec, kPgAlert, kPgPc };

// 顶栏每页的徽标与页名。设计稿这里是 MIC/AI/CH/WB + FIELD REC 之类的英文,
// 按用户要求整机文案改中文 —— 16px 中文比 8px 英文宽,顶栏按 2 字徽标 +
// 4 字页名重排过坐标。
struct PageMeta {
    const char *badge;   // 徽标胶囊文字(中文 2 字)
    const char *title;   // 页名(中文 4 字)
};
const PageMeta k_meta[PAGE_COUNT] = {
    {"余额", "积分仪表"},
    {"会话", "会话列表"},
    {"运行", "正在执行"},
    {"语音", "按住说话"},
    {"提醒", "需要处理"},
    {"主机", "电脑监控"},
};

struct TaskRow {
    char name[48];
    char model[32];
    char state[16];
    float credit;
};

// ---- 协议状态(BLE 回调与按键任务都会访问;只在这一层用临界区) ----
portMUX_TYPE s_proto_lock = portMUX_INITIALIZER_UNLOCKED;
TaskRow s_tasks[MAX_TASKS];
int s_task_count = 0;
float s_credit_remain = -1.0f, s_credit_total = -1.0f;
Mode s_mode = Mode::kStatus;
volatile int s_page = kPgQuota;
int s_prev_page = kPgQuota;
bool s_alert_auto = false;      // 页4 是否由提醒自动切入
bool s_run_auto = false;        // 页2 是否由「任务开始跑」自动切入
int s_run_count = -1;           // 上一轮 tick RUNNING 任务数(-1=首帧未采样)
int64_t s_rec_start_us = 0;
int64_t s_run_start_us = 0;     // 当前任务开始跑的时刻(协议不给,本地计)

struct Approval { int id; char tool[40]; char hint[64]; bool valid; };
struct Ask {
    int id; char tool[40]; char text[80];
    char opts[MAX_ASK_OPTS][24]; int opt_count; bool ans; bool multi;
    bool valid; int64_t deadline_us; int selected; uint32_t mask;
};
Approval s_appr;
Ask s_ask;

// 页6 电脑监控（2026-11-02，数据源 wb_app.py _push_pcmon，走 BLE/WiFi 通用
// 通道；仪表盘思路借鉴 Bigd1gua/pc-monitor，但不用 USB 串口）。
struct PcMon {
    bool valid;
    int64_t rx_us;             // 最近一帧到达时刻（超 15s 视为失联）
    int cpu, mem, dsk, gpu, gt, ct;
    float memu, nu, nd;        // 内存已用 GiB / 网速 上、下行 KB/s
    char top[28];              // 最忙进程 "name 22%"
};
PcMon s_pc = {};   // gpu/gt/ct 为 -1 才是"未检出";首帧到达前 fresh 恒 false,不会误显 0℃
static bool s_pc_seen = false;   // 首帧置位,配合把未解析字段置 -1

volatile int s_target = 0;               // 0 reply 1 job 2 manual
static const char *k_targets[3] = {"reply", "job", "manual"};
// 显示名面向用户:reply=说进当前对话 job=新建任务 manual=只转文字不发
static const char *k_target_names[3] = {"对话", "新任务", "听写"};

char s_msg[80];
int64_t s_msg_until_us = 0;
int64_t s_battery_next_us = 0;

// ---- UI 对象 ----
lv_obj_t *s_screen = nullptr;
lv_obj_t *s_pages[PAGE_COUNT] = {};
lv_obj_t *s_badge = nullptr;      // 顶栏徽标胶囊
lv_obj_t *s_badge_lbl = nullptr;
lv_obj_t *s_title = nullptr;
lv_obj_t *s_link_pill = nullptr;  // 顶栏链路胶囊
lv_obj_t *s_link_dot = nullptr;
lv_obj_t *s_link_lbl = nullptr;
lv_obj_t *s_batt_pct = nullptr;
lv_obj_t *s_batt_fill = nullptr;
lv_timer_t *s_timer = nullptr;
int s_shown_page = -1;

// 底栏三胶囊
lv_obj_t *s_foot_pill[3] = {};
lv_obj_t *s_foot_lbl[3] = {};
lv_obj_t *s_foot_dot = nullptr;   // 中胶囊的圆点

// 页0 额度
lv_obj_t *s_q_arc = nullptr;
lv_obj_t *s_q_pct = nullptr;
lv_obj_t *s_q_state = nullptr;    // NOMINAL / LOW / EMPTY 胶囊
lv_obj_t *s_q_remain = nullptr;
lv_obj_t *s_q_total = nullptr;
lv_obj_t *s_q_used = nullptr;
lv_obj_t *s_q_matrix = nullptr;
lv_obj_t *s_q_mcell[8] = {};
lv_obj_t *s_q_mhint = nullptr;
lv_obj_t *s_q_burn = nullptr;
lv_obj_t *s_q_clock = nullptr;
lv_obj_t *s_q_link = nullptr;
lv_obj_t *s_q_target = nullptr;

// 页1 会话
struct SessCard {
    lv_obj_t *card = nullptr;
    lv_obj_t *bar = nullptr;      // 左侧状态色条
    lv_obj_t *idx = nullptr;      // 01
    lv_obj_t *dot = nullptr;      // 左下状态点
    lv_obj_t *pill = nullptr;     // 状态胶囊
    lv_obj_t *pill_lbl = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *model = nullptr;
    lv_obj_t *credit = nullptr;
};
SessCard s_sc[MAX_TASKS];
lv_obj_t *s_s_count = nullptr;
lv_obj_t *s_s_sum = nullptr;

// 页2 运行
lv_obj_t *s_r_state = nullptr;
lv_obj_t *s_r_state_lbl = nullptr;
lv_obj_t *s_r_name = nullptr;
lv_obj_t *s_r_model = nullptr;
lv_obj_t *s_r_credit = nullptr;
lv_obj_t *s_r_elapsed = nullptr;
lv_obj_t *s_r_bar = nullptr;
lv_obj_t *s_r_busy = nullptr;

// 页3 录音
lv_obj_t *s_rec_time = nullptr;
lv_obj_t *s_rec_state = nullptr;
lv_obj_t *s_rec_wave[WAVE_N] = {};
int s_wave_h[WAVE_N] = {};
lv_obj_t *s_rec_vu = nullptr;
lv_obj_t *s_rec_bar = nullptr;
lv_obj_t *s_rec_chan = nullptr;
lv_obj_t *s_rec_hint = nullptr;
constexpr int WAVE_CY = 127;      // 波形柱中线(相对主卡)

// 页4 提醒
lv_obj_t *s_a_card = nullptr;
lv_obj_t *s_a_kind = nullptr;
lv_obj_t *s_a_kind_lbl = nullptr;
lv_obj_t *s_a_title = nullptr;
lv_obj_t *s_a_body = nullptr;
lv_obj_t *s_a_opt[3] = {};
lv_obj_t *s_a_hint = nullptr;
lv_obj_t *s_a_idle = nullptr;

// 页5 电脑监控
lv_obj_t *s_pc_bar[3] = {};      // CPU/内存/GPU占用
lv_obj_t *s_pc_val[3] = {};      // 右侧百分比
lv_obj_t *s_pc_net = nullptr;    // 网速行
lv_obj_t *s_pc_gpu = nullptr;    // GPU 行
lv_obj_t *s_pc_top = nullptr;    // 最忙进程
lv_obj_t *s_pc_state = nullptr;  // 离线/提示

// ---- 绘图小工具 ----

void set_text(lv_obj_t *obj, const char *text) {
    if (obj && std::strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
}

void set_color(lv_obj_t *obj, uint32_t color) {
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

// 纯色块(像素画/状态点/色条都用它)
lv_obj_t *make_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                     int radius = 0) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                     int x, int y, int w, int h, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, "");
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(o, align, 0);
    // 字间距:16px 中文在 240 宽屏上连排会糊成一片,统一留 1px 间隙。
    // 只动 letter_space 不动 line_space —— 行高不变,按 20px 行高算好的
    // 卡片高度和纵向布局全部不受影响。
    lv_obj_set_style_text_letter_space(o, 1, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    return o;
}

// 描边卡片:设计稿的卡片都是 1px 描边 + 无阴影的直角圆角
lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t edge,
                    int radius = 5, uint32_t fill = COL_PANEL, int border = 1) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, radius, 0);
    lv_obj_set_style_border_width(c, border, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(edge), 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

// 胶囊(徽标/状态/按键提示)
lv_obj_t *make_pill(lv_obj_t *parent, int x, int y, int w, int h, uint32_t fill,
                    uint32_t edge, int border) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_radius(p, h / 2, 0);
    if (border > 0) {
        lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(p, border, 0);
        lv_obj_set_style_border_color(p, lv_color_hex(edge), 0);
    } else {
        lv_obj_set_style_bg_color(p, lv_color_hex(fill), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
    return p;
}

lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
    lv_bar_set_range(b, 0, 1000);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    return b;
}

uint32_t state_color(const char *st) {
    if (!st || !st[0]) return COL_MUTED;
    if (std::strcmp(st, "RUNNING") == 0) return COL_CYAN;
    if (std::strcmp(st, "COMPLETED") == 0) return COL_GREEN;
    if (std::strcmp(st, "NEED_APPROVAL") == 0) return COL_AMBER;
    if (std::strcmp(st, "NEED_ANSWER") == 0) return COL_VIOLET;
    if (std::strcmp(st, "ERROR") == 0) return COL_RED;
    return COL_MUTED;
}

// 协议里的状态是英文枚举,界面正文统一显示中文(用户要求菜单全中文)。
const char *state_cn(const char *st) {
    if (!st || !st[0]) return "空闲";
    if (std::strcmp(st, "RUNNING") == 0) return "执行中";
    if (std::strcmp(st, "COMPLETED") == 0) return "已完成";
    if (std::strcmp(st, "NEED_APPROVAL") == 0) return "待审批";
    if (std::strcmp(st, "NEED_ANSWER") == 0) return "待回答";
    if (std::strcmp(st, "ERROR") == 0) return "出错";
    if (std::strcmp(st, "IDLE") == 0) return "空闲";
    return st;
}

// ---- 发送辅助 ----

int sendf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int sendf(const char *fmt, ...)
{
    // 不要在这里套 taskENTER_CRITICAL:wb_ble_send_line 内部失败重试会
    // vTaskDelay,临界区里阻塞是非法操作,会直接 panic 重启。
    // 发送互斥由 wb_ble_send_line 自己的 tx 信号量负责。
    static char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return -1;
    return wb_ble_send_line(buf, (size_t)n);
}

void send_ack(const char *ack)
{
    (void)wb_ble_send_line(ack, std::strlen(ack));
}

// ---- 上行帧处理 ----

void enter_alert_page() {
    // 提醒到达时自动切到页4;记下原页,提醒结束后退回。
    if (s_page != kPgAlert) {
        s_prev_page = s_page;
        s_page = kPgAlert;
        s_alert_auto = true;
    }
}

void handle_approval(const cJSON *root) {
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *tool = cJSON_GetObjectItem(root, "tool");
    const cJSON *hint = cJSON_GetObjectItem(root, "hint");
    taskENTER_CRITICAL(&s_proto_lock);
    s_appr = {};
    s_appr.valid = cJSON_IsNumber(id);
    s_appr.id = s_appr.valid ? id->valueint : 0;
    if (cJSON_IsString(tool)) {
        std::strncpy(s_appr.tool, tool->valuestring, sizeof(s_appr.tool) - 1);
    }
    if (cJSON_IsString(hint)) {
        std::strncpy(s_appr.hint, hint->valuestring, sizeof(s_appr.hint) - 1);
    }
    if (s_appr.valid) s_mode = Mode::kApproval;
    taskEXIT_CRITICAL(&s_proto_lock);
    if (s_appr.valid) enter_alert_page();
    send_ack("OK_APPROVAL");
}

void handle_ask(const cJSON *root) {
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *tool = cJSON_GetObjectItem(root, "tool");
    const cJSON *text = cJSON_GetObjectItem(root, "text");
    const cJSON *opts = cJSON_GetObjectItem(root, "opts");
    const cJSON *ans = cJSON_GetObjectItem(root, "ans");
    const cJSON *multi = cJSON_GetObjectItem(root, "multi");
    taskENTER_CRITICAL(&s_proto_lock);
    s_ask = {};
    s_ask.valid = cJSON_IsNumber(id);
    s_ask.id = s_ask.valid ? id->valueint : 0;
    if (cJSON_IsString(tool)) std::strncpy(s_ask.tool, tool->valuestring, sizeof(s_ask.tool) - 1);
    if (cJSON_IsString(text)) std::strncpy(s_ask.text, text->valuestring, sizeof(s_ask.text) - 1);
    s_ask.ans = cJSON_IsNumber(ans) && ans->valueint == 1;
    s_ask.multi = cJSON_IsNumber(multi) && multi->valueint == 1;
    s_ask.selected = 0;
    s_ask.mask = 0;
    s_ask.deadline_us = esp_timer_get_time() + ASK_TIMEOUT_MS * 1000LL;
    if (cJSON_IsArray(opts)) {
        int i = 0;
        const cJSON *item = nullptr;
        cJSON_ArrayForEach(item, opts) {
            if (i >= MAX_ASK_OPTS || !cJSON_IsString(item)) break;
            std::strncpy(s_ask.opts[i], item->valuestring, sizeof(s_ask.opts[i]) - 1);
            ++i;
        }
        s_ask.opt_count = i;
    }
    if (s_ask.valid) s_mode = Mode::kAsk;
    taskEXIT_CRITICAL(&s_proto_lock);
    if (s_ask.valid) enter_alert_page();
    send_ack("OK_ASK");
}

void handle_line(const char *line, size_t len) {
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) return;   // 裸文本帧(设备不该收到)静默忽略
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return; }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : nullptr;

    if (t && std::strcmp(t, "approval") == 0) {
        handle_approval(root);
    } else if (t && std::strcmp(t, "ask") == 0) {
        handle_ask(root);
    } else if (t && std::strcmp(t, "ask_end") == 0) {
        const cJSON *id = cJSON_GetObjectItem(root, "id");
        taskENTER_CRITICAL(&s_proto_lock);
        if (cJSON_IsNumber(id) && s_ask.valid && id->valueint == s_ask.id) {
            s_ask.valid = false;
            if (s_mode == Mode::kAsk) s_mode = Mode::kStatus;
        }
        taskEXIT_CRITICAL(&s_proto_lock);
    } else if (t && std::strcmp(t, "msg") == 0) {
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        taskENTER_CRITICAL(&s_proto_lock);
        s_msg[0] = '\0';
        if (cJSON_IsString(text)) {
            std::strncpy(s_msg, text->valuestring, sizeof(s_msg) - 1);
        }
        s_msg_until_us = esp_timer_get_time() + MSG_TOAST_MS * 1000LL;
        taskEXIT_CRITICAL(&s_proto_lock);
    } else if (t && std::strcmp(t, "pcmon") == 0) {
        const cJSON *cpu = cJSON_GetObjectItem(root, "cpu");
        const cJSON *mem = cJSON_GetObjectItem(root, "mem");
        const cJSON *memu = cJSON_GetObjectItem(root, "memu");
        const cJSON *dsk = cJSON_GetObjectItem(root, "dsk");
        const cJSON *nu = cJSON_GetObjectItem(root, "nu");
        const cJSON *nd = cJSON_GetObjectItem(root, "nd");
        const cJSON *gpu = cJSON_GetObjectItem(root, "gpu");
        const cJSON *gt = cJSON_GetObjectItem(root, "gt");
        const cJSON *ct = cJSON_GetObjectItem(root, "ct");
        const cJSON *top = cJSON_GetObjectItem(root, "top");
        taskENTER_CRITICAL(&s_proto_lock);
        if (!s_pc_seen) {           // 首帧:未带的字段置"未检出",避免 0 当真值
            s_pc_seen = true;
            s_pc.gpu = -1; s_pc.gt = -1; s_pc.ct = -1;
        }
        s_pc.valid = true;
        s_pc.rx_us = esp_timer_get_time();
        if (cJSON_IsNumber(cpu)) s_pc.cpu = cpu->valueint;
        if (cJSON_IsNumber(mem)) s_pc.mem = mem->valueint;
        if (cJSON_IsNumber(memu)) s_pc.memu = static_cast<float>(memu->valuedouble);
        if (cJSON_IsNumber(dsk)) s_pc.dsk = dsk->valueint;
        if (cJSON_IsNumber(nu)) s_pc.nu = static_cast<float>(nu->valuedouble);
        if (cJSON_IsNumber(nd)) s_pc.nd = static_cast<float>(nd->valuedouble);
        if (cJSON_IsNumber(gpu)) s_pc.gpu = gpu->valueint;
        if (cJSON_IsNumber(gt)) s_pc.gt = gt->valueint;
        if (cJSON_IsNumber(ct)) s_pc.ct = ct->valueint;
        s_pc.top[0] = '\0';
        if (cJSON_IsString(top)) std::strncpy(s_pc.top, top->valuestring, sizeof(s_pc.top) - 1);
        taskEXIT_CRITICAL(&s_proto_lock);
        // 诊断：每 10 帧打一条（~20s），确认链路活着、字段有值。
        static uint32_t s_pcmon_dbg = 0;
        if (++s_pcmon_dbg % 10 == 1) {
            ESP_LOGI(TAG, "pcmon#%lu cpu=%d mem=%d dsk=%d gpu=%d gt=%d ct=%d top=%s",
                     (unsigned long)s_pcmon_dbg, s_pc.cpu, s_pc.mem, s_pc.dsk,
                     s_pc.gpu, s_pc.gt, s_pc.ct, s_pc.top);
        }
    } else if (t && std::strcmp(t, "cfg") == 0) {
        const cJSON *vol = cJSON_GetObjectItem(root, "beep_vol");
        const int v = cJSON_IsNumber(vol) ? vol->valueint : 60;
        sendf("OK_CFG:%d", v < 0 ? 0 : (v > 100 ? 100 : v));
    } else if (t && std::strcmp(t, "status_list") == 0) {
        const cJSON *tasks = cJSON_GetObjectItem(root, "tasks");
        taskENTER_CRITICAL(&s_proto_lock);
        s_task_count = 0;
        if (cJSON_IsArray(tasks)) {
            const cJSON *item = nullptr;
            cJSON_ArrayForEach(item, tasks) {
                if (s_task_count >= MAX_TASKS) break;
                TaskRow &row = s_tasks[s_task_count];
                row = {};
                const cJSON *name = cJSON_GetObjectItem(item, "name");
                const cJSON *model = cJSON_GetObjectItem(item, "model");
                const cJSON *state = cJSON_GetObjectItem(item, "state");
                const cJSON *credit = cJSON_GetObjectItem(item, "credit");
                if (cJSON_IsString(name)) std::strncpy(row.name, name->valuestring, sizeof(row.name) - 1);
                if (cJSON_IsString(model)) std::strncpy(row.model, model->valuestring, sizeof(row.model) - 1);
                if (cJSON_IsString(state)) std::strncpy(row.state, state->valuestring, sizeof(row.state) - 1);
                if (cJSON_IsNumber(credit)) row.credit = (float)credit->valuedouble;
                ++s_task_count;
            }
        }
        taskEXIT_CRITICAL(&s_proto_lock);
        send_ack("OK_STATUS");
    } else if (t) {
        // status_list 已处理;其余未知类型回 ERR_JSON。
        if (std::strcmp(t, "status_list") != 0) send_ack("ERR_JSON");
    } else {
        // 无 type 帧:积分 {"remain":R,"total":T}。
        const cJSON *remain = cJSON_GetObjectItem(root, "remain");
        const cJSON *total = cJSON_GetObjectItem(root, "total");
        if (cJSON_IsNumber(remain) && cJSON_IsNumber(total)) {
            taskENTER_CRITICAL(&s_proto_lock);
            s_credit_remain = (float)remain->valuedouble;
            s_credit_total = (float)total->valuedouble;
            taskEXIT_CRITICAL(&s_proto_lock);
        }
    }
    cJSON_Delete(root);
}

void handle_link(bool connected) {
    taskENTER_CRITICAL(&s_proto_lock);
    if (!connected) {
        s_mode = Mode::kStatus;
        s_appr.valid = false;
        s_ask.valid = false;
        // 刻意保留积分与会话快照:短暂断连(PC 重启/看护重连的空档)不该把
        // 用户刚看到的内容抹成空页。是否在线由顶栏链路胶囊表达。
    }
    taskEXIT_CRITICAL(&s_proto_lock);
}

wb_ble_callbacks_t s_ble_cb = {handle_line, handle_link};

// ---- 页面切换 ----

void show_page(int p) {
    if (p < 0 || p >= PAGE_COUNT) p = kPgQuota;
    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_t *pg = s_pages[i];
        if (!pg) continue;
        if (i == p) {
            lv_obj_remove_flag(pg, LV_OBJ_FLAG_HIDDEN);
            // 双保险:只靠 HIDDEN 时,若某次标志没落到位,5 个页会在同一
            // 坐标叠加渲染(表现为「页码是 3/5 却显示提醒页文案」)。
            // 把非当前页同时移出屏幕,任何情况下都不会互相穿插。
            lv_obj_set_pos(pg, 0, 0);
            lv_obj_move_foreground(pg);
        } else {
            lv_obj_add_flag(pg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(pg, -SCR_W, 0);
        }
    }
    // 顶栏跟着换页(徽章已删,只换页名)
    set_text(s_title, k_meta[p].title);

    // 语音投递目标只在录音页有效:离开录音页立刻回收成「回复」并上报。
    // 否则切到「任务」档后忘了切回来,在后面随口说一句话就会开一个新任务、
    // 白烧积分 —— 这个档位的破坏性只有离开视野才显现,所以不能靠人记住。
    if (p != kPgRec && s_target != 0) {
        s_target = 0;
        (void)sendf("{\"type\":\"msg_target\",\"target\":\"reply\"}");
    }
    s_shown_page = p;
}

// ---- 各页渲染 ----

void render_quota(bool conn) {
    char buf[40];
    const bool has = s_credit_total > 0 || s_credit_remain >= 0;
    int pct = 0;
    if (s_credit_total > 0 && s_credit_remain >= 0) {
        pct = (int)(s_credit_remain * 100.0f / s_credit_total + 0.5f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    // 余量不足 10% 时整环转红(设计稿语义:余量见底整体转红)
    const bool low = has && pct < 10;
    const bool warn = has && pct < 30 && !low;
    const uint32_t accent = low ? COL_RED : (warn ? COL_AMBER : COL_GREEN);

    lv_arc_set_value(s_q_arc, pct * 10);
    lv_obj_set_style_arc_color(s_q_arc, lv_color_hex(accent), LV_PART_INDICATOR);
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    set_text(s_q_pct, buf);
    set_color(s_q_pct, accent);
    set_text(s_q_state, has ? (low ? "告急" : (warn ? "偏低" : "充足")) : "离线");
    set_color(s_q_state, has ? accent : COL_MUTED);

    if (s_credit_remain >= 0) {
        std::snprintf(buf, sizeof(buf), "%.0f", s_credit_remain);
        set_text(s_q_remain, buf);
        set_color(s_q_remain, accent);
    } else {
        set_text(s_q_remain, "--");
        set_color(s_q_remain, COL_MUTED);
    }
    if (s_credit_total >= 0) {
        std::snprintf(buf, sizeof(buf), "总计  %.0f", s_credit_total);
        set_text(s_q_total, buf);
        std::snprintf(buf, sizeof(buf), "已用  %.0f", s_credit_total - s_credit_remain);
        set_text(s_q_used, buf);
        std::snprintf(buf, sizeof(buf), "%.0f", s_credit_total - s_credit_remain);
        set_text(s_q_burn, buf);
    } else {
        set_text(s_q_total, "总计  --");
        set_text(s_q_used, "已用  --");
        set_text(s_q_burn, "--");
    }

    // 额度用量:8 格按已用比例点亮(从右往左点,和设计稿一致)
    const int lit = (pct * 8 + 99) / 100;
    for (int i = 0; i < 8; ++i) {
        const bool on = (7 - i) < lit;
        lv_obj_set_style_bg_color(s_q_mcell[i], lv_color_hex(on ? accent : COL_DIM), 0);
    }
    std::snprintf(buf, sizeof(buf), "已用 %d%%", has ? 100 - pct : 0);
    set_text(s_q_mhint, buf);
    set_color(s_q_mhint, accent);

    // 顶栏链路与时钟
    std::snprintf(buf, sizeof(buf), "%02d:%02d", (int)((esp_timer_get_time() / 1000000) / 60) % 100,
                  (int)((esp_timer_get_time() / 1000000) % 60));
    set_text(s_q_clock, buf);
    // 链路状态只在底部显示一处(运行时长卡副行留空,避免「链路正常」重复)
    set_text(s_q_link, conn ? "链路正常" : "等待主机");
    set_color(s_q_link, conn ? COL_GREEN : COL_MUTED);
    set_text(s_q_target, "");
}

void render_sessions() {
    char buf[48];
    float sum = 0.0f;
    for (int i = 0; i < s_task_count; ++i) sum += s_tasks[i].credit;
    std::snprintf(buf, sizeof(buf), "进行中会话 %02d", s_task_count);
    set_text(s_s_count, buf);
    std::snprintf(buf, sizeof(buf), "合计 %.1f", sum);
    set_text(s_s_sum, buf);

    for (int i = 0; i < MAX_TASKS; ++i) {
        SessCard &sc = s_sc[i];
        if (i >= s_task_count) {
            if (sc.card) lv_obj_add_flag(sc.card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (sc.card) {
            lv_obj_clear_flag(sc.card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(sc.card);
        }
        const TaskRow &r = s_tasks[i];
        const uint32_t c = state_color(r.state);
        std::snprintf(buf, sizeof(buf), "%02d", i + 1);
        set_text(sc.idx, buf);
        set_text(sc.pill_lbl, state_cn(r.state));
        set_color(sc.pill_lbl, c);
        lv_obj_set_style_border_color(sc.pill, lv_color_hex(c), 0);
        lv_obj_set_style_bg_color(sc.bar, lv_color_hex(c), 0);
        lv_obj_set_style_bg_color(sc.dot, lv_color_hex(c), 0);
        lv_obj_set_style_border_color(sc.card, lv_color_hex(i == 0 ? c : COL_EDGE), 0);
        set_text(sc.name, r.name[0] ? r.name : "-");
        std::snprintf(buf, sizeof(buf), "模型 %s", r.model[0] ? r.model : "-");
        set_text(sc.model, buf);
        std::snprintf(buf, sizeof(buf), "%.1f 分", r.credit);
        set_text(sc.credit, buf);
        set_color(sc.credit, c);
    }
    if (s_task_count == 0) {
        set_text(s_s_count, "进行中会话 00");
    }
}

void render_run() {
    char buf[48];
    if (s_task_count == 0) {
        lv_bar_set_value(s_r_bar, 0, LV_ANIM_OFF);
        set_text(s_r_state_lbl, "待机");
        set_color(s_r_state_lbl, COL_MUTED);
        lv_obj_set_style_border_color(s_r_state, lv_color_hex(COL_EDGE), 0);
        set_text(s_r_name, "等待任务开始");
        set_color(s_r_name, COL_MUTED);
        set_text(s_r_model, "模型 -");
        set_text(s_r_credit, "--");
        set_text(s_r_elapsed, "已运行 --:--");
        set_text(s_r_busy, "空闲中 · 等待新任务");
        set_color(s_r_busy, COL_MUTED);
        return;
    }
    const TaskRow &r = s_tasks[0];
    const uint32_t c = state_color(r.state);
    set_text(s_r_state_lbl, state_cn(r.state));
    set_color(s_r_state_lbl, c);
    lv_obj_set_style_border_color(s_r_state, lv_color_hex(c), 0);
    set_text(s_r_name, r.name[0] ? r.name : "-");
    set_color(s_r_name, COL_TEXT);
    std::snprintf(buf, sizeof(buf), "模型 %s", r.model[0] ? r.model : "-");
    set_text(s_r_model, buf);
    std::snprintf(buf, sizeof(buf), "%.1f 分", r.credit);
    set_text(s_r_credit, buf);
    set_color(s_r_credit, c);

    int64_t el = 0;
    if (std::strcmp(r.state, "RUNNING") == 0) {
        if (!s_run_start_us) s_run_start_us = esp_timer_get_time();
        el = (esp_timer_get_time() - s_run_start_us) / 1000000;
    } else {
        s_run_start_us = 0;
    }
    std::snprintf(buf, sizeof(buf), "已运行 %02d:%02d", (int)(el / 60), (int)(el % 60));
    set_text(s_r_elapsed, buf);

    const bool running = std::strcmp(r.state, "RUNNING") == 0;
    set_text(s_r_busy, running ? "忙碌 · 正在处理任务" : "空闲中 · 等待新任务");
    set_color(s_r_busy, running ? COL_CYAN : COL_MUTED);
    // 运行中给一条流动的进度条(纯视觉,协议没有 per-task 进度)
    const int phase = (int)((esp_timer_get_time() / 1000 / 8) % 1000);
    const int v = running ? (phase < 500 ? 200 + phase : 700 - (phase - 500)) : 0;
    lv_bar_set_value(s_r_bar, v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_r_bar, lv_color_hex(running ? c : COL_DIM), LV_PART_INDICATOR);
}

void render_pc(bool conn) {
    char buf[64];
    // 数据保鲜 15s：守护停止推送（PC 睡眠/守护退出）后回落到离线态。
    const bool fresh = s_pc.valid && (esp_timer_get_time() - s_pc.rx_us) < 15000000LL;
    const int vals[3] = {s_pc.cpu, s_pc.mem, s_pc.gpu};   // 04:08 磁盘条改 GPU 占用
    for (int i = 0; i < 3; ++i) {
        const bool ok = conn && fresh && vals[i] >= 0;
        // make_bar 的量程是 0~1000(任务页动画/VU 共用),百分比必须 ×10,
        // 否则 27% 只画出 2.7% 宽 —— 表现就是「进度条贴近 0 且不动」。
        lv_bar_set_value(s_pc_bar[i], ok ? vals[i] * 10 : 0, LV_ANIM_OFF);
        const uint32_t c = !ok ? COL_DIM
                               : (vals[i] >= 90 ? COL_RED : (vals[i] >= 70 ? COL_AMBER : COL_CYAN));
        lv_obj_set_style_bg_color(s_pc_bar[i], lv_color_hex(c), LV_PART_INDICATOR);
        if (ok) {
            std::snprintf(buf, sizeof(buf), "%d%%", vals[i]);
            set_text(s_pc_val[i], buf);
        } else {
            set_text(s_pc_val[i], "--");
        }
        set_color(s_pc_val[i], ok ? c : COL_MUTED);
    }
    if (conn && fresh) {
        std::snprintf(buf, sizeof(buf), "网速  ↑%.1f  ↓%.1f KB/s", s_pc.nu, s_pc.nd);
        set_text(s_pc_net, buf);
        set_color(s_pc_net, COL_TEXT);
        {
            // 下方行只显示温度（GPU 占用已上提为第三根条）
            char g[20], c[20];
            if (s_pc.gt >= 0) std::snprintf(g, sizeof(g), "%d℃", s_pc.gt);
            else std::snprintf(g, sizeof(g), "--");
            if (s_pc.ct >= 0) std::snprintf(c, sizeof(c), "%d℃", s_pc.ct);
            else std::snprintf(c, sizeof(c), "--");
            std::snprintf(buf, sizeof(buf), "GPU %s · CPU %s", g, c);
            set_text(s_pc_gpu, buf);
            set_color(s_pc_gpu, COL_TEXT);
        }
        std::snprintf(buf, sizeof(buf), "最忙  %s", s_pc.top[0] ? s_pc.top : "-");
        set_text(s_pc_top, buf);
        set_color(s_pc_top, COL_CYAN);
        set_text(s_pc_state, "数据 2 秒一刷");
        set_color(s_pc_state, COL_MUTED);
    } else {
        set_text(s_pc_net, "网速  --");
        set_color(s_pc_net, COL_MUTED);
        set_text(s_pc_gpu, "温度 --");
        set_color(s_pc_gpu, COL_MUTED);
        set_text(s_pc_top, "最忙  --");
        set_color(s_pc_top, COL_MUTED);
        set_text(s_pc_state, conn ? "等待主机数据(需 psutil)" : "等待主机连接…");
        set_color(s_pc_state, conn ? COL_AMBER : COL_MUTED);
    }
}

bool msg_active() {
    return s_msg_until_us != 0 && s_msg[0] && esp_timer_get_time() < s_msg_until_us;
}

void render_rec(bool conn) {
    char buf[48];
    const bool rec = (s_mode == Mode::kRecording);
    if (rec) {
        const int64_t el = (esp_timer_get_time() - s_rec_start_us) / 1000000;
        std::snprintf(buf, sizeof(buf), "%02d:%02d", (int)(el / 60), (int)(el % 60));
        set_text(s_rec_time, buf);
        set_color(s_rec_time, COL_TEXT);
        set_text(s_rec_state, "正在上传电脑…");
        set_color(s_rec_state, COL_CYAN);
    } else {
        set_text(s_rec_time, "00:00");
        set_color(s_rec_time, conn ? COL_TEXT : COL_MUTED);
        set_text(s_rec_state, conn ? "待机 · 按住上键说话" : "等待主机连接…");
        set_color(s_rec_state, COL_MUTED);
    }

    // 波形:用麦克风真实峰值(0..32767)驱动,松手后归零。
    int peak = wb_voice_live_peak();
    const int level = rec ? (peak * 1000 / 32767) : 0;
    for (int i = 0; i < WAVE_N; ++i) {
        int target = 5;
        if (rec) {
            // 伪随机相位,让相邻柱高低错落;整体幅度跟随真实电平
            const int ph = (int)((esp_timer_get_time() / 45000 + i * 37) % 1000);
            const int wob = ph < 500 ? ph : (1000 - ph);
            target = 6 + (level * (18 + wob * 3 / 100)) / 1000;
            if (target > 36) target = 36;
            if (i >= WAVE_N - 4) target = target / 3;   // 尾部收窄,和设计稿一致
        }
        s_wave_h[i] += (target - s_wave_h[i]) * 2 / 3;
        if (s_wave_h[i] < 4) s_wave_h[i] = 4;
        lv_obj_set_size(s_rec_wave[i], 6, s_wave_h[i]);
        lv_obj_set_y(s_rec_wave[i], WAVE_CY - s_wave_h[i] / 2);
        const bool lit = rec || s_wave_h[i] > 6;
        lv_obj_set_style_bg_color(s_rec_wave[i], lv_color_hex(lit ? COL_CYAN : COL_DIM), 0);
    }

    // VU 表:dB 与百分比都从峰值换算(0dB = 满量程)
    if (peak < 1) peak = 1;
    const int pct = (peak * 100) / 32767;
    const float db = 20.0f * log10f((float)peak / 32767.0f);
    if (rec) {
        std::snprintf(buf, sizeof(buf), "%.1f dB (%d%%)", db, pct);
        lv_bar_set_value(s_rec_bar, (int32_t)(peak * 1000 / 32767), LV_ANIM_OFF);
    } else {
        std::snprintf(buf, sizeof(buf), "-- dB (0%%)");
        lv_bar_set_value(s_rec_bar, 0, LV_ANIM_OFF);
    }
    set_text(s_rec_vu, buf);
    set_color(s_rec_vu, pct > 92 ? COL_RED : COL_CYAN);

    std::snprintf(buf, sizeof(buf), "语音投递：%s", k_target_names[s_target]);
    set_text(s_rec_chan, buf);
    // 目标档位高亮:对话=绿(安全默认) 新任务=琥珀(白烧积分档) 听写=青;
    // 原先 COL_DIM 暗色与背景几乎同色,用户看不到当前档位。
    set_color(s_rec_chan,
              s_target == 0 ? COL_GREEN : (s_target == 1 ? COL_AMBER : COL_CYAN));

    if (msg_active()) {
        set_text(s_rec_hint, s_msg);
        set_color(s_rec_hint, COL_GREEN);
    } else if (!conn) {
        set_text(s_rec_hint, "等待主机接收音频流…");
        set_color(s_rec_hint, COL_MUTED);
    } else if (rec) {
        set_text(s_rec_hint, "正在上传电脑…");
        set_color(s_rec_hint, COL_CYAN);
    } else {
        set_text(s_rec_hint, "就绪 · 按住上键开始说话");
        set_color(s_rec_hint, COL_TEXT);
    }
}

const char *footer_left() {
    if (s_mode == Mode::kApproval && s_appr.valid) return "OK 允许";
    if (s_mode == Mode::kAsk && s_ask.valid) return "↑ 选择";
    if (s_mode == Mode::kRecording) return "OK 发往";
    return "↓ 翻页";
}

const char *footer_right() {
    if (s_mode == Mode::kApproval && s_appr.valid) return "↓ 拒绝";
    if (s_mode == Mode::kAsk && s_ask.valid) {
        return s_ask.multi ? "长按提交" : "↓ 下一项";
    }
    if (s_mode == Mode::kRecording) return "↑ 松手";
    return "↑ 说话";
}

void render_alert() {
    char buf[80];
    const bool has_appr = (s_mode == Mode::kApproval && s_appr.valid);
    const bool has_ask = (s_mode == Mode::kAsk && s_ask.valid);

    if (!has_appr && !has_ask) {
        if (s_a_idle) lv_obj_clear_flag(s_a_idle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_a_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_a_idle) lv_obj_add_flag(s_a_idle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_a_card, LV_OBJ_FLAG_HIDDEN);

    if (has_appr) {
        lv_obj_set_style_border_color(s_a_card, lv_color_hex(COL_RED), 0);
        lv_obj_set_style_border_width(s_a_card, 2, 0);
        lv_obj_set_style_border_color(s_a_kind, lv_color_hex(COL_RED), 0);
        lv_obj_set_style_bg_color(s_a_kind, lv_color_hex(COL_RED), 0);
        set_text(s_a_kind_lbl, "需审批");
        set_color(s_a_kind_lbl, COL_BG);
        set_text(s_a_title, s_appr.tool);
        set_color(s_a_title, COL_TEXT);
        set_text(s_a_body, s_appr.hint[0] ? s_appr.hint : "(无说明)");
        set_text(s_a_opt[0], "OK 通过 运行这条命令");
        set_text(s_a_opt[1], "↓ 拒绝 阻止执行");
        set_text(s_a_opt[2], "");
        set_color(s_a_opt[0], COL_GREEN);
        set_color(s_a_opt[1], COL_RED);
        set_text(s_a_hint, "长按 OK 返回");
        set_color(s_a_hint, COL_MUTED);
        return;
    }

    // 提问
    lv_obj_set_style_border_color(s_a_card, lv_color_hex(COL_VIOLET), 0);
    lv_obj_set_style_border_width(s_a_card, 2, 0);
    lv_obj_set_style_border_color(s_a_kind, lv_color_hex(COL_VIOLET), 0);
    lv_obj_set_style_bg_color(s_a_kind, lv_color_hex(COL_VIOLET), 0);
    set_text(s_a_kind_lbl, s_ask.multi ? "多选" : "单选");
    set_color(s_a_kind_lbl, COL_BG);
    set_text(s_a_title, s_ask.text[0] ? s_ask.text : "主机提问");
    set_color(s_a_title, COL_TEXT);
    set_text(s_a_body, "");

    // 三行窗口跟随选中项滚动
    int base = 0;
    if (s_ask.selected > 2) base = s_ask.selected - 2;
    if (base > s_ask.opt_count - 3) base = s_ask.opt_count - 3;
    if (base < 0) base = 0;
    for (int i = 0; i < 3; ++i) {
        const int k = base + i;
        if (k >= s_ask.opt_count) { set_text(s_a_opt[i], ""); continue; }
        const bool on = s_ask.multi ? ((s_ask.mask >> k) & 1u) != 0 : (k == s_ask.selected);
        std::snprintf(buf, sizeof(buf), "%s%d. %s", on ? "› " : "  ", k + 1, s_ask.opts[k]);
        set_text(s_a_opt[i], buf);
        set_color(s_a_opt[i], k == s_ask.selected ? COL_CYAN : COL_TEXT);
    }
    const int remain_s = (int)((s_ask.deadline_us - esp_timer_get_time()) / 1000000);
    std::snprintf(buf, sizeof(buf), "剩余 %d s · OK 提交", remain_s > 0 ? remain_s : 0);
    set_text(s_a_hint, buf);
    set_color(s_a_hint, COL_MUTED);
}

// 底栏三胶囊:左=主操作 / 中=状态+页码 / 右=次操作
void render_footer(bool conn) {
    char buf[24];
    set_text(s_foot_lbl[0], footer_left());
    set_text(s_foot_lbl[2], footer_right());

    uint32_t c = COL_CYAN;
    const char *st = "待机";
    if (s_mode == Mode::kRecording) { st = "录音"; c = COL_RED; }
    else if (s_mode == Mode::kApproval && s_appr.valid) { st = "审批"; c = COL_RED; }
    else if (s_mode == Mode::kAsk && s_ask.valid) { st = "提问"; c = COL_VIOLET; }
    else if (!conn) { st = "离线"; c = COL_MUTED; }
    std::snprintf(buf, sizeof(buf), "%s %d/%d", st, s_page + 1, PAGE_COUNT);
    set_text(s_foot_lbl[1], buf);
    set_color(s_foot_lbl[1], c);
    lv_obj_set_style_bg_color(s_foot_dot, lv_color_hex(c), 0);
    lv_obj_set_style_border_color(s_foot_pill[1], lv_color_hex(c), 0);
}

void tick(lv_timer_t *timer) {
    if (!s_screen) return;
    const bool visible = app_registry_screen_is_on();
    lv_timer_set_period(timer, visible ? 200 : 1000);
    if (!visible) return;

    const int64_t now = esp_timer_get_time();

    // 电池周期上报
    if (wb_ble_connected() && now >= s_battery_next_us) {
        s_battery_next_us = now + BATTERY_PERIOD_MS * 1000LL;
        sendf("{\"type\":\"battery\",\"soc\":%d}", bsp_battery_soc());
    }

    // 提问页 90s 超时(官方语义:设备自行退出)
    bool ask_expired = false;
    taskENTER_CRITICAL(&s_proto_lock);
    if (s_mode == Mode::kAsk && s_ask.valid && now >= s_ask.deadline_us) {
        s_ask.valid = false;
        s_mode = Mode::kStatus;
        ask_expired = true;
    }
    if (s_mode != Mode::kRecording && s_msg_until_us && now >= s_msg_until_us) {
        s_msg_until_us = 0;
    }
    // 提醒结束且是自动切进来的,退回原页
    if (s_page == kPgAlert && s_alert_auto && !s_appr.valid && !s_ask.valid) {
        s_page = s_prev_page;
        s_alert_auto = false;
    }
    // 运行页自动切换(兑现已有的提示文案):RUNNING 任务数增加(新任务开跑)→跳到
    // 运行页;全部结束→跳回原页。录音中/提醒页/已手动在运行页时不打扰;
    // 用户中途手动翻走则只清标志。注意用数量变化而非有无沿检测——长挂
    // RUNNING 会话会让「无→有」沿永远不再出现。
    {
        int rc = 0;
        for (int i = 0; i < s_task_count; ++i) {
            if (std::strcmp(s_tasks[i].state, "RUNNING") == 0) ++rc;
        }
        if (s_run_count >= 0 && rc != s_run_count) {
            if (rc > s_run_count && s_mode != Mode::kRecording &&
                s_page != kPgRun && s_page != kPgAlert) {
                s_prev_page = s_page;
                s_page = kPgRun;
                s_run_auto = true;
            } else if (rc == 0 && s_run_auto) {
                if (s_page == kPgRun) s_page = s_prev_page;
                s_run_auto = false;
            }
        }
        s_run_count = rc;
    }
    const int page = s_page;
    taskEXIT_CRITICAL(&s_proto_lock);

    const bool conn = wb_ble_connected();
    if (page != s_shown_page) show_page(page);

    // 顶栏:徽章胶囊 / 链路胶囊 / 电量(2026-11-02 按 18:33 前布局恢复页码徽章)
    const uint32_t accent = (page == kPgRun) ? COL_GREEN : COL_CYAN;
    set_text(s_link_lbl, conn ? "在线" : "离线");
    set_color(s_link_lbl, conn ? accent : COL_MUTED);
    lv_obj_set_style_border_color(s_link_pill, lv_color_hex(conn ? accent : COL_EDGE), 0);
    lv_obj_set_style_bg_color(s_link_dot, lv_color_hex(conn ? accent : COL_MUTED), 0);

    const int soc = bsp_battery_soc();
    char bbuf[16];
    std::snprintf(bbuf, sizeof(bbuf), "%d%%", soc);
    set_text(s_batt_pct, bbuf);
    const int fill_w = (16 * soc) / 100;
    lv_obj_set_width(s_batt_fill, fill_w > 0 ? fill_w : 1);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(soc <= 15 ? COL_RED : (soc <= 30 ? COL_AMBER : COL_GREEN)), 0);

    if (page == kPgQuota) render_quota(conn);
    else if (page == kPgSessions) render_sessions();
    else if (page == kPgRun) render_run();
    else if (page == kPgRec) render_rec(conn);
    else if (page == kPgPc) render_pc(conn);
    else render_alert();

    render_footer(conn);

    if (ask_expired) ESP_LOGI(TAG, "提问页超时自动退出");
}

// ---- 按键 ----

void on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (btn < BSP_BTN_UP || btn > BSP_BTN_OK) return;

    Mode mode;
    taskENTER_CRITICAL(&s_proto_lock);
    mode = s_mode;
    taskEXIT_CRITICAL(&s_proto_lock);

    // 录音由 UP 按住/松开驱动,优先级最高(任何页面都可按住说话)。
    if (btn == BSP_BTN_UP && event == BTN_EVT_PRESS) {
        if (!wb_ble_connected()) return;
        if (wb_voice_begin() > 0) {
            taskENTER_CRITICAL(&s_proto_lock);
            s_mode = Mode::kRecording;
            s_rec_start_us = esp_timer_get_time();
            taskEXIT_CRITICAL(&s_proto_lock);
        }
        return;
    }
    if (btn == BSP_BTN_UP && event == BTN_EVT_RELEASE) {
        taskENTER_CRITICAL(&s_proto_lock);
        if (s_mode == Mode::kRecording) s_mode = Mode::kStatus;
        taskEXIT_CRITICAL(&s_proto_lock);
        wb_voice_end(1);
        return;
    }
    if (btn == BSP_BTN_UP) return;

    // 长按 DOWN:跳过当前提问
    if (btn == BSP_BTN_DOWN && event == BTN_EVT_LONG) {
        int ask_id = 0; bool has = false;
        taskENTER_CRITICAL(&s_proto_lock);
        if (mode == Mode::kAsk && s_ask.valid) { ask_id = s_ask.id; has = true; }
        taskEXIT_CRITICAL(&s_proto_lock);
        if (has) {
            (void)sendf("{\"type\":\"key\",\"approval_id\":%d,\"key\":\"skip\"}", ask_id);
            taskENTER_CRITICAL(&s_proto_lock);
            s_ask.valid = false;
            s_mode = Mode::kStatus;
            taskEXIT_CRITICAL(&s_proto_lock);
        }
        // 普通页:长按下键回翻上一页(短按下键=下一页,上键被录音占用)
        if (mode == Mode::kStatus) {
            taskENTER_CRITICAL(&s_proto_lock);
            s_page = (s_page + PAGE_COUNT - 1) % PAGE_COUNT;
            s_alert_auto = false;
            s_run_auto = false;
            taskEXIT_CRITICAL(&s_proto_lock);
        }
        return;
    }

    if (btn == BSP_BTN_DOWN && event == BTN_EVT_CLICK) {
        if (mode == Mode::kApproval) {
            int id = 0; bool has = false;
            taskENTER_CRITICAL(&s_proto_lock);
            if (s_appr.valid) { id = s_appr.id; has = true; }
            taskEXIT_CRITICAL(&s_proto_lock);
            if (has) {
                (void)sendf("{\"type\":\"key\",\"approval_id\":%d,\"key\":\"down\"}", id);
                taskENTER_CRITICAL(&s_proto_lock);
                s_appr.valid = false;
                s_mode = Mode::kStatus;
                taskEXIT_CRITICAL(&s_proto_lock);
            }
            return;
        }
        if (mode == Mode::kAsk) {
            // 移动选中项(单选/多选都用它移动,提交在 OK)
            taskENTER_CRITICAL(&s_proto_lock);
            if (s_ask.valid && s_ask.opt_count > 0) {
                s_ask.selected = (s_ask.selected + 1) % s_ask.opt_count;
            }
            taskEXIT_CRITICAL(&s_proto_lock);
            return;
        }
        // 普通页:翻到下一页(循环;手动翻页同时取消自动跳回)
        taskENTER_CRITICAL(&s_proto_lock);
        s_page = (s_page + 1) % PAGE_COUNT;
        s_alert_auto = false;
        s_run_auto = false;
        taskEXIT_CRITICAL(&s_proto_lock);
        return;
    }

    if (btn == BSP_BTN_OK && event == BTN_EVT_CLICK) {
        if (mode == Mode::kApproval) {
            int id = 0; bool has = false;
            taskENTER_CRITICAL(&s_proto_lock);
            if (s_appr.valid) { id = s_appr.id; has = true; }
            taskEXIT_CRITICAL(&s_proto_lock);
            if (has) {
                (void)sendf("{\"type\":\"key\",\"approval_id\":%d,\"key\":\"ok\"}", id);
                taskENTER_CRITICAL(&s_proto_lock);
                s_appr.valid = false;
                s_mode = Mode::kStatus;
                taskEXIT_CRITICAL(&s_proto_lock);
            }
            return;
        }
        if (mode == Mode::kAsk) {
            bool multi = false, ansable = false;
            int idx = 0;
            taskENTER_CRITICAL(&s_proto_lock);
            if (s_ask.valid) {
                multi = s_ask.multi;
                ansable = s_ask.ans;
                idx = s_ask.selected;
                // 多选:OK 只切换当前项勾选,提交走长按 OK(见下)
                if (multi) s_ask.mask ^= (1u << idx);
            }
            taskEXIT_CRITICAL(&s_proto_lock);
            if (multi) return;
            if (!ansable) return;   // 该提问不支持设备作答
            const int ask_id = s_ask.id;
            // 官方:作答即乐观退出提问页
            taskENTER_CRITICAL(&s_proto_lock);
            s_ask.valid = false;
            s_mode = Mode::kStatus;
            taskEXIT_CRITICAL(&s_proto_lock);
            (void)sendf("{\"type\":\"ask_answer\",\"id\":%d,\"index\":%d}", ask_id, idx);
            return;
        }
        // 记录页:OK 切换语音投递目标并上报
        if (s_page == kPgRec) {
            const int next = (s_target + 1) % 3;
            s_target = next;
            (void)sendf("{\"type\":\"msg_target\",\"target\":\"%s\"}", k_targets[next]);
        }
        return;
    }

    // 长按 OK:提交多选提问
    if (btn == BSP_BTN_OK && event == BTN_EVT_LONG) {
        if (mode != Mode::kAsk) return;
        int id = 0; uint32_t mask = 0; bool has = false;
        taskENTER_CRITICAL(&s_proto_lock);
        if (s_ask.valid && s_ask.multi) { id = s_ask.id; mask = s_ask.mask; has = true; }
        taskEXIT_CRITICAL(&s_proto_lock);
        if (!has) return;
        taskENTER_CRITICAL(&s_proto_lock);
        s_ask.valid = false;
        s_mode = Mode::kStatus;
        taskEXIT_CRITICAL(&s_proto_lock);
        (void)sendf("{\"type\":\"ask_answer\",\"id\":%d,\"mask\":%lu}", id,
                    (unsigned long)mask);
        return;
    }
}

// ---- 构建 ----

// 顶栏:徽标胶囊 + 页名 + 链路胶囊 + 电量块(设计稿结构,文案中文化)
void build_top(lv_obj_t *scr) {
    lv_obj_t *bar = make_card(scr, M, TOP_Y, CW, TOP_H, COL_EDGE, 6, COL_PANEL);
    (void)bar;

    // 页名(03:55 精简:徽章胶囊与页名内容重复,去掉徽章,页名左移补位)。
    s_title = make_label(scr, &buddy_font_16, COL_TEXT, M + 6, TOP_Y + 3, 72, 20);
    lv_label_set_text(s_title, "积分仪表");

    s_link_pill = make_pill(scr, M + 119, TOP_Y + 4, 48, 18, 0, COL_CYAN, 1);
    s_link_dot = make_block(s_link_pill, 5, 6, 5, 5, COL_CYAN, 3);
    s_link_lbl = make_label(s_link_pill, &buddy_font_16, COL_CYAN, 13, -1, 34, 20);
    lv_label_set_text(s_link_lbl, "在线");

    // 宽 36:montserrat_14 下 "100%" ≈36px,24px 会把百分号裁掉(实测)。
    s_batt_pct = make_label(scr, &lv_font_montserrat_14, COL_MUTED, M + 166, TOP_Y + 6, 36, 16,
                            LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_batt_pct, "00%");

    // 电池:外壳 + 内部填充 + 右侧正极凸起
    lv_obj_t *shell = make_card(scr, M + 203, TOP_Y + 9, 16, 11, COL_MUTED, 2, COL_PANEL, 1);
    (void)shell;
    s_batt_fill = make_block(scr, M + 205, TOP_Y + 11, 8, 7, COL_GREEN, 1);
    make_block(scr, M + 220, TOP_Y + 13, 2, 4, COL_MUTED, 0);
}

// 像素机器人(设计稿 CO-PILOT RUN 页的主视觉)。
// 只用 12 个矩形拼:每个 lv_obj 约 400B 堆,这块屏的堆余量不允许逐像素画。
void build_bot(lv_obj_t *parent, int cx, int y0) {
    const uint32_t c = COL_GREEN;
    make_block(parent, cx - 7, y0 - 6, 14, 6, c, 2);         // 天线顶
    make_block(parent, cx - 2, y0, 4, 8, c);                 // 天线杆
    // 头:描边圆角矩形
    lv_obj_t *head = make_card(parent, cx - 32, y0 + 10, 64, 34, c, 9, COL_PANEL, 3);
    (void)head;
    make_block(parent, cx - 22, y0 + 18, 14, 14, c, 7);      // 左眼
    make_block(parent, cx + 8, y0 + 18, 14, 14, c, 7);       // 右眼
    make_block(parent, cx - 10, y0 + 35, 20, 3, c, 1);       // 嘴
    make_block(parent, cx - 39, y0 + 18, 6, 12, c, 2);       // 左耳
    make_block(parent, cx + 33, y0 + 18, 6, 12, c, 2);       // 右耳
    make_block(parent, cx - 24, y0 + 46, 48, 12, c, 3);      // 脖子+身体
    make_block(parent, cx - 15, y0 + 58, 8, 8, c, 2);        // 左腿
    make_block(parent, cx + 7, y0 + 58, 8, 8, c, 2);         // 右腿
    make_block(parent, cx - 26, y0 + 66, 52, 3, c, 1);       // 底座
}

void build_quota(lv_obj_t *pg) {
    // 左:环形仪表卡
    lv_obj_t *left = make_card(pg, M, BODY_Y, 96, 100, COL_EDGE, 5, COL_PANEL);
    s_q_arc = lv_arc_create(left);
    lv_obj_set_pos(s_q_arc, 10, 4);
    lv_obj_set_size(s_q_arc, 76, 76);
    lv_arc_set_range(s_q_arc, 0, 1000);
    lv_arc_set_bg_angles(s_q_arc, 135, 45);
    lv_arc_set_value(s_q_arc, 0);
    lv_obj_set_style_arc_width(s_q_arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_q_arc, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_q_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_q_arc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_q_arc, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_q_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_q_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_q_arc, 0, 0);
    lv_obj_clear_flag(s_q_arc, LV_OBJ_FLAG_CLICKABLE);
    s_q_pct = make_label(left, &lv_font_montserrat_20, COL_GREEN, 10, 30, 76, 26,
                         LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_q_pct, "0%");
    lv_obj_t *st = make_pill(left, 20, 81, 56, 17, 0, COL_GREEN, 1);
    s_q_state = make_label(st, &buddy_font_16, COL_GREEN, 0, -2, 56, 20, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_q_state, "充足");

    // 右:余额卡
    lv_obj_t *right = make_card(pg, M + 102, BODY_Y, CW - 102, 100, COL_EDGE, 5, COL_PANEL);
    lv_obj_t *l1 = make_label(right, &buddy_font_16, COL_MUTED, 0, 2, CW - 102, 20,
                              LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(l1, "剩余积分");
    s_q_remain = make_label(right, &lv_font_montserrat_32, COL_GREEN, 0, 20, CW - 102, 32,
                            LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_q_remain, "--");
    s_q_total = make_label(right, &buddy_font_16, COL_TEXT, 0, 55, CW - 102, 20,
                           LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_q_total, "总计  --");
    s_q_used = make_label(right, &buddy_font_16, COL_AMBER, 0, 76, CW - 102, 20,
                          LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_q_used, "已用  --");

    // 中:额度用量刻度
    lv_obj_t *mx = make_card(pg, M, BODY_Y + 106, CW, 44, COL_EDGE, 5, COL_PANEL);
    lv_obj_t *h1 = make_label(mx, &buddy_font_16, COL_TEXT, 8, 1, 110, 20);
    lv_label_set_text(h1, "额度用量");
    s_q_mhint = make_label(mx, &buddy_font_16, COL_MUTED, CW - 86, 1, 78, 20,
                           LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_q_mhint, "已用 0%");
    for (int i = 0; i < 8; ++i) {
        s_q_mcell[i] = make_block(mx, 8 + i * 27, 25, 22, 11, COL_DIM, 2);
    }

    // 下:累计消耗 + 运行时长
    lv_obj_t *b1 = make_card(pg, M, BODY_Y + 156, 113, 66, COL_EDGE, 5, COL_PANEL);
    make_block(b1, 0, 8, 3, 50, COL_CYAN, 1);
    lv_obj_t *b1t = make_label(b1, &buddy_font_16, COL_CYAN, 0, 2, 113, 20,
                               LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(b1t, "累计消耗");
    s_q_burn = make_label(b1, &lv_font_montserrat_20, COL_TEXT, 0, 21, 113, 24,
                          LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_q_burn, "--");
    lv_obj_t *b1s = make_label(b1, &buddy_font_16, COL_MUTED, 0, 45, 113, 20,
                               LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(b1s, "积分");

    lv_obj_t *b2 = make_card(pg, M + 117, BODY_Y + 156, 113, 66, COL_EDGE, 5, COL_PANEL);
    make_block(b2, 0, 8, 3, 50, COL_AMBER, 1);
    lv_obj_t *b2t = make_label(b2, &buddy_font_16, COL_AMBER, 0, 2, 113, 20,
                               LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(b2t, "运行时长");
    // 副行已移除,数字下移垂直居中,不留空洞
    s_q_clock = make_label(b2, &lv_font_montserrat_20, COL_TEXT, 0, 30, 113, 24,
                           LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_q_clock, "00:00");
    s_q_target = make_label(b2, &buddy_font_16, COL_MUTED, 0, 45, 113, 20,
                            LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_q_target, "");   // 留空:链路状态只在底部显示一处

    // 底:链路说明
    lv_obj_t *info = make_card(pg, M, BODY_Y + 228, CW, 26, COL_EDGE, 5, COL_PANEL);
    s_q_link = make_label(info, &buddy_font_16, COL_MUTED, 8, 2, CW - 16, 22,
                          LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_q_link, "等待主机");
}

void build_sessions(lv_obj_t *pg) {
    s_s_count = make_label(pg, &buddy_font_16, COL_TEXT, M, BODY_Y, 130, 20);
    lv_label_set_text(s_s_count, "进行中会话 00");
    s_s_sum = make_label(pg, &buddy_font_16, COL_CYAN, M + 130, BODY_Y, 100, 20,
                         LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_s_sum, "合计 0.0");

    // 表头 20 + 3 张 75 高卡片 + 2 个 3px 间隔 = 251 < BODY_H(254)
    constexpr int card_h = 75;
    constexpr int card_gap = 3;
    for (int i = 0; i < MAX_TASKS; ++i) {
        const int y = BODY_Y + 21 + i * (card_h + card_gap);
        lv_obj_t *card = make_card(pg, M, y, CW, card_h, COL_EDGE, 5, COL_PANEL);
        SessCard &sc = s_sc[i];
        sc.card = card;
        sc.bar = make_block(card, 0, 8, 3, card_h - 16, COL_CYAN, 1);
        sc.idx = make_label(card, &lv_font_montserrat_20, COL_CYAN, 8, 10, 34, 24);
        lv_label_set_text(sc.idx, "00");
        sc.dot = make_block(card, 10, 44, 8, 8, COL_CYAN, 4);

        sc.pill = make_pill(card, 44, 1, 66, 17, 0, COL_CYAN, 1);
        sc.pill_lbl = make_label(sc.pill, &buddy_font_16, COL_CYAN, 0, -2, 66, 20,
                                 LV_TEXT_ALIGN_CENTER);
        lv_label_set_text(sc.pill_lbl, "执行中");

        sc.credit = make_label(card, &buddy_font_16, COL_CYAN, CW - 86, 0, 78, 20,
                               LV_TEXT_ALIGN_RIGHT);
        lv_label_set_text(sc.credit, "0.0 分");

        sc.name = make_label(card, &buddy_font_16, COL_TEXT, 44, 19, CW - 54, 36);
        lv_label_set_long_mode(sc.name, LV_LABEL_LONG_WRAP);
        lv_label_set_text(sc.name, "-");

        sc.model = make_label(card, &buddy_font_16, COL_MUTED, 44, 54, CW - 54, 18);
        lv_label_set_long_mode(sc.model, LV_LABEL_LONG_DOT);
        lv_label_set_text(sc.model, "模型 -");
        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    }
}

void build_run(lv_obj_t *pg) {
    lv_obj_t *a = make_card(pg, M, BODY_Y, CW, 176, COL_EDGE_HI, 5, COL_PANEL, 2);
    // 顶部一行:实时标记 + 已跑时长
    lv_obj_t *live = make_label(a, &buddy_font_16, COL_CYAN, 8, 1, 120, 20);
    lv_label_set_text(live, "实时运行");
    s_r_elapsed = make_label(a, &buddy_font_16, COL_MUTED, CW - 100, 1, 92, 20,
                             LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_r_elapsed, "已跑 --:--");

    build_bot(a, CW / 2, 30);

    // 状态胶囊 + 任务名 + 模型/额度
    s_r_state = make_pill(a, 8, 106, 62, 18, 0, COL_EDGE, 1);
    s_r_state_lbl = make_label(s_r_state, &buddy_font_16, COL_MUTED, 0, -2, 62, 20,
                               LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_r_state_lbl, "待机");
    s_r_name = make_label(a, &buddy_font_16, COL_TEXT, 8, 128, CW - 16, 22);
    lv_label_set_long_mode(s_r_name, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_r_name, "等待任务开始");
    s_r_model = make_label(a, &buddy_font_16, COL_MUTED, 8, 152, CW - 92, 20);
    lv_label_set_long_mode(s_r_model, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_r_model, "模型 -");
    s_r_credit = make_label(a, &buddy_font_16, COL_CYAN, CW - 80, 152, 72, 20,
                            LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_r_credit, "--");

    // 忙闲指示条(05:02 吸收原底部提示卡的高度,文字/进度条拉开层次)
    lv_obj_t *b = make_card(pg, M, BODY_Y + 182, CW, 68, COL_EDGE, 5, COL_PANEL);
    s_r_busy = make_label(b, &buddy_font_16, COL_MUTED, 8, 12, CW - 16, 20);
    lv_label_set_text(s_r_busy, "引擎空闲");
    s_r_bar = make_bar(b, 8, 42, CW - 16, 6, COL_CYAN);
}

void build_rec(lv_obj_t *pg) {
    // 主卡高 198:内嵌条 / 大计时 / 中文状态 / 波形 / VU / 进度条 / 通道参数
    lv_obj_t *a = make_card(pg, M, BODY_Y, CW, 204, COL_EDGE_HI, 6, COL_PANEL, 2);

    lv_obj_t *sync = make_card(a, 4, 4, CW - 8, 24, COL_EDGE, 4, COL_PANEL2);
    lv_obj_t *s1 = make_label(sync, &buddy_font_16, COL_CYAN, 7, 1, 100, 20);
    lv_label_set_text(s1, "↑ 同步");
    lv_obj_t *s2 = make_label(sync, &buddy_font_16, COL_MUTED, CW - 110, 1, 100, 20,
                              LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s2, "上限 02:00");

    s_rec_time = make_label(a, &lv_font_montserrat_48, COL_MUTED, 0, 30, CW, 54,
                            LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_rec_time, "00:00");

    s_rec_state = make_label(a, &buddy_font_16, COL_MUTED, 6, 86, CW - 12, 20,
                             LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_rec_state, "待机");

    // 波形柱:20 根,宽 6 间距 4,总宽 196,居中
    constexpr int ww = 6, wgap = 4;
    constexpr int total = WAVE_N * ww + (WAVE_N - 1) * wgap;
    const int x0 = (CW - total) / 2;
    for (int i = 0; i < WAVE_N; ++i) {
        s_wave_h[i] = 5;
        s_rec_wave[i] = make_block(a, x0 + i * (ww + wgap), WAVE_CY - 2, ww, 5, COL_DIM, 3);
    }

    lv_obj_t *v1 = make_label(a, &buddy_font_16, COL_MUTED, 8, 148, 90, 20);
    lv_label_set_text(v1, "峰值电平");
    s_rec_vu = make_label(a, &buddy_font_16, COL_CYAN, CW - 130, 148, 122, 20,
                          LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(s_rec_vu, "-- dB (0%)");
    s_rec_bar = make_bar(a, 8, 170, CW - 16, 8, COL_CYAN);

    s_rec_chan = make_label(a, &buddy_font_16, COL_GREEN, 0, 180, CW, 20,
                            LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_rec_chan, "语音投递：对话");

    // 提示卡
    lv_obj_t *h = make_card(pg, M, BODY_Y + 210, CW, 40, COL_EDGE, 6, COL_PANEL);
    s_rec_hint = make_label(h, &buddy_font_16, COL_MUTED, 6, 9, CW - 12, 22,
                            LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_rec_hint, "等待主机接收音频流…");
}

void build_alert(lv_obj_t *pg) {
    s_a_card = make_card(pg, M, BODY_Y, CW, 198, COL_RED, 6, COL_PANEL, 2);
    s_a_kind = make_pill(s_a_card, 8, 6, 58, 17, COL_RED, COL_RED, 0);
    s_a_kind_lbl = make_label(s_a_kind, &buddy_font_16, COL_BG, 0, -2, 58, 20,
                              LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_a_kind_lbl, "需审批");
    s_a_title = make_label(s_a_card, &buddy_font_16, COL_TEXT, 8, 28, CW - 16, 40);
    lv_label_set_long_mode(s_a_title, LV_LABEL_LONG_WRAP);
    s_a_body = make_label(s_a_card, &buddy_font_16, COL_MUTED, 8, 70, CW - 16, 40);
    lv_label_set_long_mode(s_a_body, LV_LABEL_LONG_WRAP);
    for (int i = 0; i < 3; ++i) {
        s_a_opt[i] = make_label(s_a_card, &buddy_font_16, COL_TEXT, 8, 116 + i * 24, CW - 16, 22);
    }
    s_a_hint = make_label(s_a_card, &buddy_font_16, COL_MUTED, 8, 176, CW - 16, 18);

    lv_obj_t *idle = make_card(pg, M, BODY_Y, CW, 198, COL_EDGE, 6, COL_PANEL);
    (void)idle;
    s_a_idle = make_label(idle, &buddy_font_16, COL_MUTED, 8, 86, CW - 16, 24,
                          LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_a_idle, "无待处理事项");
    // idle 卡在无提醒时显示,有提醒时靠 render_alert 切走
    lv_obj_add_flag(s_a_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *h = make_card(pg, M, BODY_Y + 204, CW, 40, COL_EDGE, 6, COL_PANEL);
    lv_obj_t *hl = make_label(h, &buddy_font_16, COL_MUTED, 6, 9, CW - 12, 22,
                              LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(hl, "有审批或提问会自动跳到这里");
}

void build_pc(lv_obj_t *pg) {
    // 主卡：三条资源条(CPU/内存/GPU占用) + 网速行
    lv_obj_t *a = make_card(pg, M, BODY_Y, CW, 150, COL_EDGE_HI, 6, COL_PANEL, 2);
    static const char *k_names[3] = {"CPU", "内存", "GPU"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *lbl = make_label(a, &buddy_font_16, COL_MUTED, 8, 8 + i * 38, 100, 20);
        lv_label_set_text(lbl, k_names[i]);
        s_pc_bar[i] = make_bar(a, 8, 30 + i * 38, CW - 90, 8, COL_CYAN);
        s_pc_val[i] = make_label(a, &buddy_font_16, COL_TEXT, CW - 76, 22 + i * 38, 66, 22,
                                 LV_TEXT_ALIGN_RIGHT);
        lv_label_set_text(s_pc_val[i], "--");
    }
    s_pc_net = make_label(a, &buddy_font_16, COL_MUTED, 8, 122, CW - 16, 20);
    lv_label_set_text(s_pc_net, "网速  --");

    // 副卡：GPU + 最忙进程
    lv_obj_t *b = make_card(pg, M, BODY_Y + 156, CW, 52, COL_EDGE, 5, COL_PANEL);
    s_pc_gpu = make_label(b, &buddy_font_16, COL_MUTED, 8, 2, CW - 16, 20);
    lv_label_set_text(s_pc_gpu, "GPU --");
    s_pc_top = make_label(b, &buddy_font_16, COL_TEXT, 8, 24, CW - 16, 22);
    lv_label_set_long_mode(s_pc_top, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_pc_top, "最忙  --");

    // 底部提示卡
    lv_obj_t *h = make_card(pg, M, BODY_Y + 214, CW, 32, COL_EDGE, 5, COL_PANEL);
    s_pc_state = make_label(h, &buddy_font_16, COL_MUTED, 6, 4, CW - 12, 22,
                            LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_pc_state, "等待主机数据…");
}

void build_footer(lv_obj_t *scr) {
    // 68+5+84+5+68 = 230 = CW,左右各留 M 边距;旧值总宽 240 会顶出屏幕,
    // 最右胶囊被挤到屏外后文字自动折行。中间那颗要装下「待机 5/5」+ 状态点。
    constexpr int w1 = 68, w2 = 84, w3 = 68;
    constexpr int gap = 5;
    const int x1 = M;
    const int x2 = x1 + w1 + gap;
    const int x3 = x2 + w2 + gap;

    for (int i = 0; i < 3; ++i) {
        const int x = (i == 0) ? x1 : (i == 1 ? x2 : x3);
        const int w = (i == 0) ? w1 : (i == 1 ? w2 : w3);
        s_foot_pill[i] = make_pill(scr, x, FOOT_Y, w, FOOT_H, COL_PANEL, COL_EDGE, 1);
        if (i == 1) {
            s_foot_dot = make_block(s_foot_pill[i], 8, FOOT_H / 2 - 3, 6, 6, COL_CYAN, 3);
            s_foot_lbl[i] = make_label(s_foot_pill[i], &buddy_font_16, COL_CYAN, 18, 1, w - 22, 22,
                                       LV_TEXT_ALIGN_LEFT);
        } else {
            // 标签靠左(18:33 之前为左对齐;居中是 18:33 改版引入的,现还原)
            s_foot_lbl[i] = make_label(s_foot_pill[i], &buddy_font_16, COL_MUTED, 8, 1, w - 10, 22,
                                       LV_TEXT_ALIGN_LEFT);
        }
    }
    lv_label_set_text(s_foot_lbl[0], "↓ 翻页");
    lv_label_set_text(s_foot_lbl[1], "待机 1/6");
    lv_label_set_text(s_foot_lbl[2], "↑ 说话");
}

void build_all(lv_obj_t *scr) {
    build_top(scr);
    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_t *pg = lv_obj_create(scr);
        lv_obj_remove_style_all(pg);
        // 初始就把非首页移出屏幕:首帧渲染早于 show_page 时也不会叠加。
        lv_obj_set_pos(pg, i == kPgQuota ? 0 : -SCR_W, 0);
        lv_obj_set_size(pg, SCR_W, SCR_H);
        lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(pg, 0, 0);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_CLICKABLE);
        s_pages[i] = pg;
        lv_obj_add_flag(pg, LV_OBJ_FLAG_HIDDEN);
    }
    build_quota(s_pages[kPgQuota]);
    build_sessions(s_pages[kPgSessions]);
    build_run(s_pages[kPgRun]);
    build_rec(s_pages[kPgRec]);
    build_alert(s_pages[kPgAlert]);
    build_pc(s_pages[kPgPc]);
    build_footer(scr);
}

// ---- 生命周期 ----

void init() {}

void start() {
    wb_ble_set_callbacks(&s_ble_cb);

    // 关键顺序:蓝牙控制器最先引导(它要一整块 ~50-65KB 连续内存,越早越大),
    // 然后语音环形缓冲(20KB),最后才建 LVGL 界面(上百个小对象,只适合最后)。
    // 实测(2026-11-01):voice 先于 BLE 时,voice+启动开销把 largest 从 73KB
    // 压到 47KB,BLE 控制器 Malloc failed → assert → 整机重启
    // (路径:对讲机释放蓝牙栈退出 → 再进本应用重新引导)。
    //
    // 2026-11-03 崩溃复盘:launcher 入口只有 96KB/largest 69.6KB,BLE 引导
    // 吃掉 66KB 后 UI 构建在 LVGL 样式分配上 OOM(get_local_style 断言,
    // style_cnt 回绕)→ 一进就重启。而 PC 伴生早已走 WiFi TCP 通道,
    // BLE 在 TCP 已连接时纯属浪费。故:TCP 在线 / 连续堆不足时跳过 BLE,
    // 数据、语音、下发全走 TCP(wb_ble_send_line 本就 TCP 优先路由)。
    esp_err_t err = ESP_OK;
    bool ble_skipped = false;
    const uint32_t largest0 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (wb_wifi_link_connected()) {
        ESP_LOGW(TAG, "bringup: TCP 已连接,跳过 BLE 引导(省 ~66KB 连续堆)");
        ble_skipped = true;
    } else if (largest0 < 128 * 1024) {
        // 2026-11-04 崩溃复盘:clean launch 冷启直接进本应用时 WiFi 还没连上,
        // largest≈96KB 会通过旧的 80KB 门槛 → 引导 BLE 吃掉 66KB → 紧接着
        // wb_voice_init 20KB + 建 UI(上百个 LVGL 对象)全部 OOM → 硬崩重启,
        // 外观表现就是「点开→整理内存→连续两次重启→回到首页」。
        // BLE 引导 + 语音工作区 + UI 实测要同时容纳,largest 需 >=128KB;冷启
        // 阶段永远达不到(开机空堆也只有 96KB),故这里一律跳过 BLE。
        // PC 伴生走 WiFi TCP(wb_ble_send_line 本就 TCP 优先),不依赖蓝牙。
        ESP_LOGW(TAG, "bringup: largest=%u 放不下 BLE+语音+UI(需>=128KB),跳过 BLE",
                 (unsigned)largest0);
        ble_skipped = true;
    } else {
        ESP_LOGW(TAG, "bringup: wb_ble_start (largest=%u)", (unsigned)largest0);
        err = wb_ble_start();
    }
    ESP_LOGW(TAG, "bringup: wb_voice_init");
    wb_voice_init();
    ESP_LOGW(TAG, "bringup: done (%s) heap=%u largest=%u", esp_err_to_name(err),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) { bsp_lvgl_unlock(); return; }
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_size(s_screen, SCR_W, SCR_H);

    build_all(s_screen);
    ESP_LOGW(TAG, "ui built: heap=%u largest=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    s_shown_page = -1;
    show_page(kPgQuota);
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, 200, nullptr);
    bsp_lvgl_unlock();

    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_proto_lock);
        std::snprintf(s_msg, sizeof(s_msg), "蓝牙启动失败: %s", esp_err_to_name(err));
        s_msg_until_us = esp_timer_get_time() + 8000LL * 1000;
        taskEXIT_CRITICAL(&s_proto_lock);
    }
}

bool prepare_stop() {
    // 录音中退出:按断链收尾,避免 PC 端等不到下降沿。
    wb_voice_end(4);
    return true;
}

void stop() {
    wb_ble_stop();
    // BLE 被跳过时 wb_ble_stop 会因 s_started=false 提前返回、不释放语音
    // 缓冲 —— 这里显式补一次(wb_voice_deinit 幂等,重复调用无害)。
    wb_voice_deinit();
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    // 子对象随根屏一并销毁,这里只清指针,避免下次 start() 复用野指针。
    for (auto &p : s_pages) p = nullptr;
    for (auto &sc : s_sc) sc = SessCard{};
    for (auto &o : s_foot_pill) o = nullptr;
    for (auto &o : s_foot_lbl) o = nullptr;
    for (auto &o : s_q_mcell) o = nullptr;
    for (auto &o : s_rec_wave) o = nullptr;
    for (auto &o : s_a_opt) o = nullptr;
    s_title = s_link_pill = s_link_dot = s_link_lbl = nullptr;
    s_batt_pct = s_batt_fill = s_foot_dot = nullptr;
    s_q_arc = s_q_pct = s_q_state = s_q_remain = s_q_total = s_q_used = nullptr;
    s_q_matrix = s_q_mhint = s_q_burn = s_q_clock = s_q_link = s_q_target = nullptr;
    s_s_count = s_s_sum = nullptr;
    s_r_state = s_r_state_lbl = s_r_name = s_r_model = s_r_credit = nullptr;
    s_r_elapsed = s_r_bar = s_r_busy = nullptr;
    s_rec_time = s_rec_state = s_rec_vu = s_rec_bar = s_rec_chan = s_rec_hint = nullptr;
    s_a_card = s_a_kind = s_a_kind_lbl = s_a_title = s_a_body = s_a_hint = s_a_idle = nullptr;
    s_shown_page = -1;
    taskENTER_CRITICAL(&s_proto_lock);
    s_mode = Mode::kStatus;
    s_appr.valid = false;
    s_ask.valid = false;
    s_page = kPgQuota;
    s_alert_auto = false;
    s_run_auto = false;
    s_run_count = -1;
    taskEXIT_CRITICAL(&s_proto_lock);
    bsp_lvgl_unlock();
}

} // namespace

extern const passport_app_t g_wb_app = {
    .id = APP_ID_WB,
    .name = "WorkBuddy",
    .en_name = "WORKBUDDY",
    .desc = "主机伴生控制台\n连接电脑协同",
    .tag = "BLE",
    .theme_color = 0x4FC3E8,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .prepare_stop = prepare_stop,
};
