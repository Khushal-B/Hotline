// Hotline: low-latency real-time voice calls over UDP.
//
// Threads:
//   audio callback (driver RT thread)  mic -> capture ring, playout ring -> speaker
//   tx thread       capture ring -> Opus encode -> UDP send
//   rx thread       UDP recv -> rx ring
//   playout thread  rx ring -> jitter buffer -> Opus decode (FEC / PLC) -> playout ring
//   main thread     stats once per second
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "hotline/audio_io.hpp"
#include "hotline/jitter_buffer.hpp"
#include "hotline/opus_codec.hpp"
#include "hotline/packet.hpp"
#include "hotline/pipeline.hpp"
#include "hotline/rt.hpp"
#include "hotline/udp_socket.hpp"

using namespace hotline;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

struct Config {
  std::uint16_t local_port = 0;
  std::string peer_host;
  std::uint16_t peer_port = 0;
  bool rt = false;           // SCHED_FIFO + mlockall (Linux)
  int cpu_base = -1;         // pin tx/rx/playout to cpu_base, +1, +2
  int bitrate = 32000;       // Opus bits per second
  std::uint32_t min_depth = 2;  // jitter buffer floor, in 10 ms frames
};

void usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s <local_port> <peer_host> <peer_port> [--rt] [--cpu-base N] [--bitrate bps] [--min-depth frames]\n",
               prog);
}

bool parse_args(int argc, char** argv, Config& c) {
  if (argc < 4) return false;
  c.local_port = static_cast<std::uint16_t>(std::atoi(argv[1]));
  c.peer_host = argv[2];
  c.peer_port = static_cast<std::uint16_t>(std::atoi(argv[3]));
  for (int i = 4; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--rt") c.rt = true;
    else if (a == "--cpu-base" && i + 1 < argc) c.cpu_base = std::atoi(argv[++i]);
    else if (a == "--bitrate" && i + 1 < argc) c.bitrate = std::atoi(argv[++i]);
    else if (a == "--min-depth" && i + 1 < argc) c.min_depth = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    else return false;
  }
  return true;
}

int cpu_for(const Config& c, int idx) { return c.cpu_base >= 0 ? c.cpu_base + idx : -1; }

// ---- tx: capture ring -> encode -> UDP ----------------------------------------------------
void tx_loop(Pipeline& p, UdpSocket& sock, const sockaddr_in& peer, const Config& cfg) {
  if (cfg.rt) enable_realtime_for_this_thread(80, cpu_for(cfg, 0));
  OpusEncoderWrap enc(cfg.bitrate);
  std::uint8_t payload[kMaxPayload];
  std::uint8_t wire[kHeaderSize + kMaxPayload];
  PcmFrame frame;
  std::uint32_t seq = 0;
  Backoff backoff;

  while (!g_stop.load(std::memory_order_relaxed)) {
    if (!p.capture.try_pop(frame)) {
      backoff.pause();
      continue;
    }
    backoff.reset();
    const int n = enc.encode(frame.s, payload, static_cast<int>(sizeof payload));
    if (n <= 0) continue;
    const PacketHeader h{seq++, now_ns(), static_cast<std::uint16_t>(n)};
    const std::size_t len = serialize(h, payload, wire);
    sock.send_to(wire, len, peer);
    p.ctr.tx_packets.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---- rx: UDP -> rx ring --------------------------------------------------------------------
void rx_loop(Pipeline& p, UdpSocket& sock, const Config& cfg) {
  if (cfg.rt) enable_realtime_for_this_thread(85, cpu_for(cfg, 1));
  std::uint8_t buf[kHeaderSize + kMaxPayload + 64];  // oversize datagrams get truncated and fail parse()
  RxPacket pkt;

  while (!g_stop.load(std::memory_order_relaxed)) {
    const int n = sock.recv_from(buf, sizeof buf, nullptr);  // blocks up to 100 ms (SO_RCVTIMEO)
    if (n <= 0) continue;
    if (!parse(buf, static_cast<std::size_t>(n), pkt.hdr)) {
      p.ctr.rx_bad.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    std::memcpy(pkt.data, buf + kHeaderSize, pkt.hdr.payload_len);
    p.ctr.rx_packets.fetch_add(1, std::memory_order_relaxed);
    if (!p.rx.try_push(pkt)) p.ctr.rx_ring_drops.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---- playout: jitter buffer -> decode -> playout ring ---------------------------------------
// Paced by the speaker's clock: we only decode when the playout ring is running low. This avoids
// clock drift between our timer and the audio device without needing a resampler.
void playout_loop(Pipeline& p, const Config& cfg) {
  if (cfg.rt) enable_realtime_for_this_thread(90, cpu_for(cfg, 2));
  OpusDecoderWrap dec;
  JitterBuffer jb(cfg.min_depth, 12);
  constexpr std::size_t kPlayoutTarget = 2;  // frames kept ready for the audio callback
  RxPacket pkt;
  PcmFrame out;

  while (!g_stop.load(std::memory_order_relaxed)) {
    while (p.rx.try_pop(pkt)) jb.push(pkt.hdr.seq, pkt.hdr.send_ts_ns, pkt.data, pkt.hdr.payload_len);

    if (p.playout.size_approx() >= kPlayoutTarget) {
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      continue;
    }

    const auto r = jb.pop();
    int rc = 0;
    switch (r.status) {
      case JitterBuffer::Status::Ok: {
        rc = dec.decode(r.current->data.data(), r.current->len, out.s, false);
        const std::uint64_t now = now_ns();
        if (now >= r.current->send_ts_ns) p.latency.record_ns(now - r.current->send_ts_ns);
        break;
      }
      case JitterBuffer::Status::Missing:
        if (r.next) rc = dec.decode(r.next->data.data(), r.next->len, out.s, true);  // recover via FEC
        else rc = dec.conceal(out.s);                                                // else PLC
        break;
      case JitterBuffer::Status::Underrun:
        rc = dec.conceal(out.s);
        break;
      case JitterBuffer::Status::Prebuffering:
        rc = -1;  // silence
        break;
    }
    if (rc < 0) std::memset(out.s, 0, sizeof out.s);
    if (!p.playout.try_push(out)) p.ctr.playout_drops.fetch_add(1, std::memory_order_relaxed);

    auto& c = p.ctr;
    c.jb_lost.store(jb.lost(), std::memory_order_relaxed);
    c.jb_late.store(jb.late(), std::memory_order_relaxed);
    c.jb_underruns.store(jb.underruns(), std::memory_order_relaxed);
    c.jb_skipped.store(jb.skipped(), std::memory_order_relaxed);
    c.jb_target.store(jb.target_depth(), std::memory_order_relaxed);
  }
}

void print_stats(const Pipeline& p) {
  const auto& c = p.ctr;
  auto L = [](const std::atomic<std::uint64_t>& a) {
    return static_cast<unsigned long long>(a.load(std::memory_order_relaxed));
  };
  std::printf(
      "tx=%llu rx=%llu | jb: lost=%llu late=%llu underrun=%llu skipped=%llu depth=%llu | "
      "audio: underrun=%llu xrun=%llu | pipeline latency us p50=%llu p99=%llu p99.9=%llu\n",
      L(c.tx_packets), L(c.rx_packets), L(c.jb_lost), L(c.jb_late), L(c.jb_underruns), L(c.jb_skipped),
      L(c.jb_target), L(c.audio_underruns), L(c.audio_xruns),
      static_cast<unsigned long long>(p.latency.percentile_us(50)),
      static_cast<unsigned long long>(p.latency.percentile_us(99)),
      static_cast<unsigned long long>(p.latency.percentile_us(99.9)));
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parse_args(argc, argv, cfg)) {
    usage(argv[0]);
    return 2;
  }

  try {
    UdpSocket sock;
    if (!sock.bind(cfg.local_port)) {
      std::fprintf(stderr, "bind(%u) failed\n", cfg.local_port);
      return 1;
    }
    sock.set_recv_timeout_us(100000);
    sockaddr_in peer{};
    if (!UdpSocket::resolve(cfg.peer_host, cfg.peer_port, peer)) {
      std::fprintf(stderr, "cannot resolve %s\n", cfg.peer_host.c_str());
      return 1;
    }

    auto pipe = std::make_unique<Pipeline>();  // the only big allocation, done before any thread starts
    AudioDuplex audio(*pipe);
    if (cfg.rt) lock_all_memory();

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::thread tx(tx_loop, std::ref(*pipe), std::ref(sock), std::cref(peer), std::cref(cfg));
    std::thread rx(rx_loop, std::ref(*pipe), std::ref(sock), std::cref(cfg));
    std::thread playout(playout_loop, std::ref(*pipe), std::cref(cfg));
    audio.start();

    std::printf("Hotline running on UDP %u -> %s:%u%s. Ctrl+C to hang up.\n", cfg.local_port,
                cfg.peer_host.c_str(), cfg.peer_port, cfg.rt ? " [rt]" : "");
    while (!g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      print_stats(*pipe);
    }

    audio.stop();
    tx.join();
    rx.join();
    playout.join();
    print_stats(*pipe);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}
