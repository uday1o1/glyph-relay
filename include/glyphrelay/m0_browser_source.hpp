#pragma once

#include "glyphrelay/synthetic_source.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace glyphrelay {

struct M0BrowserSourceGeometry {
  static constexpr std::size_t visible_width = 1280;
  static constexpr std::size_t visible_height = 720;
  static constexpr std::size_t coded_width = 1280;
  static constexpr std::size_t coded_height = 720;
  static constexpr std::size_t frames_per_second = 30;
  static constexpr std::size_t warmup_frames = M0SourceGeometry::warmup_frames;
  static constexpr std::size_t measurement_frames = M0SourceGeometry::measurement_frames;
  static constexpr std::size_t frame_count = warmup_frames + measurement_frames;
  static constexpr std::size_t luma_bytes = coded_width * coded_height;
  static constexpr std::size_t frame_bytes = luma_bytes + luma_bytes / 2;
};

class M0BrowserSyntheticSource {
public:
  M0BrowserSyntheticSource();

  void generate(std::size_t frame_index, std::span<std::uint8_t> destination);
  std::vector<std::uint8_t> generate(std::size_t frame_index);

private:
  M0SyntheticSource source_;
  std::vector<std::uint8_t> source_frame_;
};

} // namespace glyphrelay
