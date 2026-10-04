#pragma once
#include <cstddef>
#include <cstdint>

namespace hotline {

inline constexpr int kSampleRate = 48000;
inline constexpr int kFrameMs = 10;
inline constexpr int kFrameSamples = kSampleRate / 1000 * kFrameMs;  // 480 samples = 10 ms
inline constexpr std::size_t kMaxPayload = 400;                       // max Opus bytes per frame
inline constexpr std::size_t kCacheLine = 64;

}  // namespace hotline
