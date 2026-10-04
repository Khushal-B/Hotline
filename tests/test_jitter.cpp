#include <cstdio>

#include "hotline/jitter_buffer.hpp"

using namespace hotline;
using Status = JitterBuffer::Status;

static int g_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } \
  } while (0)

static const std::uint8_t kData[4] = {1, 2, 3, 4};
static void push(JitterBuffer& jb, std::uint32_t seq) { jb.push(seq, 0, kData, 4); }

int main() {
  {  // prebuffering then in-order playout
    JitterBuffer jb(2, 12);
    CHECK(jb.pop().status == Status::Prebuffering);
    push(jb, 0);
    CHECK(jb.pop().status == Status::Prebuffering);  // only 1 of 2 frames buffered
    push(jb, 1);
    auto r = jb.pop();
    CHECK(r.status == Status::Ok && r.current && r.current->seq == 0);
  }
  {  // reordering is fixed
    JitterBuffer jb(2, 12);
    push(jb, 0); push(jb, 2); push(jb, 1);
    for (std::uint32_t i = 0; i < 3; ++i) {
      auto r = jb.pop();
      CHECK(r.status == Status::Ok && r.current && r.current->seq == i);
    }
  }
  {  // loss: frame 2 missing, frame 3 available for FEC
    JitterBuffer jb(2, 12);
    push(jb, 0); push(jb, 1); push(jb, 3); push(jb, 4);
    CHECK(jb.pop().status == Status::Ok);
    CHECK(jb.pop().status == Status::Ok);
    auto r = jb.pop();
    CHECK(r.status == Status::Missing && r.next && r.next->seq == 3);
    CHECK(jb.lost() == 1);
    CHECK(jb.pop().status == Status::Ok);  // 3
    CHECK(jb.pop().status == Status::Ok);  // 4
  }
  {  // late packet is dropped and counted
    JitterBuffer jb(2, 12);
    push(jb, 0); push(jb, 1);
    jb.pop();
    push(jb, 0);
    CHECK(jb.late() == 1);
  }
  {  // duplicate is counted
    JitterBuffer jb(2, 12);
    push(jb, 0); push(jb, 0);
    CHECK(jb.duplicates() == 1);
  }
  {  // underrun grows the target depth and re-prebuffers
    JitterBuffer jb(2, 12);
    push(jb, 0); push(jb, 1);
    jb.pop(); jb.pop();
    CHECK(jb.pop().status == Status::Underrun);
    CHECK(jb.target_depth() == 3);
    CHECK(jb.pop().status == Status::Prebuffering);
  }
  {  // too much queued delay: skip a frame to catch up
    JitterBuffer jb(2, 12);
    for (std::uint32_t i = 0; i < 10; ++i) push(jb, i);
    auto r = jb.pop();
    CHECK(jb.skipped() == 1);
    CHECK(r.status == Status::Ok && r.current && r.current->seq == 1);
  }
  {  // sender restart (sequence goes back to 0) is detected
    JitterBuffer jb(2, 12);
    for (std::uint32_t i = 5000; i < 5004; ++i) push(jb, i);
    for (int i = 0; i < 4; ++i) jb.pop();
    push(jb, 0); push(jb, 1);
    auto r = jb.pop();
    CHECK(r.status == Status::Ok && r.current && r.current->seq == 0);
  }
  if (g_fail == 0) std::puts("test_jitter: OK");
  return g_fail == 0 ? 0 : 1;
}
