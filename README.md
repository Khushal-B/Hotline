# Hotline

Low-latency, real-time voice calls over UDP, written from scratch in C++20.

Two people run Hotline on their machines. One speaks, the other hears it about 70 ms later.
Audio is captured in 10 ms frames, compressed with Opus, sent as UDP packets, reordered and
repaired by an adaptive jitter buffer, decoded, and played out, all without locks or heap
allocation on the real-time audio path.

## Why it is interesting

A voice call has a hard deadline: a frame of sound must reach the speaker every 10 ms, but the
network delivers packets late, out of order, or not at all. Hotline is about meeting that deadline
with the lowest possible delay.

| Problem | How Hotline handles it |
|---|---|
| Audio thread must never block | Lock-free SPSC rings, no malloc, no locks, no syscalls in the callback |
| Packets arrive out of order or late | Sequence numbers + adaptive jitter buffer (grows after underruns, shrinks when stable) |
| Packets are lost | Opus in-band FEC first, packet loss concealment (PLC) as fallback |
| Cache traffic between cores | Producer and consumer indices on separate cache lines, cached opposite index |
| Scheduler delays | Optional `SCHED_FIFO`, `mlockall`, core pinning (`--rt`, Linux) |
| Honest measurement | 1 us resolution latency histogram (p50 / p99 / p99.9), network impairment proxy |

## Architecture

```
 mic --> [audio callback] --capture ring--> [tx thread]  --> Opus encode --> UDP send
                                                                                |
 speaker <-- [audio callback] <--playout ring-- [playout thread] <--rx ring-- [rx thread] <-- UDP recv
                                                 jitter buffer + Opus decode (FEC / PLC)
```

Every ring has exactly one producer and one consumer. All memory is allocated once at startup.

## Layout

```
include/hotline/
  common.hpp             constants (48 kHz, 10 ms frames)
  rt.hpp                 clock, cpu_relax, Backoff, SCHED_FIFO / affinity / mlockall helpers
  spsc_ring.hpp          wait-free SPSC ring buffer (acquire/release, cache-line aligned)
  packet.hpp             16-byte wire header + serialize/parse
  jitter_buffer.hpp      adaptive reorder buffer (single-threaded, lock-free by ownership)
  latency_histogram.hpp  1 us histogram with percentiles
  opus_codec.hpp         RAII Opus encoder/decoder (FEC, PLC)
  pipeline.hpp           all shared rings + counters in one preallocated block
  udp_socket.hpp         POSIX / Winsock UDP wrapper
  audio_io.hpp           PortAudio full-duplex stream
src/                     main.cpp (threads + CLI), audio_io.cpp, udp_socket.cpp
tools/impair_proxy.cpp   UDP proxy that drops / delays / reorders packets
tests/                   SPSC stress test, jitter buffer unit tests
bench/bench_ring.cpp     SPSC ring vs mutex queue throughput
```

## Build

Ubuntu / Debian:
```
sudo apt install build-essential cmake pkg-config libportaudio2 portaudio19-dev libopus-dev
cmake -B build && cmake --build build -j
ctest --test-dir build
```
macOS: `brew install portaudio opus cmake pkg-config`. Windows: use vcpkg (`portaudio`, `opus`); the call
app is a draft there (see Known limitations).

ThreadSanitizer run of the ring: `cmake -B build-tsan -DHOTLINE_SANITIZE=thread && cmake --build build-tsan && ./build-tsan/test_spsc`

## Run a call

On machine A (192.168.1.10) and machine B (192.168.1.20):
```
A$ ./build/hotline 7000 192.168.1.20 7000
B$ ./build/hotline 7000 192.168.1.10 7000
```
Use headphones to avoid echo feedback (there is no echo cancellation).

Real-time mode (Linux, needs root or an rtprio limit):
```
sudo ./build/hotline 7000 192.168.1.20 7000 --rt --cpu-base 2
```
The program prints stats every second:
`tx rx | jb: lost late underrun skipped depth | audio: underrun xrun | pipeline latency p50 p99 p99.9`

## Test under a bad network

Run the proxy on machine B's side of the link, point A at the proxy, and B at A directly:
```
B$ ./build/impair_proxy 7100 127.0.0.1 7000 --loss 10 --delay 20 --jitter 40
A$ ./build/hotline 7000 <B-ip> 7100
B$ ./build/hotline 7000 <A-ip> 7000
```
Sweep `--loss` (0, 2, 5, 10, 20) and `--jitter` (0, 20, 40, 80) and record the stats line for each.

## Measuring

- **Pipeline latency** (printed by the app): time from the sender stamping a frame to the receiver pulling it
  out of the jitter buffer. This is only meaningful when both ends share a clock, so test on one machine
  through the proxy (use two terminals, two ports).
- **Mouth-to-ear latency**: capture frame (10 ms) + encode + network + jitter buffer (min-depth x 10 ms) +
  playout ring (2 x 10 ms) + audio device buffers. For a true measurement, play a click on one machine and
  record both the original and the received audio with a third device, then compare timestamps.
- **Under CPU load**: run `stress-ng --cpu $(nproc)` and compare `audio: underrun` with and without `--rt`.
- **SPSC vs mutex**: `taskset -c 2,3 ./build/bench_ring` on an idle machine, two different physical cores.

Record your own numbers here:

| Metric | Value | Setup |
|---|---|---|
| Mouth-to-ear latency (LAN) | TBD | |
| Pipeline latency p50 / p99 / p99.9 | TBD | |
| Audio underruns / hour, idle | TBD | |
| Audio underruns / minute under CPU stress, default vs `--rt` | TBD | |
| Clear audio up to loss / jitter | TBD | |
| SPSC ring vs mutex queue (M msgs/s) | TBD | |
| Opus encode + decode per frame | TBD | |

## Known limitations (first draft)

- No echo cancellation, encryption, NAT traversal, or call signaling (you type the peer address).
- Sender and receiver audio clocks drift; the jitter buffer's catch-up skip handles it crudely. A resampler would be better.
- The send timestamp is only comparable on the same host (see Measuring).
- IPv4 only. `--rt` is Linux only. Windows needs testing (Sleep granularity is coarse there).
- The audio callback copies each frame twice; a zero-copy ring (reserve/commit) is a possible optimization.

## Ideas to take it further

- `perf stat` the hot threads and report cache misses, branch mispredicts and IPC.
- Replace the sleep in the playout thread with a futex or eventfd wake-up, and compare p99.9.
- SIMD (AVX2) for the level meter / mic gain, with a scalar baseline.
- Busy-polling socket options, `SO_TIMESTAMPING` or `AF_XDP` on the receive path.
- Replace the fixed 100 us back-off with an adaptive spin-then-park strategy.
- A tiny signaling server so people can call by name.
