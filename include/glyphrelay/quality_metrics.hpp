#pragma once

#include "glyphrelay/synthetic_source.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace glyphrelay {

struct LumaMetric {
  std::uint64_t squared_error = 0;
  std::uint64_t sample_count = 0;
  double mean_squared_error = 0.0;
  double psnr_db = 0.0;
  bool lossless = false;
};

struct M0QualityMetrics {
  LumaMetric whole_frame;
  LumaMetric protected_region;
  LumaMetric comparison_region;
  double protected_minus_comparison_db = 0.0;
};

LumaMetric compute_luma_metric(std::span<const std::uint8_t> reference,
                               std::size_t reference_stride, std::span<const std::uint8_t> decoded,
                               std::size_t decoded_stride, PixelRegion region);

M0QualityMetrics compute_m0_quality_metrics(std::span<const std::uint8_t> reference,
                                            std::size_t reference_stride,
                                            std::span<const std::uint8_t> decoded,
                                            std::size_t decoded_stride);

} // namespace glyphrelay
