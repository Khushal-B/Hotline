#pragma once
// Full-duplex mic/speaker stream (PortAudio). The callback runs on the audio driver's real-time thread:
// it only does memcpy, SPSC ring operations and relaxed atomic increments. No locks, no allocation, no syscalls.
#include <portaudio.h>

#include "hotline/pipeline.hpp"

namespace hotline {

class AudioDuplex {
 public:
  AudioDuplex(Pipeline& p);
  ~AudioDuplex();
  AudioDuplex(const AudioDuplex&) = delete;
  AudioDuplex& operator=(const AudioDuplex&) = delete;

  void start();
  void stop();

 private:
  static int callback(const void* input, void* output, unsigned long frames,
                      const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags flags, void* user);
  Pipeline& p_;
  PaStream* stream_ = nullptr;
};

}  // namespace hotline
