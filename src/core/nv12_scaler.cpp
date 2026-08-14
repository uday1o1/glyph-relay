#include "glyphrelay/nv12_scaler.hpp"

#include <algorithm>
#include <limits>

namespace glyphrelay {
namespace {

constexpr std::size_t kMaximumDimension = 16384U;

bool valid_source(const Nv12Image &source) {
  if (source.visible_width == 0U || source.visible_height == 0U || source.coded_width == 0U ||
      source.coded_height == 0U || (source.coded_width & 1U) != 0U ||
      (source.coded_height & 1U) != 0U || source.visible_width > source.coded_width ||
      source.visible_height > source.coded_height || source.coded_width > kMaximumDimension ||
      source.coded_height > kMaximumDimension || source.pitch < source.coded_width ||
      source.coded_height > std::numeric_limits<std::size_t>::max() / source.pitch) {
    return false;
  }
  const auto luma_bytes = source.pitch * source.coded_height;
  if (source.chroma_offset != luma_bytes ||
      source.coded_height / 2U > std::numeric_limits<std::size_t>::max() / source.pitch) {
    return false;
  }
  const auto chroma_bytes = source.pitch * (source.coded_height / 2U);
  return luma_bytes <= source.bytes.size() && chroma_bytes <= source.bytes.size() - luma_bytes;
}

std::size_t even_floor(std::size_t value) { return value & ~std::size_t{1U}; }

std::size_t nearest_source(std::size_t destination, std::size_t destination_size,
                           std::size_t source_size) {
  const auto numerator = (destination * 2U + 1U) * source_size;
  return std::min(numerator / (destination_size * 2U), source_size - 1U);
}

} // namespace

Nv12ScaleResult scale_nv12_letterbox(const Nv12Image &source, std::size_t output_width,
                                     std::size_t output_height) {
  Nv12ScaleResult result;
  if (!valid_source(source)) {
    result.reason = "nv12_scale_source_invalid";
    return result;
  }
  if (output_width < 2U || output_height < 2U || (output_width & 1U) != 0U ||
      (output_height & 1U) != 0U || output_width > kMaximumDimension ||
      output_height > kMaximumDimension ||
      output_height > std::numeric_limits<std::size_t>::max() / output_width) {
    result.reason = "nv12_scale_output_invalid";
    return result;
  }

  std::size_t scaled_width = 0U;
  std::size_t scaled_height = 0U;
  if (output_width * source.visible_height <= output_height * source.visible_width) {
    scaled_width = output_width;
    scaled_height = even_floor(source.visible_height * output_width / source.visible_width);
  } else {
    scaled_height = output_height;
    scaled_width = even_floor(source.visible_width * output_height / source.visible_height);
  }
  scaled_width = std::clamp(scaled_width, std::size_t{2U}, output_width);
  scaled_height = std::clamp(scaled_height, std::size_t{2U}, output_height);
  scaled_width = even_floor(scaled_width);
  scaled_height = even_floor(scaled_height);
  const auto offset_x = even_floor((output_width - scaled_width) / 2U);
  const auto offset_y = even_floor((output_height - scaled_height) / 2U);

  auto &output = result.image;
  output.visible_width = output_width;
  output.visible_height = output_height;
  output.coded_width = output_width;
  output.coded_height = output_height;
  output.pitch = output_width;
  output.chroma_offset = output_width * output_height;
  const auto chroma_bytes = output_width * output_height / 2U;
  output.bytes.assign(output.chroma_offset + chroma_bytes, 128U);
  std::fill_n(output.bytes.begin(), static_cast<std::ptrdiff_t>(output.chroma_offset), 16U);

  for (std::size_t y = 0U; y < scaled_height; ++y) {
    const auto source_y = nearest_source(y, scaled_height, source.visible_height);
    for (std::size_t x = 0U; x < scaled_width; ++x) {
      const auto source_x = nearest_source(x, scaled_width, source.visible_width);
      output.bytes[(offset_y + y) * output.pitch + offset_x + x] =
          source.bytes[source_y * source.pitch + source_x];
    }
  }
  for (std::size_t y = 0U; y < scaled_height / 2U; ++y) {
    const auto source_y = nearest_source(y, scaled_height / 2U, (source.visible_height + 1U) / 2U);
    for (std::size_t x = 0U; x < scaled_width / 2U; ++x) {
      const auto source_x = nearest_source(x, scaled_width / 2U, (source.visible_width + 1U) / 2U);
      const auto source_offset = source.chroma_offset + source_y * source.pitch + source_x * 2U;
      const auto output_offset =
          output.chroma_offset + (offset_y / 2U + y) * output.pitch + offset_x + x * 2U;
      output.bytes[output_offset] = source.bytes[source_offset];
      output.bytes[output_offset + 1U] = source.bytes[source_offset + 1U];
    }
  }

  result.transform = {
      .source_width = source.visible_width,
      .source_height = source.visible_height,
      .output_width = output_width,
      .output_height = output_height,
      .scaled_width = scaled_width,
      .scaled_height = scaled_height,
      .offset_x = offset_x,
      .offset_y = offset_y,
  };
  result.passed = true;
  result.reason = "nv12_scale_complete";
  return result;
}

} // namespace glyphrelay
