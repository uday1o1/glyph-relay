#include "glyphrelay/m0_browser_source.hpp"

#include <stdexcept>

namespace glyphrelay {

M0BrowserSyntheticSource::M0BrowserSyntheticSource()
    : source_frame_(M0SourceGeometry::frame_bytes) {}

void M0BrowserSyntheticSource::generate(std::size_t frame_index,
                                        std::span<std::uint8_t> destination) {
  if (destination.size() != M0BrowserSourceGeometry::frame_bytes) {
    throw std::invalid_argument("M0 browser destination has the wrong byte size");
  }
  source_.generate(frame_index, source_frame_);
  for (std::size_t row = 0U; row < M0BrowserSourceGeometry::visible_height; ++row) {
    const auto source_row = row * 3U / 2U;
    for (std::size_t column = 0U; column < M0BrowserSourceGeometry::visible_width; ++column) {
      const auto source_column = column * 3U / 2U;
      destination[row * M0BrowserSourceGeometry::coded_width + column] =
          source_frame_[source_row * M0SourceGeometry::coded_width + source_column];
    }
  }
  const auto source_chroma_offset = M0SourceGeometry::luma_bytes;
  const auto destination_chroma_offset = M0BrowserSourceGeometry::luma_bytes;
  for (std::size_t row = 0U; row < M0BrowserSourceGeometry::visible_height / 2U; ++row) {
    const auto source_row = row * 3U / 2U;
    for (std::size_t sample = 0U; sample < M0BrowserSourceGeometry::visible_width / 2U; ++sample) {
      const auto source_sample = sample * 3U / 2U;
      const auto source_offset =
          source_chroma_offset + source_row * M0SourceGeometry::coded_width + source_sample * 2U;
      const auto destination_offset =
          destination_chroma_offset + row * M0BrowserSourceGeometry::coded_width + sample * 2U;
      destination[destination_offset] = source_frame_[source_offset];
      destination[destination_offset + 1U] = source_frame_[source_offset + 1U];
    }
  }
}

std::vector<std::uint8_t> M0BrowserSyntheticSource::generate(std::size_t frame_index) {
  std::vector<std::uint8_t> frame(M0BrowserSourceGeometry::frame_bytes);
  generate(frame_index, frame);
  return frame;
}

} // namespace glyphrelay
