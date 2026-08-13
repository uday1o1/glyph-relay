#include "glyphrelay/quality_metrics.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace glyphrelay {
namespace {

void validate_plane(std::span<const std::uint8_t> plane, std::size_t stride, PixelRegion region,
                    const char *name) {
  if (stride == 0U || region.width == 0U || region.height == 0U ||
      region.x + region.width > stride) {
    throw std::invalid_argument(std::string(name) + " has invalid geometry");
  }
  const auto required = (region.y + region.height - 1U) * stride + region.x + region.width;
  if (required > plane.size()) {
    throw std::invalid_argument(std::string(name) + " is smaller than the requested region");
  }
}

} // namespace

LumaMetric compute_luma_metric(std::span<const std::uint8_t> reference,
                               std::size_t reference_stride, std::span<const std::uint8_t> decoded,
                               std::size_t decoded_stride, PixelRegion region) {
  validate_plane(reference, reference_stride, region, "reference luma plane");
  validate_plane(decoded, decoded_stride, region, "decoded luma plane");

  std::uint64_t squared_error = 0;
  for (std::size_t y = region.y; y < region.y + region.height; ++y) {
    for (std::size_t x = region.x; x < region.x + region.width; ++x) {
      const auto reference_value = static_cast<int>(reference[y * reference_stride + x]);
      const auto decoded_value = static_cast<int>(decoded[y * decoded_stride + x]);
      const auto difference = reference_value - decoded_value;
      squared_error += static_cast<std::uint64_t>(difference * difference);
    }
  }

  LumaMetric result;
  result.squared_error = squared_error;
  result.sample_count = static_cast<std::uint64_t>(region.width * region.height);
  result.mean_squared_error =
      static_cast<double>(squared_error) / static_cast<double>(result.sample_count);
  result.lossless = squared_error == 0U;
  result.psnr_db = result.lossless ? std::numeric_limits<double>::infinity()
                                   : 10.0 * std::log10((255.0 * 255.0) / result.mean_squared_error);
  return result;
}

M0QualityMetrics compute_m0_quality_metrics(std::span<const std::uint8_t> reference,
                                            std::size_t reference_stride,
                                            std::span<const std::uint8_t> decoded,
                                            std::size_t decoded_stride) {
  M0QualityMetrics result;
  result.whole_frame = compute_luma_metric(
      reference, reference_stride, decoded, decoded_stride,
      {0, 0, M0SourceGeometry::visible_width, M0SourceGeometry::visible_height});
  result.protected_region = compute_luma_metric(reference, reference_stride, decoded,
                                                decoded_stride, M0SourceGeometry::protected_region);
  result.comparison_region = compute_luma_metric(
      reference, reference_stride, decoded, decoded_stride, M0SourceGeometry::comparison_region);
  if (result.protected_region.lossless && result.comparison_region.lossless) {
    result.protected_minus_comparison_db = 0.0;
  } else {
    result.protected_minus_comparison_db =
        result.protected_region.psnr_db - result.comparison_region.psnr_db;
  }
  return result;
}

} // namespace glyphrelay
