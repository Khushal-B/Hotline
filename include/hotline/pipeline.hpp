#pragma once
// All shared state in one preallocated block. Every ring has exactly one producer and one consumer.
//
//   mic -> [audio cb] --capture--> [tx thread] --UDP-->  (network)
//   (network) --UDP--> [rx thread] --rx--> [playout thread: jitter buffer + decode] --playout--> [audio cb] -> speaker
#include <atomic>
#include <cstdint>

#include "hotline/common.hpp"
#include "hotline/latency_histogram.hpp"
#include "hotline/packet.hpp"
#include "hotline/spsc_ring.hpp"

namespace hotline {

struct PcmFrame {
  std::int16_t s[kFrameSamples];
};

struct RxPacket {
  PacketHeader hdr;
  std::uint8_t data[kMaxPayload];
};

struct Counters {
  std::atomic<std::uint64_t> tx_packets{0}, rx_packets{0}, rx_bad{0}, rx_ring_drops{0};
  std::atomic<std::uint64_t> capture_drops{0}, audio_xruns{0}, audio_underruns{0}, playout_drops{0};
  std::atomic<std::uint64_t> jb_lost{0}, jb_late{0}, jb_underruns{0}, jb_skipped{0}, jb_target{0};
};

struct Pipeline {
  SpscRing<PcmFrame, 16> capture;  // audio callback -> tx thread
  SpscRing<RxPacket, 64> rx;       // rx thread -> playout thread
  SpscRing<PcmFrame, 8> playout;   // playout thread -> audio callback
  LatencyHistogram latency;        // written by playout thread, read by main
  Counters ctr;
};

}  // namespace hotline
