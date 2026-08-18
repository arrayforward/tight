#pragma once

// Internal wire-format constants and big-endian conversion helpers shared by
// the tight transport translation units. Not part of the public API.

#include <cstddef>
#include <cstdint>

namespace tight::tight_detail {

inline constexpr std::uint32_t kMagic = 0x54474854U;
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 48;

// flags 字段高位：负载为 AES-256-GCM 密文（低位仍保留原语义，
// 对数据报文即数据分片数 data_cnt）
inline constexpr std::uint16_t kFlagEncrypted = 0x8000;

// 逻辑通道号打包在 reserved 字段（Data/Parity 报文中存 real_size）的高 4 位：
//   reserved = (channel << 12) | (real_size & 0x0FFF)
// real_size ≤ frag_payload = mtu - 48。默认 mtu 1350 → ≤1302，即便 mtu≤4143
// 也仍在 12 位内；默认与 buffer_pool 优化的 ≤2KB 报文场景均安全。低位 12 位
// 保留 real_size 原语义，通道最高 16 路，接收端据此识别所属通道。
inline constexpr std::uint16_t kChannelShift = 12;
inline constexpr std::uint16_t kChannelMask = 0xF000;
inline constexpr std::uint16_t kRealSizeMask = 0x0FFF;

inline std::uint8_t channel_of(std::uint16_t reserved) {
    return static_cast<std::uint8_t>((reserved & kChannelMask) >> kChannelShift);
}

inline std::uint16_t to_be16(std::uint16_t v) {
    return static_cast<std::uint16_t>(((v & 0x00FFU) << 8) | ((v & 0xFF00U) >> 8));
}
inline std::uint32_t to_be32(std::uint32_t v) {
    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8)
         | ((v & 0x00FF0000U) >> 8)  | ((v & 0xFF000000U) >> 24);
}
inline std::uint64_t to_be64(std::uint64_t v) {
    return (static_cast<std::uint64_t>(to_be32(static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFULL))) << 32)
         | to_be32(static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
}

}
