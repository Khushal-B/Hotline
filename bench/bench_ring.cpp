// Throughput of the lock-free SPSC ring vs a mutex-protected bounded queue (1 producer, 1 consumer).
// Run on an otherwise idle machine, ideally with the two threads on different physical cores:
//   taskset -c 2,3 ./bench_ring
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "hotline/rt.hpp"
#include "hotline/spsc_ring.hpp"

using namespace hotline;

struct MutexQueue {
  bool try_push(std::uint64_t v) {
    std::lock_guard<std::mutex> g(m);
    if (d.size() >= 1024) return false;
    d.push_back(v);
    return true;
  }
  bool try_pop(std::uint64_t& v) {
    std::lock_guard<std::mutex> g(m);
    if (d.empty()) return false;
    v = d.front();
    d.pop_front();
    return true;
  }
  std::mutex m;
  std::deque<std::uint64_t> d;
};

template <class Q>
double run(Q& q, std::uint64_t n) {
  const auto t0 = std::chrono::steady_clock::now();
  std::thread prod([&] {
    for (std::uint64_t i = 0; i < n;) {
      if (q.try_push(i)) ++i;
      else cpu_relax();
    }
  });
  std::uint64_t got = 0, v = 0, sum = 0;
  while (got < n) {
    if (q.try_pop(v)) { sum += v; ++got; }
    else cpu_relax();
  }
  prod.join();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (sum != n * (n - 1) / 2) std::fprintf(stderr, "checksum mismatch!\n");
  return static_cast<double>(n) / secs / 1e6;
}

int main() {
  constexpr std::uint64_t kN = 5'000'000;
  double best_ring = 0, best_mutex = 0;
  for (int rep = 0; rep < 5; ++rep) {
    { SpscRing<std::uint64_t, 1024> q; best_ring = std::max(best_ring, run(q, kN)); }
    { MutexQueue q; best_mutex = std::max(best_mutex, run(q, kN)); }
  }
  std::printf("SPSC ring   : %7.1f M msgs/s\n", best_ring);
  std::printf("mutex queue : %7.1f M msgs/s\n", best_mutex);
  std::printf("speedup     : %7.1fx\n", best_ring / best_mutex);
  return 0;
}
