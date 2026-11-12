// 近场对讲引擎实现，见 talkie_local.h。
// 主循环（控制任务）结构对应原版 app_main.cpp：收包处理、仲裁 tick、心跳、
// 在线超时、UI 快照；采集/播放任务对应原版 capture_task/playback_task。

#include "apps/talkie/talkie_local.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "nvs.h"

#include "walkie/adpcm.hpp"
#include "walkie/audio_jitter.hpp"
#include "walkie/espnow_transport.hpp"
#include "walkie/presence.hpp"
#include "walkie/protocol.hpp"
#include "walkie/talk_controller.hpp"

namespace {

using namespace walkie;
namespace wp = walkie::protocol;
namespace wa = walkie::audio;

constexpr char kTag[] = "talkie_local";
constexpr char kNvsNs[] = "walkie";
constexpr char kNvsChan[] = "channel";
constexpr uint8_t kRadioChannel = 6;   // 户外无 AP 时的射频信道；同 WiFi 时自动跟随 AP

constexpr EventBits_t kCaptureBit = 1U << 0;
constexpr EventBits_t kPlaybackBit = 1U << 1;
constexpr uint32_t kAudioTaskStackBytes = 6144;
constexpr uint32_t kCtrlTaskStackBytes = 5120;
constexpr uint32_t kPresenceMaintenanceMs = 250;
constexpr uint32_t kRxDiagnosticsMs = 100;

EspNowTransport g_transport;
TalkController g_controller({0});
PresenceManager g_presence;
EventGroupHandle_t g_audio_events = nullptr;
QueueHandle_t g_playback_queue = nullptr;   // EncodedAudioFrame
SemaphoreHandle_t g_audio_owner = nullptr;  // 采集/播放互斥（半双工）
SemaphoreHandle_t g_snap_lock = nullptr;
TaskHandle_t g_capture_task = nullptr;
TaskHandle_t g_playback_task = nullptr;
TaskHandle_t g_ctrl_task = nullptr;

std::atomic<uint8_t> g_logical_channel{1};
std::atomic<uint8_t> g_volume_percent{100};
std::atomic<uint32_t> g_tx_session{0};
std::atomic<uint16_t> g_tx_sequence{0};
std::atomic<bool> g_ptt{false};
std::atomic<bool> g_ptt_seen{false};       // 控制任务是否已消费当前按住状态
std::atomic<int> g_channel_step{0};   // 请求切频道的步进(+1/-1)，空闲时消费
std::atomic<bool> g_running{false};
std::atomic<int> g_last_wave{0};           // 最近一帧载荷字节数（波形柱）
std::atomic<bool> g_weak_signal{false};
std::atomic<uint32_t> g_rx_audio{0};       // 诊断：收到的音频帧总数（decode 成功即计）
std::atomic<uint32_t> g_tx_audio_frames{0}; // 诊断：本轮采集实际编码发出的音频帧数

// 缓冲放静态区：各约 2KB，避免吃任务栈。
struct AudioBuffers {
    std::array<std::array<int16_t, wp::kAudioSamples>, 3> pcm{};
    std::array<int16_t, wp::kAudioSamples> silence{};
    wa::AudioJitterBuffer jitter{};
    wa::EncodedAudioFrame incoming{};
    wa::EncodedAudioFrame frame{};
    std::array<uint8_t, wp::kAudioPayloadSize> payload{};
};
AudioBuffers g_audio_buf;

talkie_local_snapshot_t g_snap{};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

wp::Header make_header(wp::MessageType type, uint32_t session_id, uint16_t sequence) {
    wp::Header header{};
    header.type = type;
    header.logical_channel = g_logical_channel.load();
    header.sender_id = g_transport.device_id();
    header.session_id = session_id;
    header.sequence = sequence;
    header.timestamp_ms = now_ms();
    return header;
}

bool send_packet(const wp::Header &header, const uint8_t *payload, size_t payload_size) {
    std::array<uint8_t, wp::kMaxWireSize> wire{};
    size_t wire_size = 0;
    if (!wp::encode(header, payload, payload_size, wire.data(), wire.size(), wire_size)) {
        return false;
    }
    return g_transport.send(wire.data(), wire_size) == ESP_OK;
}

// 控制帧(claim/start/end)连发 3 次：ESP-NOW 无重传，广播包丢了就没人理你。
void send_control_repeated(wp::MessageType type, uint32_t session_id) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        send_packet(make_header(type, session_id, static_cast<uint16_t>(attempt)), nullptr, 0);
        if (attempt != 2) vTaskDelay(pdMS_TO_TICKS(8));
    }
}

// 一帧 PCM(320 样本) → IMA-ADPCM(160B) + 帧首 ADPCM 状态(4B)。
// 每帧都带初始状态，解码器从任意帧都能接上（ESP-NOW 无序到达的解药）。
bool encode_and_send(const int16_t *samples, wa::AdpcmState &encoder_state) {
    auto &payload = g_audio_buf.payload;
    payload[0] = static_cast<uint8_t>(static_cast<uint16_t>(encoder_state.predictor) >> 8);
    payload[1] = static_cast<uint8_t>(encoder_state.predictor);
    payload[2] = encoder_state.step_index;
    payload[3] = 0;
    if (!wa::encode_ima_adpcm(samples, wp::kAudioSamples, payload.data() + 4, wp::kAdpcmBytes,
                              encoder_state)) {
        return false;
    }
    const uint16_t sequence = g_tx_sequence.fetch_add(1);
    const bool ok = send_packet(make_header(wp::MessageType::Audio, g_tx_session.load(), sequence),
                                payload.data(), payload.size());
    if (ok) {
        g_last_wave.store(static_cast<int>(wp::kAudioPayloadSize));
        g_tx_audio_frames.fetch_add(1, std::memory_order_relaxed);
        // 二次冗余：ESP-NOW 广播无重传，丢 1 包=20ms 静音（听感就是「字间卡顿」）。
        // 同序号重发一遍，接收端 jitter 按序号去重，单包丢失几乎归零。
        // 第二发只等 4ms，忙则放弃（首发大概率已到达）。
        vTaskDelay(pdMS_TO_TICKS(4));
        send_packet(make_header(wp::MessageType::Audio, g_tx_session.load(), sequence),
                    payload.data(), payload.size());
    }
    return ok;
}

void format_device_name(const wp::DeviceId &id, std::array<char, wp::kDeviceNameSize> &name) {
    std::snprintf(name.data(), name.size(), "AP-%02X%02X", id[4], id[5]);
}

// ---------------------------------------------------------------- 采集任务

void capture_task(void *) {
    wa::AdpcmState encoder_state{};
    size_t record_index = 0;
    size_t queued = 0;
    uint32_t read_fail_streak = 0;
    uint32_t last_read_fail_log_ms = 0;
    for (;;) {
        xEventGroupWaitBits(g_audio_events, kCaptureBit, pdFALSE, pdFALSE, portMAX_DELAY);
        xSemaphoreTake(g_audio_owner, portMAX_DELAY);
        if (bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
            ESP_LOGE(kTag, "录音格式设置失败");
            xEventGroupClearBits(g_audio_events, kCaptureBit);
            xSemaphoreGive(g_audio_owner);
            continue;
        }
        // PA 常开，采集期间静音扬声器，避免啸叫
        bsp_audio_set_volume(0);
        encoder_state = {};
        record_index = 0;
        queued = 0;
        read_fail_streak = 0;
        ESP_LOGI(kTag, "采集开始");
        while ((xEventGroupGetBits(g_audio_events) & kCaptureBit) != 0) {
            // 注意：bsp_audio_read 返回 esp_err_t，ESP_OK==0。
            // 千万不能写 !bsp_audio_read(...) —— 那会把「成功」当「失败」，
            // 采集一帧都发不出去（这正是近场对讲一直没声音的根因）。
            if (bsp_audio_read(g_audio_buf.pcm[record_index].data(),
                               wp::kAudioSamples * sizeof(int16_t)) != ESP_OK) {
                ++read_fail_streak;
                // 读取失败绝不能静默空转：否则按下讲话后一帧都发不出，
                // 对端只显示「接收」然后 800ms 超时，完全没有线索。
                if (now_ms() - last_read_fail_log_ms >= 2000) {
                    ESP_LOGE(kTag, "录音读取失败 连续=%lu", (unsigned long)read_fail_streak);
                    last_read_fail_log_ms = now_ms();
                }
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            if (read_fail_streak != 0) {
                ESP_LOGI(kTag, "录音恢复(此前连续失败 %lu 次)", (unsigned long)read_fail_streak);
                read_fail_streak = 0;
            }
            // 三缓冲：读满 3 帧再发最早的，平滑 I2S DMA 的节奏抖动
            ++queued;
            if (queued >= 3) {
                const size_t ready = (record_index + 1) % g_audio_buf.pcm.size();
                encode_and_send(g_audio_buf.pcm[ready].data(), encoder_state);
            }
            record_index = (record_index + 1) % g_audio_buf.pcm.size();
        }
        ESP_LOGI(kTag, "采集停止(本次已发 %lu 帧)", (unsigned long)g_tx_audio_frames.load());
        bsp_audio_set_volume(0);
        xSemaphoreGive(g_audio_owner);
    }
}

// ---------------------------------------------------------------- 播放任务

void playback_task(void *) {
    size_t buffer_index = 0;
    uint32_t write_fail_streak = 0;
    uint32_t last_write_fail_log_ms = 0;
    int32_t lpf_y = 0;                    // 高频柔和滤波器状态（见下方低通）
    for (;;) {
        xEventGroupWaitBits(g_audio_events, kPlaybackBit, pdFALSE, pdTRUE, portMAX_DELAY);
        xSemaphoreTake(g_audio_owner, portMAX_DELAY);
        g_audio_buf.jitter.reset();
        lpf_y = 0;                        // 每次播放会话复位，避免残留值产生咔哒声
        ESP_LOGI(kTag, "播放开始");
        if (bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
            ESP_LOGE(kTag, "播放格式设置失败");
            xEventGroupClearBits(g_audio_events, kPlaybackBit);
            xSemaphoreGive(g_audio_owner);
            continue;
        }
        // 直通音量：主音量默认 80% 会把 100% 请求压到 -10dB 档，对讲听感太小
        bsp_audio_set_volume_direct(g_volume_percent.load());
        write_fail_streak = 0;
        for (;;) {
            if ((xEventGroupGetBits(g_audio_events) & kPlaybackBit) == 0) break;
            const TickType_t wait = g_audio_buf.jitter.started() ? 0 : pdMS_TO_TICKS(30);
            if (xQueueReceive(g_playback_queue, &g_audio_buf.incoming, wait) == pdTRUE) {
                g_audio_buf.jitter.push(g_audio_buf.incoming);
                while (xQueueReceive(g_playback_queue, &g_audio_buf.incoming, 0) == pdTRUE) {
                    g_audio_buf.jitter.push(g_audio_buf.incoming);
                }
            }
            const wa::JitterPopResult result = g_audio_buf.jitter.pop(g_audio_buf.frame);
            if (result == wa::JitterPopResult::Wait) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            if (result == wa::JitterPopResult::Missing) {
                // 序号缺口：补 20ms 静音，保持时间轴连续
                bsp_audio_write(g_audio_buf.silence.data(), wp::kAudioSamples * sizeof(int16_t));
                continue;
            }
            wa::AdpcmState state = g_audio_buf.frame.initial_state;
            if (wa::decode_ima_adpcm(g_audio_buf.frame.data.data(), g_audio_buf.frame.data.size(),
                                     g_audio_buf.pcm[buffer_index].data(),
                                     wp::kAudioSamples, state)) {
                // 高频柔和：一阶低通，转折 ~4kHz（α≈0.80 @16kHz）。
                // 小喇叭在 5-8kHz 有共振峰，听感「尖锐」；压掉高频不影响语音可懂度。
                // 只作用于对讲播放，不影响系统其他音频。lpf_y 在播放开始时复位。
                for (size_t i = 0; i < wp::kAudioSamples; ++i) {
                    lpf_y += (static_cast<int32_t>(g_audio_buf.pcm[buffer_index][i]) - lpf_y) * 205 / 256;
                    g_audio_buf.pcm[buffer_index][i] = static_cast<int16_t>(lpf_y);
                }
                g_last_wave.store(static_cast<int>(wp::kAudioPayloadSize));
                // 同上：esp_err_t 语义，ESP_OK==0；!写法会把成功当失败死循环重写
                while (bsp_audio_write(g_audio_buf.pcm[buffer_index].data(),
                                       wp::kAudioSamples * sizeof(int16_t)) != ESP_OK &&
                       (xEventGroupGetBits(g_audio_events) & kPlaybackBit) != 0) {
                    // 写入持续失败（设备未开/忙）绝不能静默空转
                    ++write_fail_streak;
                    if (now_ms() - last_write_fail_log_ms >= 2000) {
                        ESP_LOGE(kTag, "播放写入失败 连续=%lu", (unsigned long)write_fail_streak);
                        last_write_fail_log_ms = now_ms();
                    }
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
                if (write_fail_streak != 0) {
                    ESP_LOGI(kTag, "播放写入恢复(此前连续失败 %lu 次)",
                             (unsigned long)write_fail_streak);
                    write_fail_streak = 0;
                }
                buffer_index = (buffer_index + 1) % g_audio_buf.pcm.size();
            }
        }
        ESP_LOGI(kTag, "播放停止");
        bsp_audio_set_volume(0);
        xQueueReset(g_playback_queue);
        xSemaphoreGive(g_audio_owner);
    }
}

// ---------------------------------------------------------------- 动作执行

void apply_actions(Actions actions, TalkController &controller) {
    const uint32_t session = controller.action_session_id();
    if ((actions & SendClaim) != 0) {
        uint8_t payload[4]{};
        size_t size = 0;
        const wp::Claim claim{controller.snapshot().arbitration_value};
        if (wp::encode_claim(claim, payload, sizeof(payload), size)) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                send_packet(make_header(wp::MessageType::TalkClaim, session,
                                        static_cast<uint16_t>(attempt)),
                            payload, size);
                if (attempt != 2) vTaskDelay(pdMS_TO_TICKS(8));
            }
        }
    }
    if ((actions & SendStart) != 0) {
        g_tx_session.store(session);
        g_tx_sequence.store(0);
        send_control_repeated(wp::MessageType::TalkStart, session);
    }
    if ((actions & StartCapture) != 0) {
        xEventGroupClearBits(g_audio_events, kPlaybackBit);
        xEventGroupSetBits(g_audio_events, kCaptureBit);
    }
    if ((actions & StopCapture) != 0) xEventGroupClearBits(g_audio_events, kCaptureBit);
    if ((actions & StartPlayback) != 0) {
        xEventGroupClearBits(g_audio_events, kCaptureBit);
        xEventGroupSetBits(g_audio_events, kPlaybackBit);
    }
    if ((actions & StopPlayback) != 0) xEventGroupClearBits(g_audio_events, kPlaybackBit);
    if ((actions & SendEnd) != 0) send_control_repeated(wp::MessageType::TalkEnd, session);
    if ((actions & TalkTimedOut) != 0) ESP_LOGW(kTag, "单次发言到达 30 秒上限");
}

// ---------------------------------------------------------------- 控制任务

void save_channel(uint8_t channel) {
    nvs_handle_t handle{};
    if (nvs_open(kNvsNs, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, kNvsChan, channel);
    nvs_commit(handle);
    nvs_close(handle);
}

void publish_snapshot(const TalkController &controller) {
    const TalkState st = controller.snapshot().state;
    xSemaphoreTake(g_snap_lock, portMAX_DELAY);
    g_snap.channel = g_logical_channel.load();
    g_snap.radio_channel = static_cast<int>(g_transport.radio_channel());
    g_snap.peers = static_cast<int>(g_presence.count());
    g_snap.tx = (st == TalkState::Talking);
    g_snap.rx = (st == TalkState::Receiving);
    g_snap.busy = (st == TalkState::Busy);
    g_snap.requesting = (st == TalkState::Requesting);
    g_snap.weak = g_weak_signal.load();
    g_snap.tx_ok = static_cast<int>(g_transport.tx_ok());
    g_snap.tx_fail = static_cast<int>(g_transport.tx_fail());
    g_snap.rx_audio = static_cast<int>(g_rx_audio.load(std::memory_order_relaxed));
    std::memset(g_snap.speaker, 0, sizeof(g_snap.speaker));
    if (st == TalkState::Receiving || st == TalkState::Busy) {
        if (const Peer *peer = g_presence.find(controller.snapshot().speaker_id);
            peer != nullptr) {
            std::strncpy(g_snap.speaker, peer->heartbeat.name.data(), sizeof(g_snap.speaker) - 1);
            g_snap.weak = g_snap.weak || peer->rssi <= -78;
        }
    }
    xSemaphoreGive(g_snap_lock);
}

void ctrl_task(void *) {
    TalkController &controller = g_controller;
    const wp::DeviceId local_id = g_transport.device_id();
    std::array<char, wp::kDeviceNameSize> local_name{};
    format_device_name(local_id, local_name);

    for (;;) {
        if (!g_running.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // 每次进入本地模式都重置会话状态
        controller = TalkController(local_id);
        g_presence = {};
        uint32_t next_heartbeat_ms = 0;
        uint32_t next_presence_ms = now_ms() + kPresenceMaintenanceMs;
        uint32_t next_rx_diag_ms = now_ms() + kRxDiagnosticsMs;
        uint32_t observed_dropped_rx = 0;
        uint32_t weak_signal_until_ms = 0;
        bool was_ptt = false;
        uint32_t last_gate_log_ms = 0;        // 音频门控拒绝日志节流
        uint32_t last_rx_summary_ms = 0;      // RX 诊断汇总节流(1s)
        uint32_t last_summary_rx = 0;
        uint32_t last_summary_dropped = 0;
        ESP_LOGI(kTag, "近场对讲就绪 %s CH%u 射频信道 %u", local_name.data(),
                 g_logical_channel.load(), g_transport.radio_channel());

        while (g_running.load()) {
        const uint32_t now = now_ms();

        // 1) PTT 边沿 → 仲裁状态机
        const bool ptt = g_ptt.load();
        if (ptt != was_ptt) {
            was_ptt = ptt;
            if (ptt) {
                apply_actions(controller.ptt_pressed(now, esp_random(), esp_random()), controller);
            } else {
                apply_actions(controller.ptt_released(now), controller);
            }
        }

        // 2) 切频道请求（空闲时才切，正在讲/听时延后；支持双向步进）
        const int step = g_channel_step.exchange(0);
        if (step != 0 && controller.snapshot().state == TalkState::Idle) {
            int next = static_cast<int>(g_logical_channel.load()) + step;
            if (next < 1) next = 4;
            if (next > 4) next = 1;
            g_logical_channel.store(static_cast<uint8_t>(next));
            save_channel(next);
            g_presence = {};
            ESP_LOGI(kTag, "切到逻辑频道 CH%d", next);
        }

        // 3) 收包
        ReceivedFrame received{};
        while (g_transport.receive(received)) {
            wp::PacketView packet{};
            if (wp::decode(received.data.data(), received.size, packet) != wp::DecodeError::None ||
                packet.header.logical_channel != g_logical_channel.load() ||
                packet.header.sender_id == local_id) {
                continue;
            }
            if (packet.header.type == wp::MessageType::Heartbeat) {
                wp::Heartbeat heartbeat{};
                if (wp::decode_heartbeat(packet, heartbeat)) {
                    g_presence.observe(packet.header.sender_id, heartbeat, now, received.rssi);
                }
            } else {
                if (packet.header.type == wp::MessageType::Audio) {
                    g_rx_audio.fetch_add(1, std::memory_order_relaxed);
                }
                wp::DeviceState activity = wp::DeviceState::Idle;
                if (packet.header.type == wp::MessageType::TalkClaim) {
                    activity = wp::DeviceState::Requesting;
                } else if (packet.header.type == wp::MessageType::TalkStart ||
                           packet.header.type == wp::MessageType::Audio) {
                    activity = wp::DeviceState::Talking;
                }
                g_presence.observe_activity(packet.header.sender_id, activity, now, received.rssi);
            }

            Actions actions = NoAction;
            switch (packet.header.type) {
                case wp::MessageType::TalkClaim: {
                    wp::Claim claim{};
                    if (wp::decode_claim(packet, claim)) {
                        actions = controller.receive_claim(packet.header, claim, now);
                    }
                    break;
                }
                case wp::MessageType::TalkStart:
                    actions = controller.receive_start(packet.header, now);
                    break;
                case wp::MessageType::Audio: {
                    actions = controller.receive_audio(packet.header, now);
                    // 音频帧入队前的多重门控——任何一门不过都会静默丢帧，
                    // 这里把拒绝原因节流打出来（1s 一条），否则无从定位。
                    const auto &snap = controller.snapshot();
                    const bool state_ok = snap.state == TalkState::Receiving ||
                                          snap.state == TalkState::Busy;
                    const bool gated = !state_ok ||
                                       packet.header.payload_length != wp::kAudioPayloadSize ||
                                       packet.header.session_id != snap.session_id ||
                                       packet.header.sender_id != snap.speaker_id;
                    if (gated) {
                        if (now - last_gate_log_ms >= 1000) {
                            last_gate_log_ms = now;
                            ESP_LOGW(kTag, "音频帧被丢: len=%u 状态=%d 会话%u/%u 说话人%d",
                                     packet.header.payload_length, static_cast<int>(snap.state),
                                     packet.header.session_id, snap.session_id,
                                     static_cast<int>(packet.header.sender_id == snap.speaker_id));
                        }
                        break;
                    }
                    wa::EncodedAudioFrame audio_frame{};
                    audio_frame.session_id = packet.header.session_id;
                    audio_frame.sequence = packet.header.sequence;
                    audio_frame.initial_state.predictor = static_cast<int16_t>(
                        (static_cast<uint16_t>(packet.payload[0]) << 8) | packet.payload[1]);
                    audio_frame.initial_state.step_index = packet.payload[2];
                    if (audio_frame.initial_state.step_index <= 88 && packet.payload[3] == 0) {
                        std::memcpy(audio_frame.data.data(), packet.payload + 4,
                                    audio_frame.data.size());
                        if (xQueueSend(g_playback_queue, &audio_frame, 0) != pdTRUE) {
                            weak_signal_until_ms = now + 1500;
                            if (now - last_gate_log_ms >= 1000) {
                                last_gate_log_ms = now;
                                ESP_LOGW(kTag, "播放队列满，音频帧被丢");
                            }
                        }
                    } else if (now - last_gate_log_ms >= 1000) {
                        last_gate_log_ms = now;
                        ESP_LOGW(kTag, "ADPCM 状态非法: step=%u b3=%u",
                                 audio_frame.initial_state.step_index, packet.payload[3]);
                    }
                    break;
                }
                case wp::MessageType::TalkEnd:
                    actions = controller.receive_end(packet.header, now);
                    break;
                case wp::MessageType::Heartbeat:
                    break;
            }
            apply_actions(actions, controller);
        }

        // 4) 仲裁定时器（发言超时/远端信道释放等）
        apply_actions(controller.tick(now), controller);

        // 5) 在线超时 + 丢帧诊断
        if (now >= next_presence_ms) {
            g_presence.expire(now);
            next_presence_ms = now + kPresenceMaintenanceMs;
        }
        if (now >= next_rx_diag_ms) {
            const uint32_t dropped = g_transport.dropped_rx_frames();
            if (dropped != observed_dropped_rx) {
                observed_dropped_rx = dropped;
                weak_signal_until_ms = now + 1500;
            }
            next_rx_diag_ms = now + kRxDiagnosticsMs;
        }
        // RX 汇总（1s 节流）：接收态或有新计数时输出，直接对照
        // 「发N/M 收N」屏幕诊断，定位音频断链在收端还是发端。
        if (now - last_rx_summary_ms >= 1000) {
            last_rx_summary_ms = now;
            const uint32_t rx_a = g_rx_audio.load(std::memory_order_relaxed);
            const uint32_t dropped_now = g_transport.dropped_rx_frames();
            const auto st = controller.snapshot().state;
            if (rx_a != last_summary_rx || dropped_now != last_summary_dropped ||
                st == TalkState::Receiving || st == TalkState::Busy) {
                ESP_LOGI(kTag, "RX诊断 音频帧=%lu 丢弃=%lu 播放队列=%u 状态=%d",
                         (unsigned long)rx_a, (unsigned long)dropped_now,
                         static_cast<unsigned>(uxQueueMessagesWaiting(g_playback_queue)),
                         static_cast<int>(st));
                last_summary_rx = rx_a;
                last_summary_dropped = dropped_now;
            }
        }
        g_weak_signal.store(static_cast<int32_t>(weak_signal_until_ms - now) > 0);

        // 6) 心跳（1.8s + 随机抖动，避免多台设备同步风暴）
        if (now >= next_heartbeat_ms) {
            wp::Heartbeat heartbeat{};
            std::memcpy(heartbeat.name.data(), local_name.data(), local_name.size());
            const int soc = bsp_battery_soc();
            heartbeat.battery_percent =
                static_cast<uint8_t>(soc < 0 ? 0 : (soc > 100 ? 100 : soc));
            heartbeat.state = static_cast<wp::DeviceState>(
                static_cast<uint8_t>(controller.snapshot().state));
            heartbeat.board = wp::BoardType::AiPassport;
            uint8_t payload[16]{};
            size_t payload_size = 0;
            if (wp::encode_heartbeat(heartbeat, payload, sizeof(payload), payload_size)) {
                send_packet(make_header(wp::MessageType::Heartbeat, 0, 0), payload, payload_size);
            }
            next_heartbeat_ms = now + 1800 + (esp_random() % 401);
        }

        publish_snapshot(controller);
        vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- C 接口

extern "C" {

bool talkie_local_start(void) {
    if (g_running.exchange(true)) return true;
    g_rx_audio.store(0);          // 诊断计数每次进近场重新计

    uint8_t channel = 1;
    nvs_handle_t handle{};
    if (nvs_open(kNvsNs, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, kNvsChan, &channel);
        nvs_close(handle);
    }
    if (channel < 1 || channel > 4) channel = 1;
    g_logical_channel.store(channel);

    if (g_audio_events == nullptr) {
        g_audio_events = xEventGroupCreate();
        g_playback_queue = xQueueCreate(8, sizeof(wa::EncodedAudioFrame));
        g_audio_owner = xSemaphoreCreateMutex();
        g_snap_lock = xSemaphoreCreateMutex();
    }
    if (g_audio_events == nullptr || g_playback_queue == nullptr || g_audio_owner == nullptr ||
        g_snap_lock == nullptr) {
        g_running.store(false);
        return false;
    }

    if (g_transport.initialize(kRadioChannel) != ESP_OK) {
        ESP_LOGE(kTag, "ESP-NOW 初始化失败");
        g_running.store(false);
        return false;
    }

    // 任务常驻（阻塞在事件位/运行标志上），反复进出 app 不重复建栈。
    if (g_capture_task == nullptr &&
        xTaskCreate(capture_task, "wk_capture", kAudioTaskStackBytes, nullptr, 5,
                    &g_capture_task) != pdPASS) {
        g_capture_task = nullptr;
    }
    if (g_playback_task == nullptr &&
        xTaskCreate(playback_task, "wk_playback", kAudioTaskStackBytes, nullptr, 5,
                    &g_playback_task) != pdPASS) {
        g_playback_task = nullptr;
    }
    if (g_ctrl_task == nullptr &&
        xTaskCreate(ctrl_task, "wk_ctrl", kCtrlTaskStackBytes, nullptr, 4, &g_ctrl_task) != pdPASS) {
        g_ctrl_task = nullptr;
    }
    if (g_capture_task == nullptr || g_playback_task == nullptr || g_ctrl_task == nullptr) {
        ESP_LOGE(kTag, "任务创建失败");
        g_transport.deinitialize();
        g_running.store(false);
        return false;
    }
    return true;
}

void talkie_local_stop(void) {
    if (!g_running.exchange(false)) return;
    g_ptt.store(false);
    xEventGroupClearBits(g_audio_events, kCaptureBit | kPlaybackBit);
    // 等采集/播放任务退出内层循环并释放音频互斥量（最长 1 秒）
    for (int i = 0; i < 100; ++i) {
        if (xSemaphoreTake(g_audio_owner, 0) == pdTRUE) {
            xSemaphoreGive(g_audio_owner);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    g_transport.deinitialize();
    bsp_audio_set_volume(0);
    ESP_LOGI(kTag, "近场对讲已停止");
}

void talkie_local_set_ptt(bool pressed) { g_ptt.store(pressed); }

void talkie_local_cycle_channel(void) { talkie_local_shift_channel(1); }

// 频道步进：delta=+1 下一频道，-1 上一频道（写入 NVS，空闲时生效）
void talkie_local_shift_channel(int delta) { g_channel_step.fetch_add(delta); }

void talkie_local_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume_percent.store(static_cast<uint8_t>(percent));
    // 播放任务运行中时立即生效；未运行则下次开播时读取
}

bool talkie_local_get_snapshot(talkie_local_snapshot_t *out) {
    if (out == nullptr || g_snap_lock == nullptr) return false;
    xSemaphoreTake(g_snap_lock, portMAX_DELAY);
    *out = g_snap;
    xSemaphoreGive(g_snap_lock);
    return true;
}

int talkie_local_pop_wave(void) { return g_last_wave.exchange(0); }

}  // extern "C"
