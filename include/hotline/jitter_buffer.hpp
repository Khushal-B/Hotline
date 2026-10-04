#pragma once
// Fixed-size adaptive jitter buffer. Owned by ONE thread (the playout thread), so it needs no locks.
//
// Packets arrive late / out of order / not at all. Frames must leave every 10 ms.
//  - push(): files a packet into slot (seq % kSlots); late and duplicate packets are counted and dropped
//  - pop():  called once per 10 ms tick, returns the next frame in sequence order
//
// Delay vs smoothness tradeoff:
//  - underrun (nothing to play)        -> target depth grows (more delay, fewer glitches)
//  - ~10 s of clean playout            -> target depth shrinks (less delay)
//  - buffer much deeper than target    -> skip a frame to catch up (sender clock faster / burst arrival)
#include <array>
#include <cstdint>
#include <cstring>

#include "hotline/common.hpp"

namespace hotline {

class JitterBuffer {
 public:
  static constexpr std::uint32_t kSlots = 64;
  static constexpr std::uint32_t kCatchUpSlack = 4;
  static constexpr std::uint32_t kShrinkAfterClean = 1000;  // 1000 frames = 10 s
  static constexpr std::uint32_t kRestartGap = 1000;        // seq far in the past => sender restarted

  enum class Status {
    Prebuffering,  // call starting or recovering from an underrun: play silence
    Ok,            // `current` holds the frame to decode
    Missing,       // frame lost: use `next` for Opus FEC if present, else packet loss concealment
    Underrun       // nothing queued at all (packets late, not lost): conceal and re-prebuffer deeper
  };

  struct Slot {
    bool valid = false;
    std::uint16_t len = 0;
    std::uint32_t seq = 0;
    std::uint64_t send_ts_ns = 0;
    std::array<std::uint8_t, kMaxPayload> data{};
  };

  // Pointers stay valid until the next push() or pop().
  struct PopResult {
    Status status;
    const Slot* current;
    const Slot* next;
  };

  explicit JitterBuffer(std::uint32_t min_depth = 2, std::uint32_t max_depth = 12) noexcept
      : min_depth_(min_depth), max_depth_(max_depth), target_depth_(min_depth) {}

  void push(std::uint32_t seq, std::uint64_t send_ts_ns, const std::uint8_t* data,
            std::uint16_t len) noexcept {
    if (len > kMaxPayload) return;
    ++received_;
    if (!started_) {
      reset_to(seq);
      started_ = true;
    } else if (seq < next_seq_) {
      if (next_seq_ - seq > kRestartGap) {
        reset_to(seq);  // sender restarted
      } else {
        ++late_;  // already played or already declared lost
        return;
      }
    } else if (seq - next_seq_ >= kSlots) {
      reset_to(seq);  // jumped too far ahead to buffer
    }

    Slot& s = slots_[seq % kSlots];
    if (s.valid && s.seq == seq) {
      ++duplicates_;
      return;
    }
    s.valid = true;
    s.seq = seq;
    s.len = len;
    s.send_ts_ns = send_ts_ns;
    std::memcpy(s.data.data(), data, len);
    if (seq > highest_) highest_ = seq;
  }

  PopResult pop() noexcept {
    if (!started_) return {Status::Prebuffering, nullptr, nullptr};

    if (prebuffering_) {
      if (highest_ + 1 < next_seq_ + target_depth_) return {Status::Prebuffering, nullptr, nullptr};
      prebuffering_ = false;
    }

    if (highest_ < next_seq_) {  // nothing at or beyond the frame we need
      ++underruns_;
      if (target_depth_ < max_depth_) ++target_depth_;
      prebuffering_ = true;
      clean_ = 0;
      return {Status::Underrun, nullptr, nullptr};
    }

    if (highest_ - next_seq_ > target_depth_ + kCatchUpSlack) {  // too much queued delay: skip one
      slots_[next_seq_ % kSlots].valid = false;
      ++next_seq_;
      ++skipped_;
    }

    Slot& cur = slots_[next_seq_ % kSlots];
    PopResult r{Status::Ok, nullptr, nullptr};
    if (cur.valid && cur.seq == next_seq_) {
      r.current = &cur;
      cur.valid = false;  // data stays readable until the next push()
      ++clean_;
    } else {
      ++lost_;
      clean_ = 0;
      r.status = Status::Missing;
      const Slot& nx = slots_[(next_seq_ + 1) % kSlots];
      if (nx.valid && nx.seq == next_seq_ + 1) r.next = &nx;
    }
    ++next_seq_;

    if (clean_ >= kShrinkAfterClean && target_depth_ > min_depth_) {
      --target_depth_;
      clean_ = 0;
    }
    return r;
  }

  std::uint32_t target_depth() const noexcept { return target_depth_; }
  std::uint64_t received() const noexcept { return received_; }
  std::uint64_t lost() const noexcept { return lost_; }
  std::uint64_t late() const noexcept { return late_; }
  std::uint64_t duplicates() const noexcept { return duplicates_; }
  std::uint64_t underruns() const noexcept { return underruns_; }
  std::uint64_t skipped() const noexcept { return skipped_; }

 private:
  void reset_to(std::uint32_t seq) noexcept {
    for (auto& s : slots_) s.valid = false;
    next_seq_ = seq;
    highest_ = seq;
    prebuffering_ = true;
    clean_ = 0;
  }

  std::array<Slot, kSlots> slots_{};
  std::uint32_t min_depth_;
  std::uint32_t max_depth_;
  std::uint32_t target_depth_;
  std::uint32_t next_seq_ = 0;
  std::uint32_t highest_ = 0;
  std::uint32_t clean_ = 0;
  bool started_ = false;
  bool prebuffering_ = true;
  std::uint64_t received_ = 0, lost_ = 0, late_ = 0, duplicates_ = 0, underruns_ = 0, skipped_ = 0;
};

}  // namespace hotline
