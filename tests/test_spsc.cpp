// Two threads push 5M integers through the ring; consumer checks strict FIFO order and no loss.
// Build with -DHOTLINE_SANITIZE=thread to have ThreadSanitizer check the memory ordering.
#include <cstdint>
#include <cstdio>
#include <thread>

#include "hotline/rt.hpp"
#include "hotline/spsc_ring.hpp"

int main() {
  using namespace hotline;
  constexpr std::uint64_t kN = 2'000'000;
  SpscRing<std::uint64_t, 1024> ring;

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kN;) {
      if (ring.try_push(i)) ++i;
      else std::this_thread::yield();
    }
  });

  std::uint64_t expect = 0;
  while (expect < kN) {
    std::uint64_t v;
    if (ring.try_pop(v)) {
      if (v != expect) {
        std::fprintf(stderr, "FAIL: expected %llu got %llu\n", static_cast<unsigned long long>(expect),
                     static_cast<unsigned long long>(v));
        producer.detach();
        return 1;
      }
      ++expect;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();

  std::uint64_t dummy;
  if (ring.try_pop(dummy)) { std::fprintf(stderr, "FAIL: ring should be empty\n"); return 1; }
  std::puts("test_spsc: OK");
  return 0;
}
