#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace glyphrelay {

struct PixelRegion {
  std::size_t x;
  std::size_t y;
  std::size_t width;
  std::size_t height;
};

struct M0SourceGeometry {
  static constexpr std::size_t visible_width = 1920;
  static constexpr std::size_t visible_height = 1080;
  static constexpr std::size_t coded_width = 1920;
  static constexpr std::size_t coded_height = 1088;
  static constexpr std::size_t frames_per_second = 30;
  static constexpr std::size_t warmup_frames = 300;
  static constexpr std::size_t measurement_frames = 1800;
  static constexpr std::size_t frame_count = warmup_frames + measurement_frames;
  static constexpr std::size_t macroblock_width = coded_width / 16;
  static constexpr std::size_t macroblock_height = coded_height / 16;
  static constexpr std::size_t map_entries = macroblock_width * macroblock_height;
  static constexpr PixelRegion protected_region{640, 384, 640, 320};
  static constexpr PixelRegion comparison_region{640, 32, 640, 320};
  static constexpr std::uint64_t seed = 0x6d305f6669786564ULL;

  static constexpr std::size_t luma_bytes = coded_width * coded_height;
  static constexpr std::size_t frame_bytes = luma_bytes + luma_bytes / 2;
};

class M0SyntheticSource {
public:
  M0SyntheticSource();

  void generate(std::size_t frame_index, std::span<std::uint8_t> destination) const;
  std::vector<std::uint8_t> generate(std::size_t frame_index) const;

private:
  std::vector<std::uint8_t> base_frame_;
};

std::vector<std::int8_t> m0_fixed_emphasis_map();

} // namespace glyphrelay
