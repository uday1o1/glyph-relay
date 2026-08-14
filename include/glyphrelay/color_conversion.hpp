#pragma once

#include "glyphrelay/capture.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace glyphrelay {

enum class ColorRange { limited, full };
enum class ColorConversionBackend { automatic, scalar, simd };

struct Nv12Image {
  std::size_t visible_width = 0;
  std::size_t visible_height = 0;
  std::size_t coded_width = 0;
  std::size_t coded_height = 0;
  std::size_t pitch = 0;
  std::size_t chroma_offset = 0;
  std::vector<std::uint8_t> bytes;
};

struct ColorConversionResult {
  bool passed = false;
  std::string reason;
  ColorConversionBackend backend = ColorConversionBackend::scalar;
  Nv12Image image;
};

bool color_conversion_simd_available();
ColorConversionResult
convert_bgra_or_rgba_to_nv12(const CapturedFrame &frame, ColorRange range,
                             ColorConversionBackend backend = ColorConversionBackend::automatic);
std::string_view color_range_name(ColorRange range);
std::string_view color_conversion_backend_name(ColorConversionBackend backend);

} // namespace glyphrelay
