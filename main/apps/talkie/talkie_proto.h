#pragma once

// 对讲机协议常量。必须与服务器 talkie_server/relay.py 逐字节一致。
//
// 帧格式(TCP, 大端):
//     [0..1] magic  'T' 'B'
//     [2]    type
//     [3..4] payload 长度
//     [5]    seq
//     [6..]  payload
//
// 设备端只有 6 字节头 + 最多 ~512 字节 Opus,一帧远小于 MSS,不会分片。

#include <cstddef>
#include <cstdint>

namespace talkie {
namespace proto {

constexpr uint8_t kMagic0 = 'T';
constexpr uint8_t kMagic1 = 'B';
constexpr size_t kHeaderLen = 6;
constexpr size_t kMaxFrame = 512;

constexpr uint8_t kTypeJoin = 0x01;
constexpr uint8_t kTypeJoinAck = 0x81;
constexpr uint8_t kTypeAudio = 0x02;
constexpr uint8_t kTypePing = 0x03;
constexpr uint8_t kTypePong = 0x83;
constexpr uint8_t kTypePeers = 0x84;
constexpr uint8_t kTypeTalkReq = 0x05;
constexpr uint8_t kTypeTalk = 0x85;

constexpr uint8_t kResultOk = 0;
constexpr uint8_t kResultFull = 1;
constexpr uint8_t kResultVersion = 2;
constexpr uint8_t kResultToken = 3;

inline void put_header(uint8_t *out, uint8_t type, uint16_t len, uint8_t seq) {
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = type;
    out[3] = static_cast<uint8_t>(len >> 8);
    out[4] = static_cast<uint8_t>(len & 0xFF);
    out[5] = seq;
}

inline uint16_t get_u16(const uint8_t *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t get_u32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

}  // namespace proto
}  // namespace talkie
