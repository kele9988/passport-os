#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

// Foreground audio apps share this storage. Release it only after their worker
// has stopped using its stack or buffers.
namespace audio_workspace {
enum class Owner { None, Radio, Xiaozhi, Muse, MusePairing, Talkie, Live };
constexpr std::size_t kRealtimeBytes = 22 * 1024;
// Same exclusive owner as the realtime stack/frame area. Xiaozhi's Opus
// half-duplex encoder/decoder, Muse reassembly and Radio compressed FIFO never
// coexist. Codec handoffs require the audio worker to be completely idle.
constexpr std::size_t kAuxBytes = 26 * 1024;
constexpr std::size_t kBytes = kRealtimeBytes + kAuxBytes;
alignas(16) inline std::uint8_t storage[kBytes];
inline std::uint8_t *aux() { return storage + kRealtimeBytes; }
// Explicit app layouts: these slices coexist only within the exclusive owner.
namespace muse {
constexpr std::size_t kReceiveBytes = 17 * 1024;
constexpr std::size_t kTransmitBytes = 4 * 1024;
constexpr std::size_t kSendScratchBytes = 3 * 1024;
inline std::uint8_t *service_scratch() { return aux() + kReceiveBytes; }
inline std::uint8_t *envelope_scratch() { return service_scratch() + kSendScratchBytes; }
constexpr std::size_t kQueueSlotBytes = 1152;
constexpr unsigned kQueueSlots = 2;
inline std::uint8_t *queue_slot(unsigned i) {
    return envelope_scratch() + kSendScratchBytes + i * kQueueSlotBytes;
}
static_assert(kReceiveBytes + kTransmitBytes <= kRealtimeBytes);
static_assert(kReceiveBytes + 2 * kSendScratchBytes + kQueueSlots * kQueueSlotBytes <= kAuxBytes);
}
namespace radio {
constexpr std::size_t kCompressedBytes = 16 * 1024;
inline std::uint8_t *stations() { return aux() + kCompressedBytes; }
constexpr std::size_t kStationBytes = kAuxBytes - kCompressedBytes;
}
// 抖音直播助手（2026-11-03 移植自 AI Passport）：0.5s @16k 单声道 PCM 环形缓冲
// + 30ms 写入分块。静态存储认领，不从堆上分配。
namespace live {
constexpr std::size_t kRingBytes = 16 * 1024;
constexpr std::size_t kChunkBytes = 1024;
inline std::uint8_t *ring() { return storage; }
inline std::uint8_t *chunk() { return aux(); }
static_assert(kRingBytes <= kRealtimeBytes, "live ring must fit realtime slice");
static_assert(kChunkBytes <= kAuxBytes, "live chunk must fit aux slice");
}
inline std::atomic<Owner> owner{Owner::None};
inline bool acquire(Owner requested) {
    Owner empty = Owner::None;
    return requested != Owner::None && owner.compare_exchange_strong(empty, requested);
}
inline bool release(Owner requested) {
    return requested != Owner::None && owner.compare_exchange_strong(requested, Owner::None);
}
}
