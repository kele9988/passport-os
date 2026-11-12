#include "walkie/espnow_transport.hpp"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"

namespace walkie {

namespace {
constexpr char kTag[] = "walkie_espnow";
}

EspNowTransport *EspNowTransport::instance_ = nullptr;

EspNowTransport::EspNowTransport() {
    receive_queue_ = xQueueCreate(12, sizeof(ReceivedFrame));
    send_ready_ = xSemaphoreCreateBinary();
    send_mutex_ = xSemaphoreCreateMutex();
    if (send_ready_ != nullptr) xSemaphoreGive(send_ready_);
}

EspNowTransport::~EspNowTransport() {
    deinitialize();
    if (receive_queue_ != nullptr) vQueueDelete(receive_queue_);
    if (send_ready_ != nullptr) vSemaphoreDelete(send_ready_);
    if (send_mutex_ != nullptr) vSemaphoreDelete(send_mutex_);
    if (instance_ == this) instance_ = nullptr;
}

void EspNowTransport::deinitialize() {
    if (initialized_) {
        esp_now_deinit();
        initialized_ = false;
    }
    // 省电策略还给 WiFi 的默认值（连着 AP 时 modem-sleep 是正常态）。
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

esp_err_t EspNowTransport::initialize(uint8_t radio_channel) {
    if (receive_queue_ == nullptr || send_ready_ == nullptr || send_mutex_ == nullptr ||
        radio_channel < 1 || radio_channel > 13) {
        return ESP_ERR_INVALID_STATE;
    }
    instance_ = this;
    tx_ok_.store(0);
    tx_fail_.store(0);
    dropped_rx_frames_.store(0);

    // passport-os 的 WiFi 由 WifiManager 管理：已起则复用，完全没起才自己拉起。
    // 注意区分两种「没在转」：
    //  a. 完全没 init（get_mode 报错）→ 完整 esp_wifi_init + set_mode + start；
    //  b. 已 init 未 start（冷启动且未配网时 WifiManager 只 init 不 start，
    //     get_mode 返回 OK + WIFI_MODE_NULL）→ 此时二次 esp_wifi_init 会报
    //     ESP_ERR_WIFI_INIT_STATE，只需 set_mode + start 把驱动转起来。
    wifi_mode_t mode{WIFI_MODE_NULL};
    esp_err_t error = esp_wifi_get_mode(&mode);
    if (error != ESP_OK) {
        wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
        if ((error = esp_wifi_init(&wifi_config)) != ESP_OK) return error;
        if ((error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return error;
    }
    if (mode == WIFI_MODE_NULL) {
        if ((error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return error;
        if ((error = esp_wifi_start()) != ESP_OK) return error;
    }
    // C3 默认 modem-sleep 会在 beacon 间歇关射频 → ESP-NOW 大量漏收，必须关掉。
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 信道策略：已连 AP → 跟随 AP（同 WiFi 下的设备天然同信道）；
    // 未连 → 用配置信道（户外无 AP 场景，两台设备默认信道 6 即互通）。
    // 注意：softAP 活跃时 esp_wifi_set_channel 会失败，绝不能静默吞掉——
    // 否则实际信道不受控，两台设备各自挂错信道还都显示「就绪」。
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.primary >= 1 && ap.primary <= 13) {
        radio_channel_ = ap.primary;
    } else {
        radio_channel_ = radio_channel;
        const esp_err_t ch_err = esp_wifi_set_channel(radio_channel_, WIFI_SECOND_CHAN_NONE);
        if (ch_err != ESP_OK) {
            ESP_LOGE(kTag, "esp_wifi_set_channel(%u) 失败: %s（射频被 softAP 或其他状态占用？）",
                     radio_channel_, esp_err_to_name(ch_err));
            return ch_err;
        }
    }
    // 复核：屏幕显示的「射频信道N」是配置值，这里记录的是驱动实际值。
    uint8_t primary_ch = 0;
    wifi_second_chan_t second_ch = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary_ch, &second_ch) == ESP_OK) {
        ESP_LOGI(kTag, "实际射频信道=%u (配置=%u)", primary_ch, radio_channel_);
        if (primary_ch != radio_channel_) {
            ESP_LOGE(kTag, "信道设置未生效！实际=%u 配置=%u", primary_ch, radio_channel_);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if ((error = esp_read_mac(device_id_.data(), ESP_MAC_WIFI_STA)) != ESP_OK) return error;
    if ((error = esp_now_init()) != ESP_OK) return error;
    if ((error = esp_now_register_recv_cb(receive_callback)) != ESP_OK) return error;
    if ((error = esp_now_register_send_cb(send_callback)) != ESP_OK) return error;

    esp_now_peer_info_t peer{};
    std::memset(peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    peer.channel = radio_channel_;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if ((error = esp_now_add_peer(&peer)) != ESP_OK && error != ESP_ERR_ESPNOW_EXIST) return error;

    initialized_ = true;
    ESP_LOGI(kTag, "ESP-NOW 就绪 信道=%u MAC=%02x:%02x:%02x:%02x:%02x:%02x", radio_channel_,
             device_id_[0], device_id_[1], device_id_[2], device_id_[3], device_id_[4],
             device_id_[5]);
    return ESP_OK;
}

bool EspNowTransport::receive(ReceivedFrame &frame, TickType_t wait_ticks) {
    return receive_queue_ != nullptr && xQueueReceive(receive_queue_, &frame, wait_ticks) == pdTRUE;
}

esp_err_t EspNowTransport::send(const uint8_t *data, size_t size, TickType_t wait_ticks) {
    if (!initialized_ || data == nullptr || size == 0 || size > protocol::kMaxWireSize) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(send_mutex_, wait_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(send_ready_, wait_ticks) == pdTRUE) {
        static constexpr uint8_t broadcast[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        error = esp_now_send(broadcast, data, size);
        if (error != ESP_OK) xSemaphoreGive(send_ready_);
    }
    (error == ESP_OK ? tx_ok_ : tx_fail_).fetch_add(1, std::memory_order_relaxed);
    xSemaphoreGive(send_mutex_);
    return error;
}

void EspNowTransport::receive_callback(const esp_now_recv_info_t *info, const uint8_t *data,
                                       int size) {
    if (instance_ == nullptr || info == nullptr || data == nullptr || size <= 0 ||
        size > static_cast<int>(protocol::kMaxWireSize)) {
        return;
    }
    ReceivedFrame frame{};
    frame.size = static_cast<uint16_t>(size);
    frame.rssi = info->rx_ctrl == nullptr ? 0 : info->rx_ctrl->rssi;
    std::memcpy(frame.data.data(), data, frame.size);
    if (xQueueSend(instance_->receive_queue_, &frame, 0) != pdTRUE) {
        instance_->dropped_rx_frames_.fetch_add(1, std::memory_order_relaxed);
    }
}

void EspNowTransport::send_callback(const esp_now_send_info_t *, esp_now_send_status_t) {
    if (instance_ != nullptr && instance_->send_ready_ != nullptr) {
        xSemaphoreGive(instance_->send_ready_);
    }
}

}  // namespace walkie
