#pragma once
// Clock + real-time helpers. Everything here is optional and degrades to a no-op
// on platforms that do not support it.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace hotline {

inline std::uint64_t now_ns() noexcept {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

// Spin briefly (cheap, lowest latency), then back off to a short sleep so an idle
// thread does not burn a whole core.
class Backoff {
 public:
  void pause() {
    if (spins_ < 200) {
      ++spins_;
      cpu_relax();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
  void reset() noexcept { spins_ = 0; }

 private:
  int spins_ = 0;
};

// SCHED_FIFO + optional core pinning for the calling thread (Linux only).
inline bool enable_realtime_for_this_thread(int priority, int cpu) {
#if defined(__linux__)
  bool ok = true;
  sched_param sp{};
  sp.sched_priority = priority;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
    std::fprintf(stderr, "[rt] SCHED_FIFO failed (run as root, or raise 'ulimit -r')\n");
    ok = false;
  }
  if (cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
      std::fprintf(stderr, "[rt] could not pin to cpu %d\n", cpu);
      ok = false;
    }
  }
  return ok;
#else
  (void)priority;
  (void)cpu;
  std::fprintf(stderr, "[rt] real-time scheduling is only implemented on Linux\n");
  return false;
#endif
}

// Lock all current and future pages in RAM so the hot path never page-faults.
inline bool lock_all_memory() {
#if defined(__linux__)
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    std::perror("[rt] mlockall");
    return false;
  }
  return true;
#else
  return false;
#endif
}

}  // namespace hotline
