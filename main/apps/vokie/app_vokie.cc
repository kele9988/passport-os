// apps/vokie/app_vokie.cc —— Vokie 麦克风应用外壳。
// 复用社区开源项目 vokie-ai-passport-firmware (MIT) 的 BLE 协议与状态屏视觉。
// 交互:UP 按住说话/松开发送、DOWN 发送、OK 撤销;长按 OK 由 launcher 返回主页。
#include "apps/app_base.h"
#include "apps/vokie/vokie_ble.h"
#include "apps/vokie/ui_status.h"
#include "apps/wbconsole/wb_gatt.h"
#include "bsp_display.h"
#include "esp_log.h"

static const char *TAG = "app_vokie";

namespace {
bool s_ble_ok = false;

void init() {}

void start() {
    s_ble_ok = vokie_ble_start() == ESP_OK;
    if (!s_ble_ok) {
        ESP_LOGE(TAG, "Vokie BLE 启动失败");
        // 仍然进界面,显示 OFFLINE,用户退出重进即可重试。
    }
    if (bsp_lvgl_lock(1000)) {
        ui_status_init();
        bsp_lvgl_unlock();
    }
}

bool prepare_stop() { return true; }

void stop() {
    vokie_ble_stop();
    // 退出即彻底释放:NimBLE host(~60KB)+采集任务+codec/I2S DMA 全部归还。
    // 此前 host 常驻,实测退出后 largest 连续块只剩 ~34KB,小智画一个字形
    // 就 OOM panic(重启)。代价:重进 Vokie 时 BLE 重新引导,约多 1~2 秒。
    vokie_ble_free_audio();
    wb_ble_release_host();
    if (bsp_lvgl_lock(-1)) {
        ui_status_deinit();
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "stopped; ble_ok=%d", s_ble_ok);
}

void on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    vokie_ble_on_key(static_cast<bsp_btn_t>(btn), static_cast<bsp_btn_ev_t>(event), mv);
}
} // namespace

// 沿用 g_voice_app 导出名:注册表/测试无需改动,应用身份由字段决定。
extern const passport_app_t g_voice_app = {
    .id = APP_ID_VOICE,
    .name = "Vokie 麦克风",
    .en_name = "VOKIE",
    .desc = "电脑语音输入麦克风\nUP 说话 · DOWN 发送 · OK 撤销",
    .tag = "BLE",
    .theme_color = 0x67D8FF,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .prepare_stop = prepare_stop,
};
