#pragma once
// RAII wrappers around libopus. All memory is allocated at construction; encode/decode never allocate.
#include <opus/opus.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "hotline/common.hpp"

namespace hotline {

class OpusEncoderWrap {
 public:
  explicit OpusEncoderWrap(int bitrate_bps) {
    int err = 0;
    enc_ = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc_)
      throw std::runtime_error(std::string("opus_encoder_create: ") + opus_strerror(err));
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_bps));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));         // embed a low-rate copy of the previous frame
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(10));  // tells the encoder how much FEC to spend
  }
  ~OpusEncoderWrap() { opus_encoder_destroy(enc_); }
  OpusEncoderWrap(const OpusEncoderWrap&) = delete;
  OpusEncoderWrap& operator=(const OpusEncoderWrap&) = delete;

  // Returns encoded bytes, or a negative Opus error code.
  int encode(const std::int16_t* pcm, std::uint8_t* out, int max_bytes) noexcept {
    return opus_encode(enc_, pcm, kFrameSamples, out, max_bytes);
  }

 private:
  OpusEncoder* enc_ = nullptr;
};

class OpusDecoderWrap {
 public:
  OpusDecoderWrap() {
    int err = 0;
    dec_ = opus_decoder_create(kSampleRate, 1, &err);
    if (err != OPUS_OK || !dec_)
      throw std::runtime_error(std::string("opus_decoder_create: ") + opus_strerror(err));
  }
  ~OpusDecoderWrap() { opus_decoder_destroy(dec_); }
  OpusDecoderWrap(const OpusDecoderWrap&) = delete;
  OpusDecoderWrap& operator=(const OpusDecoderWrap&) = delete;

  // Normal decode, or (fec=true) recover the PREVIOUS lost frame from this packet's FEC data.
  int decode(const std::uint8_t* data, int len, std::int16_t* pcm, bool fec) noexcept {
    return opus_decode(dec_, data, len, pcm, kFrameSamples, fec ? 1 : 0);
  }
  // Packet loss concealment: synthesize a frame from decoder history.
  int conceal(std::int16_t* pcm) noexcept {
    return opus_decode(dec_, nullptr, 0, pcm, kFrameSamples, 0);
  }

 private:
  OpusDecoder* dec_ = nullptr;
};

}  // namespace hotline
