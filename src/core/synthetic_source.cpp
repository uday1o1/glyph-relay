#include "glyphrelay/synthetic_source.hpp"

#include <algorithm>
#include <stdexcept>

namespace glyphrelay {
namespace {

std::uint64_t mix(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

void fill_luma(std::span<std::uint8_t> frame, PixelRegion region, std::uint8_t value) {
  if (region.x + region.width > M0SourceGeometry::coded_width ||
      region.y + region.height > M0SourceGeometry::coded_height) {
    throw std::out_of_range("luma rectangle exceeds the coded frame");
  }
  for (std::size_t row = region.y; row < region.y + region.height; ++row) {
    auto *begin = frame.data() + row * M0SourceGeometry::coded_width + region.x;
    std::fill_n(begin, region.width, value);
  }
}

void draw_border(std::span<std::uint8_t> frame, PixelRegion region, std::uint8_t value) {
  fill_luma(frame, {region.x, region.y, region.width, 2}, value);
  fill_luma(frame, {region.x, region.y + region.height - 2, region.width, 2}, value);
  fill_luma(frame, {region.x, region.y, 2, region.height}, value);
  fill_luma(frame, {region.x + region.width - 2, region.y, 2, region.height}, value);
}

void draw_glyph(std::span<std::uint8_t> frame, std::size_t origin_x, std::size_t origin_y,
                std::uint64_t identity) {
  const std::uint64_t bits = mix(identity);
  for (std::size_t row = 0; row < 10U; ++row) {
    for (std::size_t column = 0; column < 6U; ++column) {
      const auto bit = (row * 6U + column) % 64U;
      const bool edge = (column == 0U || column == 5U) && ((identity + row) % 3U == 0U);
      if (((bits >> bit) & 1U) != 0U || edge) {
        fill_luma(frame, {origin_x + column, origin_y + row, 1, 1}, 218U);
      }
    }
  }
}

void draw_text_region(std::span<std::uint8_t> frame, PixelRegion region) {
  fill_luma(frame, region, 40U);
  draw_border(frame, region, 150U);
  for (std::size_t line = 0; line < 12U; ++line) {
    const auto line_y = region.y + 16U + line * 24U;
    fill_luma(frame, {region.x + 12U, line_y + 13U, region.width - 24U, 1U}, 54U);
    for (std::size_t glyph = 0; glyph < 59U; ++glyph) {
      const auto glyph_x = region.x + 16U + glyph * 10U;
      draw_glyph(frame, glyph_x, line_y,
                 M0SourceGeometry::seed ^ (line * 0x10001U) ^ (glyph * 0x9e37U));
    }
  }
}

void draw_dynamic_region(std::span<std::uint8_t> frame, PixelRegion region,
                         std::size_t frame_index) {
  const auto active_line = (frame_index / 45U) % 12U;
  const auto active_y = region.y + 15U + active_line * 24U;
  fill_luma(frame, {region.x + 8U, active_y, 3U, 13U}, 112U);
  if ((frame_index / 15U) % 2U == 0U) {
    const auto caret_column = (frame_index / 30U) % 55U;
    fill_luma(frame, {region.x + 18U + caret_column * 10U, active_y, 2U, 12U}, 235U);
  }
}

void repeat_padding_row(std::span<std::uint8_t> frame) {
  const auto *source =
      frame.data() + (M0SourceGeometry::visible_height - 1U) * M0SourceGeometry::coded_width;
  for (std::size_t row = M0SourceGeometry::visible_height; row < M0SourceGeometry::coded_height;
       ++row) {
    std::copy_n(source, M0SourceGeometry::coded_width,
                frame.data() + row * M0SourceGeometry::coded_width);
  }
}

} // namespace

M0SyntheticSource::M0SyntheticSource()
    : base_frame_(M0SourceGeometry::frame_bytes, static_cast<std::uint8_t>(128U)) {
  std::fill_n(base_frame_.begin(), M0SourceGeometry::luma_bytes, static_cast<std::uint8_t>(30U));
  fill_luma(base_frame_, {0, 0, M0SourceGeometry::visible_width, 28}, 54U);
  fill_luma(base_frame_, {32, 32, 560, 1008}, 35U);
  fill_luma(base_frame_, {1328, 32, 560, 1008}, 37U);
  for (std::size_t row = 80; row < 1040; row += 64U) {
    fill_luma(base_frame_, {64, row, 480, 1}, 48U);
    fill_luma(base_frame_, {1376, row, 464, 1}, 50U);
  }
  draw_text_region(base_frame_, M0SourceGeometry::comparison_region);
  draw_text_region(base_frame_, M0SourceGeometry::protected_region);
  fill_luma(base_frame_, {640, 736, 640, 304}, 42U);
  draw_border(base_frame_, {640, 736, 640, 304}, 96U);
  repeat_padding_row(base_frame_);
}

void M0SyntheticSource::generate(std::size_t frame_index,
                                 std::span<std::uint8_t> destination) const {
  if (frame_index >= M0SourceGeometry::frame_count) {
    throw std::out_of_range("M0 synthetic frame index exceeds the frozen sequence");
  }
  if (destination.size() != M0SourceGeometry::frame_bytes) {
    throw std::invalid_argument("M0 synthetic destination has the wrong byte size");
  }
  std::copy(base_frame_.begin(), base_frame_.end(), destination.begin());
  draw_dynamic_region(destination, M0SourceGeometry::comparison_region, frame_index);
  draw_dynamic_region(destination, M0SourceGeometry::protected_region, frame_index);

  const auto indicator_x = 664U + (frame_index * 7U) % 568U;
  fill_luma(destination, {indicator_x, 760U, 16U, 16U}, 196U);
  const auto pulse =
      static_cast<std::uint8_t>(64U + (mix(M0SourceGeometry::seed ^ frame_index) % 96U));
  fill_luma(destination, {672U, 800U, 576U, 4U}, pulse);
  repeat_padding_row(destination);
}

std::vector<std::uint8_t> M0SyntheticSource::generate(std::size_t frame_index) const {
  std::vector<std::uint8_t> frame(M0SourceGeometry::frame_bytes);
  generate(frame_index, frame);
  return frame;
}

std::vector<std::int8_t> m0_fixed_emphasis_map() {
  std::vector<std::int8_t> map(M0SourceGeometry::map_entries, 0);
  const auto first_x = M0SourceGeometry::protected_region.x / 16U;
  const auto first_y = M0SourceGeometry::protected_region.y / 16U;
  const auto block_width = M0SourceGeometry::protected_region.width / 16U;
  const auto block_height = M0SourceGeometry::protected_region.height / 16U;
  for (std::size_t y = first_y; y < first_y + block_height; ++y) {
    for (std::size_t x = first_x; x < first_x + block_width; ++x) {
      map[y * M0SourceGeometry::macroblock_width + x] = 4;
    }
  }
  return map;
}

} // namespace glyphrelay
