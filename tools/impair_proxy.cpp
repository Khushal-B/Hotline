// UDP network impairment proxy: drops, delays and reorders packets so you can test the jitter buffer.
//   impair_proxy <listen_port> <dest_host> <dest_port> [--loss pct] [--delay ms] [--jitter ms] [--seed n]
// Each packet gets delay + uniform(0, jitter) ms, so a large jitter naturally reorders packets.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include "hotline/rt.hpp"
#include "hotline/udp_socket.hpp"

using namespace hotline;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

struct Pending {
  std::uint64_t release_ns;
  std::vector<std::uint8_t> data;
  bool operator>(const Pending& o) const { return release_ns > o.release_ns; }
};
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <listen_port> <dest_host> <dest_port> [--loss pct] [--delay ms] [--jitter ms] [--seed n]\n", argv[0]);
    return 2;
  }
  const auto listen_port = static_cast<std::uint16_t>(std::atoi(argv[1]));
  const std::string dest_host = argv[2];
  const auto dest_port = static_cast<std::uint16_t>(std::atoi(argv[3]));
  double loss = 0, delay = 0, jitter = 0;
  std::uint64_t seed = 1;
  for (int i = 4; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--loss") loss = std::atof(argv[i + 1]);
    else if (a == "--delay") delay = std::atof(argv[i + 1]);
    else if (a == "--jitter") jitter = std::atof(argv[i + 1]);
    else if (a == "--seed") seed = static_cast<std::uint64_t>(std::atoll(argv[i + 1]));
  }

  UdpSocket sock;
  if (!sock.bind(listen_port)) { std::fprintf(stderr, "bind failed\n"); return 1; }
  sock.set_recv_timeout_us(1000);
  sockaddr_in dest{};
  if (!UdpSocket::resolve(dest_host, dest_port, dest)) { std::fprintf(stderr, "resolve failed\n"); return 1; }

  std::signal(SIGINT, on_signal);
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  std::priority_queue<Pending, std::vector<Pending>, std::greater<Pending>> q;
  std::uint8_t buf[2048];
  std::uint64_t seen = 0, dropped = 0, last_print = now_ns();

  std::printf("impair_proxy :%u -> %s:%u loss=%.1f%% delay=%.1fms jitter=%.1fms\n", listen_port,
              dest_host.c_str(), dest_port, loss, delay, jitter);
  while (!g_stop.load()) {
    const int n = sock.recv_from(buf, sizeof buf, nullptr);
    if (n > 0) {
      ++seen;
      if (u01(rng) * 100.0 < loss) {
        ++dropped;
      } else {
        const double d_ms = delay + u01(rng) * jitter;
        q.push({now_ns() + static_cast<std::uint64_t>(d_ms * 1e6), std::vector<std::uint8_t>(buf, buf + n)});
      }
    }
    const std::uint64_t now = now_ns();
    while (!q.empty() && q.top().release_ns <= now) {
      sock.send_to(q.top().data.data(), q.top().data.size(), dest);
      q.pop();
    }
    if (now - last_print > 5'000'000'000ULL) {
      std::printf("seen=%llu dropped=%llu (%.1f%%) queued=%zu\n", static_cast<unsigned long long>(seen),
                  static_cast<unsigned long long>(dropped), seen ? 100.0 * static_cast<double>(dropped) / static_cast<double>(seen) : 0.0, q.size());
      last_print = now;
    }
  }
  return 0;
}
