#pragma once

#include "glyphrelay/color_conversion.hpp"

#include <cstddef>
#include <string>

namespace glyphrelay {

struct Nv12PresentationTransform {
  std::size_t source_width = 0;
  std::size_t source_height = 0;
  std::size_t output_width = 0;
  std::size_t output_height = 0;
  std::size_t scaled_width = 0;
  std::size_t scaled_height = 0;
  std::size_t offset_x = 0;
  std::size_t offset_y = 0;
};

struct Nv12ScaleResult {
  bool passed = false;
  std::string reason;
  Nv12Image image;
  Nv12PresentationTransform transform;
};

Nv12ScaleResult scale_nv12_letterbox(const Nv12Image &source, std::size_t output_width,
                                     std::size_t output_height);

} // namespace glyphrelay
