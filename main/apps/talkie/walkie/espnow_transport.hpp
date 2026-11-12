// ESP-NOW 传输层（Passport OS 适配版）。
// 移植自 espnow-walkie-talkie（MIT, LeslieLeung），改动点：
//   * passport-os 的 WiFi/事件循环由 WifiManager 统一管理，这里绝不重复
//     esp_netif_init / esp_wifi_init / esp_wifi_start（重复 init 会破坏已建连接）。
//   * 射频信道策略：STA 已连 AP 时信道由 AP 决定（两台设备连同一 WiFi 即互通）；
//     未连接（户外无 AP）时设为配置信道（默认 6，两台设备默认即互通）。
//   * 关 modem 省电：C3 默认 WIFI_PS_MIN_MODEM 会在 beacon 间歇关射频，
//     ESP-NOW 帧会大量漏收（对讲断续的元凶），本地模式期间强制 WIFI_PS_NONE。
#pragma once

#include "walkie/protocol.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace walkie {

struct ReceivedFrame {
    std::array<uint8_t, protocol::kMaxWireSize> data{};
    uint16_t size{0};
    int8_t rssi{0};
};

class EspNowTransport {
public:
    EspNowTransport();
    ~EspNowTransport();

    // radio_channel 仅在 STA 未连接时生效；返回实际生效信道用 radio_channel()。
    esp_err_t initialize(uint8_t radio_channel);
    void deinitialize();
    bool receive(ReceivedFrame &frame, TickType_t wait_ticks = 0);
    esp_err_t send(const uint8_t *data, size_t size, TickType_t wait_ticks = pdMS_TO_TICKS(25));
    const protocol::DeviceId &device_id() const { return device_id_; }
    uint8_t radio_channel() const { return radio_channel_; }
    uint32_t dropped_rx_frames() const { return dropped_rx_frames_.load(std::memory_order_relaxed); }
    // 诊断：esp_now_send 调用成功/失败计数（广播无 ACK，成功≠对端收到）
    uint32_t tx_ok() const { return tx_ok_.load(std::memory_order_relaxed); }
    uint32_t tx_fail() const { return tx_fail_.load(std::memory_order_relaxed); }

private:
    static void receive_callback(const esp_now_recv_info_t *info, const uint8_t *data, int size);
    static void send_callback(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);

    QueueHandle_t receive_queue_{nullptr};
    SemaphoreHandle_t send_ready_{nullptr};
    SemaphoreHandle_t send_mutex_{nullptr};
    protocol::DeviceId device_id_{};
    uint8_t radio_channel_{6};
    std::atomic<uint32_t> dropped_rx_frames_{0};
    std::atomic<uint32_t> tx_ok_{0};
    std::atomic<uint32_t> tx_fail_{0};
    bool initialized_{false};
    static EspNowTransport *instance_;
};

}  // namespace walkie
