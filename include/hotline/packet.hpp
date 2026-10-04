#pragma once
// Wire format (16-byte header, then Opus payload). Little-endian hosts assumed.
//   0..1   magic 'HL'
//   2..3   payload length
//   4..7   sequence number (one per 10 ms frame)
//   8..15  sender steady-clock timestamp in ns (only comparable on the same host)
#include <cstdint>
#include <cstring>

#include "hotline/common.hpp"

namespace hotline {

inline constexpr std::uint16_t kMagic = 0x484C;
inline constexpr std::size_t kHeaderSize = 16;

struct PacketHeader {
  std::uint32_t seq;
  std::uint64_t send_ts_ns;
  std::uint16_t payload_len;
};

inline std::size_t serialize(const PacketHeader& h, const std::uint8_t* payload,
                             std::uint8_t* out) noexcept {
  const std::uint16_t magic = kMagic;
  std::memcpy(out + 0, &magic, 2);
  std::memcpy(out + 2, &h.payload_len, 2);
  std::memcpy(out + 4, &h.seq, 4);
  std::memcpy(out + 8, &h.send_ts_ns, 8);
  std::memcpy(out + kHeaderSize, payload, h.payload_len);
  return kHeaderSize + h.payload_len;
}

inline bool parse(const std::uint8_t* in, std::size_t n, PacketHeader& h) noexcept {
  if (n < kHeaderSize) return false;
  std::uint16_t magic = 0;
  std::memcpy(&magic, in, 2);
  if (magic != kMagic) return false;
  std::memcpy(&h.payload_len, in + 2, 2);
  std::memcpy(&h.seq, in + 4, 4);
  std::memcpy(&h.send_ts_ns, in + 8, 8);
  return h.payload_len <= kMaxPayload && n == kHeaderSize + h.payload_len;
}

}  // namespace hotline
