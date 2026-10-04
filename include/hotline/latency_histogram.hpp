#pragma once
// 1 microsecond-resolution histogram, 0..100 ms. One writer thread, any reader.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hotline {

class LatencyHistogram {
 public:
  explicit LatencyHistogram(std::size_t max_us = 100000) : buckets_(max_us + 1) {}

  void record_ns(std::uint64_t ns) noexcept {
    std::size_t us = static_cast<std::size_t>(ns / 1000);
    if (us >= buckets_.size()) us = buckets_.size() - 1;
    auto& b = buckets_[us];
    b.store(b.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // single writer
  }

  std::uint64_t count() const noexcept {
    std::uint64_t total = 0;
    for (const auto& b : buckets_) total += b.load(std::memory_order_relaxed);
    return total;
  }

  std::uint64_t percentile_us(double p) const noexcept {
    const std::uint64_t total = count();
    if (total == 0) return 0;
    std::uint64_t target = static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(total)));
    if (target == 0) target = 1;
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
      acc += buckets_[i].load(std::memory_order_relaxed);
      if (acc >= target) return i;
    }
    return buckets_.size() - 1;
  }

 private:
  std::vector<std::atomic<std::uint64_t>> buckets_;
};

}  // namespace hotline
