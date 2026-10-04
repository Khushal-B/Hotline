#pragma once
// Wait-free single-producer / single-consumer ring buffer.
//  - power-of-two capacity, index masking instead of modulo
//  - acquire/release only (no seq_cst)
//  - producer-owned and consumer-owned indices live on separate cache lines
//  - each side caches the other side's index, so the shared cache line is only
//    re-read when the ring looks full/empty (less cross-core coherence traffic)
//  - storage is inline, so there is never a heap allocation after construction
#include <array>
#include <atomic>
#include <cstddef>

#include "hotline/common.hpp"

namespace hotline {

template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two");
  static constexpr std::size_t kMask = Capacity - 1;

 public:
  // Producer thread only.
  bool try_push(const T& v) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head - cached_tail_ == Capacity) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head - cached_tail_ == Capacity) return false;  // full
    }
    buf_[head & kMask] = v;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only.
  bool try_pop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) return false;  // empty
    }
    out = buf_[tail & kMask];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Safe from any thread; the value may be stale by the time you use it.
  std::size_t size_approx() const noexcept {
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t n = head - tail;
    return n > Capacity ? Capacity : n;
  }

 private:
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by producer
  std::size_t cached_tail_{0};                            // producer-local
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by consumer
  std::size_t cached_head_{0};                            // consumer-local
  alignas(kCacheLine) std::array<T, Capacity> buf_{};
};

}  // namespace hotline
