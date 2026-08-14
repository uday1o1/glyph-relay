#pragma once

#include "glyphrelay/annex_b.hpp"
#include "glyphrelay/i420.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace glyphrelay {

struct OpenH264EncoderConfig {
  std::size_t width = 1920;
  std::size_t height = 1080;
  unsigned int frames_per_second = 30;
  unsigned int target_bitrate_bps = 1'000'000;
  unsigned int maximum_bitrate_bps = 1'000'000;
  unsigned int gop_frames = 60;
  std::uint8_t level_idc = 40;
};

struct OpenH264EncodeResult {
  bool passed = false;
  std::string reason;
  AnnexBAccessUnit access_unit;
  bool keyframe = false;
  bool skipped = false;
  std::uint64_t frame_index = 0;
};

class OpenH264Encoder {
public:
  explicit OpenH264Encoder(const OpenH264EncoderConfig &config);
  ~OpenH264Encoder();

  OpenH264Encoder(OpenH264Encoder &&) noexcept;
  OpenH264Encoder &operator=(OpenH264Encoder &&) noexcept;
  OpenH264Encoder(const OpenH264Encoder &) = delete;
  OpenH264Encoder &operator=(const OpenH264Encoder &) = delete;

  bool available() const;
  const std::string &initialization_reason() const;
  OpenH264EncodeResult encode(const I420Frame &frame, bool force_idr);

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace glyphrelay
