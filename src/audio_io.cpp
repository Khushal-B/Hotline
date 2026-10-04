#include "hotline/audio_io.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace hotline {

namespace {
[[noreturn]] void fail(const char* what, PaError err) {
  Pa_Terminate();
  throw std::runtime_error(std::string(what) + ": " + Pa_GetErrorText(err));
}
}  // namespace

AudioDuplex::AudioDuplex(Pipeline& p) : p_(p) {
  PaError err = Pa_Initialize();
  if (err != paNoError) fail("Pa_Initialize", err);

  PaStreamParameters in{}, out{};
  in.device = Pa_GetDefaultInputDevice();
  out.device = Pa_GetDefaultOutputDevice();
  if (in.device == paNoDevice || out.device == paNoDevice) fail("no default audio device", paDeviceUnavailable);

  in.channelCount = 1;
  in.sampleFormat = paInt16;
  in.suggestedLatency = Pa_GetDeviceInfo(in.device)->defaultLowInputLatency;
  out.channelCount = 1;
  out.sampleFormat = paInt16;
  out.suggestedLatency = Pa_GetDeviceInfo(out.device)->defaultLowOutputLatency;

  err = Pa_OpenStream(&stream_, &in, &out, kSampleRate, kFrameSamples, paClipOff,
                      &AudioDuplex::callback, this);
  if (err != paNoError) fail("Pa_OpenStream", err);
}

AudioDuplex::~AudioDuplex() {
  if (stream_) Pa_CloseStream(stream_);
  Pa_Terminate();
}

void AudioDuplex::start() {
  const PaError err = Pa_StartStream(stream_);
  if (err != paNoError) fail("Pa_StartStream", err);
}

void AudioDuplex::stop() {
  if (stream_) Pa_StopStream(stream_);
}

// ---- real-time audio thread -------------------------------------------------------------
int AudioDuplex::callback(const void* input, void* output, unsigned long frames,
                          const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags flags, void* user) {
  auto* self = static_cast<AudioDuplex*>(user);
  auto& ctr = self->p_.ctr;
  const auto* mic = static_cast<const std::int16_t*>(input);
  auto* spk = static_cast<std::int16_t*>(output);

  if (flags & (paInputOverflow | paOutputUnderflow)) ctr.audio_xruns.fetch_add(1, std::memory_order_relaxed);

  if (frames != static_cast<unsigned long>(kFrameSamples)) {  // should not happen: we request fixed 480
    std::memset(spk, 0, frames * sizeof(std::int16_t));
    return paContinue;
  }

  PcmFrame in_frame;
  if (mic) {
    std::memcpy(in_frame.s, mic, sizeof in_frame.s);
    if (!self->p_.capture.try_push(in_frame)) ctr.capture_drops.fetch_add(1, std::memory_order_relaxed);
  }

  PcmFrame out_frame;
  if (self->p_.playout.try_pop(out_frame)) {
    std::memcpy(spk, out_frame.s, sizeof out_frame.s);
  } else {
    std::memset(spk, 0, sizeof out_frame.s);  // nothing decoded in time: play silence
    ctr.audio_underruns.fetch_add(1, std::memory_order_relaxed);
  }
  return paContinue;
}

}  // namespace hotline
