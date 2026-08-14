#include "glyphrelay/cuda_preprocess.hpp"

#include "glyphrelay/cuda_context.hpp"

#include <cuda_runtime.h>
#include <nvtx3/nvToolsExt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace glyphrelay {
namespace {

constexpr std::uint64_t kTemporalResetGapNs = 200'000'000U;
constexpr int kScharrScale = 4'080;
constexpr std::size_t kMaximumOverrideRectangles = 64U;

enum class TimingEvent : std::size_t {
  total_start,
  upload_start,
  upload_end,
  conversion_start,
  conversion_end,
  features_start,
  features_end,
  temporal_start,
  temporal_end,
  morphology_start,
  morphology_end,
  reduction_start,
  reduction_end,
  map_copy_start,
  map_copy_end,
  total_end,
  count,
};

constexpr std::size_t event_index(TimingEvent event) { return static_cast<std::size_t>(event); }

bool checked_product(std::size_t left, std::size_t right, std::size_t &result) {
  if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

std::string cuda_failure(const char *operation, cudaError_t status) {
  return std::string(operation) + ":" + cudaGetErrorName(status);
}

class NvtxRange {
public:
  explicit NvtxRange(const char *name) { nvtxRangePushA(name); }
  ~NvtxRange() { nvtxRangePop(); }
  NvtxRange(const NvtxRange &) = delete;
  NvtxRange &operator=(const NvtxRange &) = delete;
};

struct DeviceTileFeature {
  double gradient_density;
  double local_contrast;
  double edge_pair_density;
  double small_structure_density;
  double raw_score;
  double temporal_stability;
  double current_score;
  double filtered_score;
  std::uint8_t active;
  std::uint8_t automatic_level;
};

struct DeviceConfiguration {
  double gradient_weight;
  double contrast_weight;
  double edge_pair_weight;
  double small_structure_weight;
  double entry_threshold;
  double exit_threshold;
  double previous_score_coefficient;
  std::size_t dilation_radius_tiles;
};

__device__ std::uint64_t round_divide_ties_even_unsigned(std::uint64_t numerator,
                                                         std::uint64_t denominator) {
  auto quotient = numerator / denominator;
  const auto remainder = numerator % denominator;
  if (remainder > denominator / 2U || (remainder * 2U == denominator && (quotient & 1U) != 0U)) {
    ++quotient;
  }
  return quotient;
}

__device__ std::int64_t round_divide_ties_even_signed(std::int64_t numerator,
                                                      std::int64_t denominator) {
  const bool negative = numerator < 0;
  const auto magnitude = static_cast<std::uint64_t>(negative ? -numerator : numerator);
  const auto result = static_cast<std::int64_t>(
      round_divide_ties_even_unsigned(magnitude, static_cast<std::uint64_t>(denominator)));
  return negative ? -result : result;
}

__device__ std::uint8_t clamp_code(std::int64_t value) {
  return static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
}

__device__ std::uint8_t canonical_code(std::uint8_t code, bool limited_range) {
  if (!limited_range) {
    return code;
  }
  const auto clamped = static_cast<std::uint64_t>(code < 16U ? 16U : code > 235U ? 235U : code);
  return static_cast<std::uint8_t>(round_divide_ties_even_unsigned((clamped - 16U) * 255U, 219U));
}

__device__ void read_rgb(const std::uint8_t *source, std::size_t pitch, std::size_t x,
                         std::size_t y, bool rgba, std::uint8_t &red, std::uint8_t &green,
                         std::uint8_t &blue) {
  const auto offset = y * pitch + x * 4U;
  const auto first = source[offset];
  green = source[offset + 1U];
  const auto third = source[offset + 2U];
  red = rgba ? first : third;
  blue = rgba ? third : first;
}

__global__ void convert_luma_kernel(const std::uint8_t *source, std::size_t source_pitch,
                                    std::uint8_t *nv12, std::size_t nv12_pitch,
                                    std::size_t visible_width, std::size_t visible_height,
                                    std::size_t coded_width, std::size_t coded_height, bool rgba,
                                    bool limited_range) {
  const auto x = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const auto y = static_cast<std::size_t>(blockIdx.y) * blockDim.y + threadIdx.y;
  if (x >= coded_width || y >= coded_height) {
    return;
  }
  const auto source_x = x < visible_width ? x : visible_width - 1U;
  const auto source_y = y < visible_height ? y : visible_height - 1U;
  std::uint8_t red = 0U;
  std::uint8_t green = 0U;
  std::uint8_t blue = 0U;
  read_rgb(source, source_pitch, source_x, source_y, rgba, red, green, blue);
  constexpr std::int64_t denominator = 10'000LL * 255LL;
  const auto scale = limited_range ? 219LL : 255LL;
  const auto offset = limited_range ? 16LL : 0LL;
  const auto weighted = 2'126LL * red + 7'152LL * green + 722LL * blue;
  nv12[y * nv12_pitch + x] =
      clamp_code(offset + round_divide_ties_even_signed(scale * weighted, denominator));
}

__global__ void convert_chroma_kernel(const std::uint8_t *source, std::size_t source_pitch,
                                      std::uint8_t *chroma, std::size_t nv12_pitch,
                                      std::size_t visible_width, std::size_t visible_height,
                                      std::size_t coded_width, std::size_t coded_height, bool rgba,
                                      bool limited_range) {
  const auto chroma_x = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const auto chroma_y = static_cast<std::size_t>(blockIdx.y) * blockDim.y + threadIdx.y;
  const auto x = chroma_x * 2U;
  const auto y = chroma_y * 2U;
  if (x >= coded_width || y >= coded_height) {
    return;
  }
  std::uint64_t red_sum = 0U;
  std::uint64_t green_sum = 0U;
  std::uint64_t blue_sum = 0U;
  for (std::size_t delta_y = 0U; delta_y < 2U; ++delta_y) {
    for (std::size_t delta_x = 0U; delta_x < 2U; ++delta_x) {
      std::uint8_t red = 0U;
      std::uint8_t green = 0U;
      std::uint8_t blue = 0U;
      read_rgb(source, source_pitch, x + delta_x < visible_width ? x + delta_x : visible_width - 1U,
               y + delta_y < visible_height ? y + delta_y : visible_height - 1U, rgba, red, green,
               blue);
      red_sum += red;
      green_sum += green;
      blue_sum += blue;
    }
  }
  const auto scale = limited_range ? 224LL : 255LL;
  constexpr std::int64_t denominator = 1'000'000LL * 255LL * 4LL;
  const auto cb = -114'572LL * static_cast<std::int64_t>(red_sum) -
                  385'428LL * static_cast<std::int64_t>(green_sum) +
                  500'000LL * static_cast<std::int64_t>(blue_sum);
  const auto cr = 500'000LL * static_cast<std::int64_t>(red_sum) -
                  454'153LL * static_cast<std::int64_t>(green_sum) -
                  45'847LL * static_cast<std::int64_t>(blue_sum);
  const auto offset = chroma_y * nv12_pitch + x;
  chroma[offset] = clamp_code(128LL + round_divide_ties_even_signed(scale * cb, denominator));
  chroma[offset + 1U] = clamp_code(128LL + round_divide_ties_even_signed(scale * cr, denominator));
}

__global__ void canonical_kernel(const std::uint8_t *luma, std::size_t luma_pitch,
                                 std::uint8_t *canonical, std::size_t width, std::size_t height,
                                 bool limited_range) {
  const auto x = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const auto y = static_cast<std::size_t>(blockIdx.y) * blockDim.y + threadIdx.y;
  if (x < width && y < height) {
    canonical[y * width + x] = canonical_code(luma[y * luma_pitch + x], limited_range);
  }
}

__global__ void gradient_kernel(const std::uint8_t *canonical, int *gradient_x, int *gradient_y,
                                std::uint8_t *high, std::size_t width, std::size_t height) {
  const auto x = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const auto y = static_cast<std::size_t>(blockIdx.y) * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) {
    return;
  }
  constexpr int scharr_x[3][3] = {{-3, 0, 3}, {-10, 0, 10}, {-3, 0, 3}};
  constexpr int scharr_y[3][3] = {{-3, -10, -3}, {0, 0, 0}, {3, 10, 3}};
  int horizontal = 0;
  int vertical = 0;
  for (int kernel_y = 0; kernel_y < 3; ++kernel_y) {
    const auto candidate_y = static_cast<std::int64_t>(y) + kernel_y - 1;
    const auto source_y = static_cast<std::size_t>(
        candidate_y < 0                                    ? 0
        : candidate_y >= static_cast<std::int64_t>(height) ? static_cast<std::int64_t>(height - 1U)
                                                           : candidate_y);
    for (int kernel_x = 0; kernel_x < 3; ++kernel_x) {
      const auto candidate_x = static_cast<std::int64_t>(x) + kernel_x - 1;
      const auto source_x = static_cast<std::size_t>(
          candidate_x < 0                                   ? 0
          : candidate_x >= static_cast<std::int64_t>(width) ? static_cast<std::int64_t>(width - 1U)
                                                            : candidate_x);
      const auto sample = static_cast<int>(canonical[source_y * width + source_x]);
      horizontal += sample * scharr_x[kernel_y][kernel_x];
      vertical += sample * scharr_y[kernel_y][kernel_x];
    }
  }
  const auto index = y * width + x;
  gradient_x[index] = horizontal;
  gradient_y[index] = vertical;
  const auto magnitude = static_cast<std::int64_t>(abs(horizontal) + abs(vertical)) * 100LL;
  high[index] = magnitude > static_cast<std::int64_t>(kScharrScale) * 12LL ? 1U : 0U;
}

__device__ bool high_component_device(int component) {
  return static_cast<std::int64_t>(abs(component)) * 100LL >
         static_cast<std::int64_t>(kScharrScale) * 12LL;
}

__device__ double clamp01_device(double value) {
  return value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
}

__global__ void tile_feature_kernel(const std::uint8_t *canonical, const int *gradient_x,
                                    const int *gradient_y, const std::uint8_t *high,
                                    DeviceTileFeature *features, std::size_t width,
                                    std::size_t height, std::size_t tile_width,
                                    std::size_t tile_count, DeviceConfiguration configuration) {
  const auto tile_index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (tile_index >= tile_count) {
    return;
  }
  const auto tile_x = tile_index % tile_width;
  const auto tile_y = tile_index / tile_width;
  const auto left = tile_x * kSaliencyTileSize;
  const auto top = tile_y * kSaliencyTileSize;
  const auto right = left + kSaliencyTileSize < width ? left + kSaliencyTileSize : width;
  const auto bottom = top + kSaliencyTileSize < height ? top + kSaliencyTileSize : height;
  const auto tile_pixel_count = (right - left) * (bottom - top);
  std::uint8_t sorted_codes[64];
  std::size_t sorted_count = 0U;
  std::size_t high_count = 0U;
  std::size_t small_count = 0U;
  std::size_t eligible_anchors = 0U;
  std::size_t qualifying_anchors = 0U;
  for (auto y = top; y < bottom; ++y) {
    for (auto x = left; x < right; ++x) {
      const auto index = y * width + x;
      const auto value = canonical[index];
      auto insertion = sorted_count;
      while (insertion > 0U && sorted_codes[insertion - 1U] > value) {
        sorted_codes[insertion] = sorted_codes[insertion - 1U];
        --insertion;
      }
      sorted_codes[insertion] = value;
      ++sorted_count;
      if (high[index] == 0U) {
        continue;
      }
      ++high_count;
      std::size_t horizontal_run = 1U;
      for (auto scan = x; scan > left && high[y * width + scan - 1U] != 0U; --scan) {
        ++horizontal_run;
      }
      for (auto scan = x + 1U; scan < right && high[y * width + scan] != 0U; ++scan) {
        ++horizontal_run;
      }
      std::size_t vertical_run = 1U;
      for (auto scan = y; scan > top && high[(scan - 1U) * width + x] != 0U; --scan) {
        ++vertical_run;
      }
      for (auto scan = y + 1U; scan < bottom && high[scan * width + x] != 0U; ++scan) {
        ++vertical_run;
      }
      if ((horizontal_run >= 1U && horizontal_run <= 3U) ||
          (vertical_run >= 1U && vertical_run <= 3U)) {
        ++small_count;
      }
    }
  }
  for (auto y = top; y < bottom; ++y) {
    for (auto x = left; x < right; ++x) {
      const auto index = y * width + x;
      if (x + 1U < right) {
        ++eligible_anchors;
        const auto available = right - x - 1U;
        const auto maximum_distance = available < 12U ? available : 12U;
        bool matched = false;
        for (std::size_t distance = 1U; distance <= maximum_distance; ++distance) {
          const auto partner = y * width + x + distance;
          if (high_component_device(gradient_x[index]) &&
              high_component_device(gradient_x[partner]) &&
              static_cast<std::int64_t>(gradient_x[index]) * gradient_x[partner] < 0) {
            matched = true;
            break;
          }
        }
        qualifying_anchors += matched ? 1U : 0U;
      }
      if (y + 1U < bottom) {
        ++eligible_anchors;
        const auto available = bottom - y - 1U;
        const auto maximum_distance = available < 12U ? available : 12U;
        bool matched = false;
        for (std::size_t distance = 1U; distance <= maximum_distance; ++distance) {
          const auto partner = (y + distance) * width + x;
          if (high_component_device(gradient_y[index]) &&
              high_component_device(gradient_y[partner]) &&
              static_cast<std::int64_t>(gradient_y[index]) * gradient_y[partner] < 0) {
            matched = true;
            break;
          }
        }
        qualifying_anchors += matched ? 1U : 0U;
      }
    }
  }
  const auto percentile_10 = static_cast<std::size_t>(floor(0.10 * (tile_pixel_count - 1U)));
  const auto percentile_90 = static_cast<std::size_t>(floor(0.90 * (tile_pixel_count - 1U)));
  DeviceTileFeature output{};
  output.gradient_density =
      clamp01_device(static_cast<double>(high_count) / static_cast<double>(tile_pixel_count));
  output.local_contrast = clamp01_device(
      static_cast<double>(sorted_codes[percentile_90] - sorted_codes[percentile_10]) / 255.0);
  output.edge_pair_density = eligible_anchors == 0U
                                 ? 0.0
                                 : clamp01_device(static_cast<double>(qualifying_anchors) /
                                                  static_cast<double>(eligible_anchors));
  output.small_structure_density =
      high_count == 0U
          ? 0.0
          : clamp01_device(static_cast<double>(small_count) / static_cast<double>(high_count));
  output.raw_score =
      clamp01_device(configuration.gradient_weight * output.gradient_density +
                     configuration.contrast_weight * output.local_contrast +
                     configuration.edge_pair_weight * output.edge_pair_density +
                     configuration.small_structure_weight * output.small_structure_density);
  output.temporal_stability = 1.0;
  features[tile_index] = output;
}

__device__ std::uint8_t quantize_level_device(double score) {
  return score >= 0.85 ? 4U : score >= 0.70 ? 3U : score >= 0.55 ? 2U : 1U;
}

__global__ void temporal_kernel(const std::uint8_t *canonical, const std::uint8_t *previous_luma,
                                DeviceTileFeature *features, double *filtered_scores,
                                std::uint8_t *active_tiles, std::uint8_t *automatic_levels,
                                const double *previous_scores, const std::uint8_t *previous_active,
                                std::size_t width, std::size_t height, std::size_t tile_width,
                                std::size_t tile_count, bool same_epoch, bool prior_luma_compatible,
                                DeviceConfiguration configuration) {
  const auto tile_index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (tile_index >= tile_count) {
    return;
  }
  auto feature = features[tile_index];
  if (prior_luma_compatible) {
    const auto tile_x = tile_index % tile_width;
    const auto tile_y = tile_index / tile_width;
    const auto left = tile_x * kSaliencyTileSize;
    const auto top = tile_y * kSaliencyTileSize;
    const auto right = left + kSaliencyTileSize < width ? left + kSaliencyTileSize : width;
    const auto bottom = top + kSaliencyTileSize < height ? top + kSaliencyTileSize : height;
    std::uint64_t absolute_change = 0U;
    for (auto y = top; y < bottom; ++y) {
      for (auto x = left; x < right; ++x) {
        const auto index = y * width + x;
        const auto difference = static_cast<int>(canonical[index]) - previous_luma[index];
        absolute_change += static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
      }
    }
    const auto count = (right - left) * (bottom - top);
    const auto mean_change = static_cast<double>(absolute_change) / static_cast<double>(count);
    feature.temporal_stability = 1.0 - (mean_change / 32.0 < 1.0 ? mean_change / 32.0 : 1.0);
  }
  feature.current_score =
      clamp01_device(feature.raw_score * (0.75 + 0.25 * feature.temporal_stability));
  feature.filtered_score =
      same_epoch
          ? clamp01_device(configuration.previous_score_coefficient * previous_scores[tile_index] +
                           (1.0 - configuration.previous_score_coefficient) * feature.current_score)
          : feature.current_score;
  const bool was_active = same_epoch && previous_active[tile_index] != 0U;
  const bool active = was_active ? feature.filtered_score >= configuration.exit_threshold
                                 : feature.filtered_score >= configuration.entry_threshold;
  feature.active = active ? 1U : 0U;
  feature.automatic_level = active ? quantize_level_device(feature.filtered_score) : 0U;
  features[tile_index] = feature;
  filtered_scores[tile_index] = feature.filtered_score;
  active_tiles[tile_index] = feature.active;
  automatic_levels[tile_index] = feature.automatic_level;
}

__device__ std::size_t saturating_add_device(std::size_t left, std::size_t right) {
  return right > static_cast<std::size_t>(-1) - left ? static_cast<std::size_t>(-1) : left + right;
}

__device__ bool rectangle_intersects_device(const SaliencyRectangle &rectangle, std::size_t left,
                                            std::size_t top, std::size_t right,
                                            std::size_t bottom) {
  if (rectangle.width == 0U || rectangle.height == 0U) {
    return false;
  }
  return rectangle.x < right && saturating_add_device(rectangle.x, rectangle.width) > left &&
         rectangle.y < bottom && saturating_add_device(rectangle.y, rectangle.height) > top;
}

__device__ bool any_intersection_device(const SaliencyRectangle *rectangles, std::size_t count,
                                        std::size_t left, std::size_t top, std::size_t right,
                                        std::size_t bottom) {
  for (std::size_t index = 0U; index < count; ++index) {
    if (rectangle_intersects_device(rectangles[index], left, top, right, bottom)) {
      return true;
    }
  }
  return false;
}

__global__ void morphology_kernel(const std::uint8_t *automatic_levels, std::uint8_t *final_levels,
                                  std::size_t tile_width, std::size_t tile_height,
                                  std::size_t visible_width, std::size_t visible_height,
                                  std::size_t radius, const SaliencyRectangle *exclusions,
                                  std::size_t exclusion_count, const SaliencyRectangle *pins,
                                  std::size_t pin_count, const SaliencyRectangle *cursor_halos,
                                  std::size_t cursor_count, std::uint8_t pin_minimum,
                                  std::uint8_t cursor_minimum) {
  const auto tile_index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const auto tile_count = tile_width * tile_height;
  if (tile_index >= tile_count) {
    return;
  }
  const auto tile_x = tile_index % tile_width;
  const auto tile_y = tile_index / tile_width;
  const auto minimum_y = tile_y > radius ? tile_y - radius : 0U;
  const auto maximum_y = tile_y + radius < tile_height ? tile_y + radius : tile_height - 1U;
  const auto minimum_x = tile_x > radius ? tile_x - radius : 0U;
  const auto maximum_x = tile_x + radius < tile_width ? tile_x + radius : tile_width - 1U;
  std::uint8_t level = 0U;
  for (auto source_y = minimum_y; source_y <= maximum_y; ++source_y) {
    for (auto source_x = minimum_x; source_x <= maximum_x; ++source_x) {
      const auto candidate = automatic_levels[source_y * tile_width + source_x];
      level = candidate > level ? candidate : level;
    }
  }
  const auto left = tile_x * kSaliencyTileSize;
  const auto top = tile_y * kSaliencyTileSize;
  const auto right =
      left + kSaliencyTileSize < visible_width ? left + kSaliencyTileSize : visible_width;
  const auto bottom =
      top + kSaliencyTileSize < visible_height ? top + kSaliencyTileSize : visible_height;
  if (any_intersection_device(cursor_halos, cursor_count, left, top, right, bottom)) {
    level = cursor_minimum > level ? cursor_minimum : level;
  }
  if (any_intersection_device(pins, pin_count, left, top, right, bottom)) {
    level = pin_minimum > level ? pin_minimum : level;
  }
  if (any_intersection_device(exclusions, exclusion_count, left, top, right, bottom)) {
    level = 0U;
  }
  final_levels[tile_index] = level;
}

__global__ void macroblock_reduction_kernel(const std::uint8_t *tile_levels,
                                            std::int8_t *macroblock_levels, std::size_t tile_width,
                                            std::size_t tile_height, std::size_t macroblock_width,
                                            std::size_t macroblock_count) {
  const auto macroblock_index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (macroblock_index >= macroblock_count) {
    return;
  }
  const auto macroblock_x = macroblock_index % macroblock_width;
  const auto macroblock_y = macroblock_index / macroblock_width;
  std::uint8_t level = 0U;
  for (auto tile_y = macroblock_y * 2U; tile_y < tile_height && tile_y < macroblock_y * 2U + 2U;
       ++tile_y) {
    for (auto tile_x = macroblock_x * 2U; tile_x < tile_width && tile_x < macroblock_x * 2U + 2U;
         ++tile_x) {
      const auto candidate = tile_levels[tile_y * tile_width + tile_x];
      level = candidate > level ? candidate : level;
    }
  }
  macroblock_levels[macroblock_index] = static_cast<std::int8_t>(level);
}

template <typename Value>
bool allocate_device(Value *&pointer, std::size_t count, std::string &reason, const char *name) {
  const auto status = cudaMalloc(reinterpret_cast<void **>(&pointer), count * sizeof(Value));
  if (status != cudaSuccess) {
    reason = cuda_failure(name, status);
    pointer = nullptr;
    return false;
  }
  return true;
}

void free_device(void *pointer) {
  if (pointer != nullptr) {
    static_cast<void>(cudaFree(pointer));
  }
}

std::uint64_t elapsed_event_ns(cudaEvent_t start, cudaEvent_t end) {
  float milliseconds = 0.0F;
  if (cudaEventElapsedTime(&milliseconds, start, end) != cudaSuccess || milliseconds < 0.0F) {
    return 0U;
  }
  return static_cast<std::uint64_t>(std::llround(static_cast<double>(milliseconds) * 1'000'000.0));
}

} // namespace

std::uint64_t CudaPreprocessTimingsNs::total() const { return total_pipeline; }

struct CudaPreprocessor::Implementation {
  struct SourceSlot {
    std::uint8_t *packed = nullptr;
    std::size_t allocation_bytes = 0U;
    cudaEvent_t read_complete = nullptr;
    bool in_use = false;
  };

  struct SurfaceSlot {
    std::uint8_t *nv12 = nullptr;
    std::uint8_t *canonical = nullptr;
    int *gradient_x = nullptr;
    int *gradient_y = nullptr;
    std::uint8_t *high = nullptr;
    DeviceTileFeature *features = nullptr;
    double *filtered_scores = nullptr;
    std::uint8_t *active = nullptr;
    std::uint8_t *automatic_levels = nullptr;
    std::uint8_t *final_levels = nullptr;
    std::int8_t *device_map = nullptr;
    std::int8_t *host_map = nullptr;
    SaliencyRectangle *exclusions = nullptr;
    SaliencyRectangle *pins = nullptr;
    SaliencyRectangle *cursor_halos = nullptr;
    std::array<cudaEvent_t, event_index(TimingEvent::count)> events{};
    std::size_t nv12_allocation_bytes = 0U;
    std::size_t map_capacity = 0U;
    bool in_use = false;
    bool waited = false;
    bool capture_debug = false;
    PreprocessSlotToken token;
    std::size_t visible_width = 0U;
    std::size_t visible_height = 0U;
    std::size_t coded_width = 0U;
    std::size_t coded_height = 0U;
    std::size_t tile_width = 0U;
    std::size_t tile_height = 0U;
    std::size_t macroblock_width = 0U;
    std::size_t macroblock_height = 0U;
  };

  Implementation(std::shared_ptr<CudaPrimaryContext> selected_context, std::size_t requested_width,
                 std::size_t requested_height, std::size_t source_capacity,
                 std::size_t surface_capacity, SaliencyConfiguration selected_configuration)
      : ownership(source_capacity, surface_capacity), maximum_visible_width(requested_width),
        maximum_visible_height(requested_height), configuration(selected_configuration),
        context(std::move(selected_context)), sources(source_capacity), surfaces(surface_capacity) {
    initialize();
  }

  ~Implementation() { cleanup(); }

  void initialize() {
    if (maximum_visible_width == 0U || maximum_visible_height == 0U ||
        maximum_visible_width > 16'384U || maximum_visible_height > 16'384U ||
        !valid_saliency_configuration(configuration)) {
      reason = "cuda_preprocess_configuration_invalid";
      return;
    }
    if (!context || !context->available()) {
      reason = context ? context->reason() : "cuda_primary_context_missing";
      return;
    }
    ScopedCudaContext guard(*context);
    if (!guard.active()) {
      reason = guard.reason();
      return;
    }
    maximum_coded_width = (maximum_visible_width + 1U) & ~std::size_t{1U};
    maximum_coded_height = (maximum_visible_height + 1U) & ~std::size_t{1U};
    nv12_pitch = maximum_coded_width;
    maximum_tile_width = (maximum_visible_width + kSaliencyTileSize - 1U) / kSaliencyTileSize;
    maximum_tile_height = (maximum_visible_height + kSaliencyTileSize - 1U) / kSaliencyTileSize;
    maximum_macroblock_width =
        (maximum_coded_width + kH264MacroblockSize - 1U) / kH264MacroblockSize;
    maximum_macroblock_height =
        (maximum_coded_height + kH264MacroblockSize - 1U) / kH264MacroblockSize;
    if (!checked_product(maximum_visible_width, maximum_visible_height, maximum_pixels) ||
        !checked_product(maximum_tile_width, maximum_tile_height, maximum_tiles) ||
        !checked_product(maximum_macroblock_width, maximum_macroblock_height,
                         maximum_macroblocks)) {
      reason = "cuda_preprocess_capacity_overflow";
      return;
    }
    std::size_t luma_allocation = 0U;
    if (!checked_product(nv12_pitch, maximum_coded_height, luma_allocation) ||
        luma_allocation > std::numeric_limits<std::size_t>::max() - luma_allocation / 2U) {
      reason = "cuda_preprocess_nv12_capacity_overflow";
      return;
    }
    maximum_nv12_bytes = luma_allocation + luma_allocation / 2U;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
      reason = "cuda_preprocess_stream_create_failed";
      return;
    }
    if (!allocate_device(previous_luma, maximum_pixels, reason,
                         "cuda_preprocess_previous_luma_alloc") ||
        !allocate_device(previous_scores, maximum_tiles, reason,
                         "cuda_preprocess_previous_scores_alloc") ||
        !allocate_device(previous_active, maximum_tiles, reason,
                         "cuda_preprocess_previous_active_alloc")) {
      return;
    }
    const auto source_bytes = maximum_visible_width * maximum_visible_height * 4U;
    for (auto &source : sources) {
      source.allocation_bytes = source_bytes;
      if (!allocate_device(source.packed, source_bytes, reason,
                           "cuda_preprocess_packed_source_alloc") ||
          cudaEventCreateWithFlags(&source.read_complete, cudaEventDisableTiming) != cudaSuccess) {
        reason = "cuda_preprocess_source_event_or_allocation_failed";
        return;
      }
    }
    for (auto &surface : surfaces) {
      surface.nv12_allocation_bytes = maximum_nv12_bytes;
      surface.map_capacity = maximum_macroblocks;
      if (!allocate_device(surface.nv12, maximum_nv12_bytes, reason,
                           "cuda_preprocess_nv12_alloc") ||
          !allocate_device(surface.canonical, maximum_pixels, reason,
                           "cuda_preprocess_canonical_alloc") ||
          !allocate_device(surface.gradient_x, maximum_pixels, reason,
                           "cuda_preprocess_gradient_x_alloc") ||
          !allocate_device(surface.gradient_y, maximum_pixels, reason,
                           "cuda_preprocess_gradient_y_alloc") ||
          !allocate_device(surface.high, maximum_pixels, reason, "cuda_preprocess_high_alloc") ||
          !allocate_device(surface.features, maximum_tiles, reason,
                           "cuda_preprocess_features_alloc") ||
          !allocate_device(surface.filtered_scores, maximum_tiles, reason,
                           "cuda_preprocess_filtered_scores_alloc") ||
          !allocate_device(surface.active, maximum_tiles, reason, "cuda_preprocess_active_alloc") ||
          !allocate_device(surface.automatic_levels, maximum_tiles, reason,
                           "cuda_preprocess_automatic_levels_alloc") ||
          !allocate_device(surface.final_levels, maximum_tiles, reason,
                           "cuda_preprocess_final_levels_alloc") ||
          !allocate_device(surface.device_map, maximum_macroblocks, reason,
                           "cuda_preprocess_device_map_alloc") ||
          !allocate_device(surface.exclusions, kMaximumOverrideRectangles, reason,
                           "cuda_preprocess_exclusions_alloc") ||
          !allocate_device(surface.pins, kMaximumOverrideRectangles, reason,
                           "cuda_preprocess_pins_alloc") ||
          !allocate_device(surface.cursor_halos, kMaximumOverrideRectangles, reason,
                           "cuda_preprocess_cursor_alloc")) {
        return;
      }
      if (cudaMallocHost(reinterpret_cast<void **>(&surface.host_map), maximum_macroblocks) !=
          cudaSuccess) {
        reason = "cuda_preprocess_host_map_alloc_failed";
        return;
      }
      for (auto &event : surface.events) {
        if (cudaEventCreate(&event) != cudaSuccess) {
          reason = "cuda_preprocess_timing_event_create_failed";
          return;
        }
      }
    }
    ready = true;
    reason = "cuda_preprocessor_ready";
  }

  void cleanup() {
    ownership.close_admission();
    if (!context || !context->available()) {
      return;
    }
    {
      ScopedCudaContext guard(*context);
      if (!guard.active()) {
        return;
      }
      if (stream != nullptr) {
        static_cast<void>(cudaStreamSynchronize(stream));
      }
      for (auto &surface : surfaces) {
        if (surface.in_use) {
          static_cast<void>(ownership.abort(surface.token));
        }
        for (auto &event : surface.events) {
          if (event != nullptr) {
            static_cast<void>(cudaEventDestroy(event));
            event = nullptr;
          }
        }
        if (surface.host_map != nullptr) {
          static_cast<void>(cudaFreeHost(surface.host_map));
          surface.host_map = nullptr;
        }
        free_device(surface.nv12);
        free_device(surface.canonical);
        free_device(surface.gradient_x);
        free_device(surface.gradient_y);
        free_device(surface.high);
        free_device(surface.features);
        free_device(surface.filtered_scores);
        free_device(surface.active);
        free_device(surface.automatic_levels);
        free_device(surface.final_levels);
        free_device(surface.device_map);
        free_device(surface.exclusions);
        free_device(surface.pins);
        free_device(surface.cursor_halos);
        surface = {};
      }
      for (auto &source : sources) {
        if (source.read_complete != nullptr) {
          static_cast<void>(cudaEventDestroy(source.read_complete));
        }
        free_device(source.packed);
        source = {};
      }
      free_device(previous_luma);
      free_device(previous_scores);
      free_device(previous_active);
      previous_luma = nullptr;
      previous_scores = nullptr;
      previous_active = nullptr;
      if (stream != nullptr) {
        static_cast<void>(cudaStreamDestroy(stream));
        stream = nullptr;
      }
    }
    ready = false;
  }

  bool record_event(SurfaceSlot &surface, TimingEvent event) {
    const auto status = cudaEventRecord(surface.events[event_index(event)], stream);
    if (status != cudaSuccess) {
      reason = cuda_failure("cuda_preprocess_event_record", status);
      return false;
    }
    return true;
  }

  bool launch_ok(const char *operation) {
    const auto status = cudaPeekAtLastError();
    if (status != cudaSuccess) {
      reason = cuda_failure(operation, status);
      return false;
    }
    return true;
  }

  void abort_after_enqueue_failure(SourceSlot &source, SurfaceSlot &surface,
                                   const PreprocessSlotToken &token) {
    static_cast<void>(cudaStreamSynchronize(stream));
    static_cast<void>(ownership.abort(token));
    source.in_use = false;
    surface.in_use = false;
    surface.waited = false;
    fatal = true;
    ready = false;
  }

  std::string reason = "cuda_preprocessor_uninitialized";
  PreprocessOwnershipRing ownership;
  std::size_t maximum_visible_width = 0U;
  std::size_t maximum_visible_height = 0U;
  std::size_t maximum_coded_width = 0U;
  std::size_t maximum_coded_height = 0U;
  std::size_t nv12_pitch = 0U;
  std::size_t maximum_tile_width = 0U;
  std::size_t maximum_tile_height = 0U;
  std::size_t maximum_macroblock_width = 0U;
  std::size_t maximum_macroblock_height = 0U;
  std::size_t maximum_pixels = 0U;
  std::size_t maximum_tiles = 0U;
  std::size_t maximum_macroblocks = 0U;
  std::size_t maximum_nv12_bytes = 0U;
  SaliencyConfiguration configuration;
  std::shared_ptr<CudaPrimaryContext> context;
  cudaStream_t stream = nullptr;
  std::vector<SourceSlot> sources;
  std::vector<SurfaceSlot> surfaces;
  std::uint8_t *previous_luma = nullptr;
  double *previous_scores = nullptr;
  std::uint8_t *previous_active = nullptr;
  bool ready = false;
  bool fatal = false;
  bool has_prior = false;
  std::uint64_t previous_frame_id = 0U;
  std::uint64_t previous_geometry_epoch = 0U;
  std::uint64_t previous_timestamp_ns = 0U;
  std::size_t previous_width = 0U;
  std::size_t previous_height = 0U;
  ColorRange previous_range = ColorRange::limited;
  mutable std::mutex state_mutex;
};

CudaPreprocessor::CudaPreprocessor(int device_ordinal, std::size_t maximum_visible_width,
                                   std::size_t maximum_visible_height, std::size_t source_capacity,
                                   std::size_t surface_capacity,
                                   SaliencyConfiguration configuration)
    : implementation_(std::make_unique<Implementation>(
          std::make_shared<CudaPrimaryContext>(device_ordinal), maximum_visible_width,
          maximum_visible_height, source_capacity, surface_capacity, std::move(configuration))) {}

CudaPreprocessor::CudaPreprocessor(std::shared_ptr<CudaPrimaryContext> context,
                                   std::size_t maximum_visible_width,
                                   std::size_t maximum_visible_height, std::size_t source_capacity,
                                   std::size_t surface_capacity,
                                   SaliencyConfiguration configuration)
    : implementation_(std::make_unique<Implementation>(
          std::move(context), maximum_visible_width, maximum_visible_height, source_capacity,
          surface_capacity, std::move(configuration))) {}

CudaPreprocessor::~CudaPreprocessor() = default;
CudaPreprocessor::CudaPreprocessor(CudaPreprocessor &&) noexcept = default;

bool CudaPreprocessor::available() const {
  return implementation_ && implementation_->ready && !implementation_->fatal;
}

const std::string &CudaPreprocessor::reason() const { return implementation_->reason; }

CudaContextIdentity CudaPreprocessor::context_identity() const {
  return implementation_->context ? implementation_->context->identity() : CudaContextIdentity{};
}

CudaPreprocessTicket CudaPreprocessor::enqueue(const CapturedFrame &frame, ColorRange range,
                                               const SaliencyProcessOptions &options,
                                               bool capture_debug_output) {
  std::scoped_lock state_lock(implementation_->state_mutex);
  auto reject = [&](std::string reason) {
    return CudaPreprocessTicket{false, std::move(reason), {}};
  };
  if (!available()) {
    return reject(implementation_->reason);
  }
  const auto width = frame.geometry.visible_width;
  const auto height = frame.geometry.visible_height;
  if (frame.frame_id == 0U || frame.geometry.epoch == 0U || width == 0U || height == 0U ||
      width > implementation_->maximum_visible_width ||
      height > implementation_->maximum_visible_height || width > SIZE_MAX / 4U ||
      frame.pitch < width * 4U || height - 1U > SIZE_MAX / frame.pitch ||
      frame.pixels.size() < (height - 1U) * frame.pitch + width * 4U) {
    return reject("cuda_preprocess_frame_invalid");
  }
  if (options.overrides.exclusions.size() > kMaximumOverrideRectangles ||
      options.overrides.pins.size() > kMaximumOverrideRectangles ||
      options.overrides.cursor_halos.size() > kMaximumOverrideRectangles ||
      options.overrides.pin_minimum_level > kMaximumEmphasisLevel ||
      options.overrides.cursor_minimum_level > kMaximumEmphasisLevel) {
    return reject("cuda_preprocess_overrides_invalid");
  }
  if (implementation_->has_prior && frame.frame_id <= implementation_->previous_frame_id) {
    return reject("cuda_preprocess_frame_id_not_monotonic");
  }
  if (implementation_->has_prior &&
      frame.monotonic_timestamp_ns < implementation_->previous_timestamp_ns) {
    return reject("cuda_preprocess_timestamp_regressed");
  }
  const bool same_epoch = implementation_->has_prior &&
                          frame.geometry.epoch == implementation_->previous_geometry_epoch;
  if (same_epoch &&
      (width != implementation_->previous_width || height != implementation_->previous_height ||
       range != implementation_->previous_range)) {
    return reject("cuda_preprocess_geometry_changed_without_new_epoch");
  }
  auto source_iterator =
      std::find_if(implementation_->sources.begin(), implementation_->sources.end(),
                   [](const auto &slot) { return !slot.in_use; });
  auto surface_iterator =
      std::find_if(implementation_->surfaces.begin(), implementation_->surfaces.end(),
                   [](const auto &slot) { return !slot.in_use; });
  if (source_iterator == implementation_->sources.end() ||
      surface_iterator == implementation_->surfaces.end()) {
    return reject("cuda_preprocess_pool_exhausted");
  }
  auto &source = *source_iterator;
  auto &surface = *surface_iterator;
  const auto reservation = implementation_->ownership.reserve(
      frame.frame_id, frame.geometry.epoch,
      {reinterpret_cast<std::uintptr_t>(source.packed), source.allocation_bytes},
      {reinterpret_cast<std::uintptr_t>(surface.nv12), surface.nv12_allocation_bytes});
  if (!reservation.passed) {
    return reject(reservation.reason);
  }
  source.in_use = true;
  surface.in_use = true;
  surface.waited = false;
  surface.capture_debug = capture_debug_output;
  surface.token = reservation.token;
  surface.visible_width = width;
  surface.visible_height = height;
  surface.coded_width = (width + 1U) & ~std::size_t{1U};
  surface.coded_height = (height + 1U) & ~std::size_t{1U};
  surface.tile_width = (width + kSaliencyTileSize - 1U) / kSaliencyTileSize;
  surface.tile_height = (height + kSaliencyTileSize - 1U) / kSaliencyTileSize;
  surface.macroblock_width = (surface.coded_width + kH264MacroblockSize - 1U) / kH264MacroblockSize;
  surface.macroblock_height =
      (surface.coded_height + kH264MacroblockSize - 1U) / kH264MacroblockSize;
  const auto tile_count = surface.tile_width * surface.tile_height;
  const auto macroblock_count = surface.macroblock_width * surface.macroblock_height;
  const auto pixel_count = width * height;
  ScopedCudaContext context_guard(*implementation_->context);
  if (!context_guard.active()) {
    implementation_->reason = context_guard.reason();
    implementation_->abort_after_enqueue_failure(source, surface, reservation.token);
    return reject(implementation_->reason);
  }
  bool scheduled = implementation_->record_event(surface, TimingEvent::total_start) &&
                   implementation_->record_event(surface, TimingEvent::upload_start);
  if (scheduled) {
    NvtxRange range_marker("glyphrelay.cuda.input_upload");
    const auto status = cudaMemcpy2DAsync(
        source.packed, implementation_->maximum_visible_width * 4U, frame.pixels.data(),
        frame.pitch, width * 4U, height, cudaMemcpyHostToDevice, implementation_->stream);
    if (status != cudaSuccess) {
      implementation_->reason = cuda_failure("cuda_preprocess_input_upload", status);
      scheduled = false;
    }
  }
  scheduled = scheduled && implementation_->record_event(surface, TimingEvent::upload_end) &&
              implementation_->record_event(surface, TimingEvent::conversion_start);
  if (scheduled) {
    NvtxRange range_marker("glyphrelay.cuda.color_conversion");
    constexpr dim3 threads(16U, 16U);
    const dim3 luma_blocks(static_cast<unsigned int>((surface.coded_width + 15U) / 16U),
                           static_cast<unsigned int>((surface.coded_height + 15U) / 16U));
    convert_luma_kernel<<<luma_blocks, threads, 0U, implementation_->stream>>>(
        source.packed, implementation_->maximum_visible_width * 4U, surface.nv12,
        implementation_->nv12_pitch, width, height, surface.coded_width, surface.coded_height,
        frame.pixel_order == PackedPixelOrder::rgba, range == ColorRange::limited);
    const dim3 chroma_blocks(static_cast<unsigned int>((surface.coded_width / 2U + 15U) / 16U),
                             static_cast<unsigned int>((surface.coded_height / 2U + 15U) / 16U));
    convert_chroma_kernel<<<chroma_blocks, threads, 0U, implementation_->stream>>>(
        source.packed, implementation_->maximum_visible_width * 4U,
        surface.nv12 + implementation_->nv12_pitch * surface.coded_height,
        implementation_->nv12_pitch, width, height, surface.coded_width, surface.coded_height,
        frame.pixel_order == PackedPixelOrder::rgba, range == ColorRange::limited);
    scheduled = implementation_->launch_ok("cuda_preprocess_color_kernel_launch");
  }
  scheduled = scheduled && implementation_->record_event(surface, TimingEvent::conversion_end);
  if (scheduled) {
    const auto source_event_status = cudaEventRecord(source.read_complete, implementation_->stream);
    if (source_event_status != cudaSuccess) {
      implementation_->reason =
          cuda_failure("cuda_preprocess_source_event_record", source_event_status);
      scheduled = false;
    }
  }
  scheduled = scheduled && implementation_->record_event(surface, TimingEvent::features_start);
  if (scheduled) {
    NvtxRange range_marker("glyphrelay.cuda.feature_extraction");
    constexpr dim3 threads(16U, 16U);
    const dim3 pixel_blocks(static_cast<unsigned int>((width + 15U) / 16U),
                            static_cast<unsigned int>((height + 15U) / 16U));
    canonical_kernel<<<pixel_blocks, threads, 0U, implementation_->stream>>>(
        surface.nv12, implementation_->nv12_pitch, surface.canonical, width, height,
        range == ColorRange::limited);
    gradient_kernel<<<pixel_blocks, threads, 0U, implementation_->stream>>>(
        surface.canonical, surface.gradient_x, surface.gradient_y, surface.high, width, height);
    const DeviceConfiguration device_configuration{
        implementation_->configuration.gradient_weight,
        implementation_->configuration.contrast_weight,
        implementation_->configuration.edge_pair_weight,
        implementation_->configuration.small_structure_weight,
        implementation_->configuration.entry_threshold,
        implementation_->configuration.exit_threshold,
        implementation_->configuration.previous_score_coefficient,
        implementation_->configuration.dilation_radius_tiles,
    };
    tile_feature_kernel<<<static_cast<unsigned int>((tile_count + 255U) / 256U), 256U, 0U,
                          implementation_->stream>>>(
        surface.canonical, surface.gradient_x, surface.gradient_y, surface.high, surface.features,
        width, height, surface.tile_width, tile_count, device_configuration);
    scheduled = implementation_->launch_ok("cuda_preprocess_feature_kernel_launch");
  }
  scheduled = scheduled && implementation_->record_event(surface, TimingEvent::features_end) &&
              implementation_->record_event(surface, TimingEvent::temporal_start);
  if (scheduled) {
    NvtxRange range_marker("glyphrelay.cuda.temporal_hysteresis");
    const bool prior_luma_compatible =
        same_epoch && frame.monotonic_timestamp_ns - implementation_->previous_timestamp_ns <=
                          kTemporalResetGapNs;
    const DeviceConfiguration device_configuration{
        implementation_->configuration.gradient_weight,
        implementation_->configuration.contrast_weight,
        implementation_->configuration.edge_pair_weight,
        implementation_->configuration.small_structure_weight,
        implementation_->configuration.entry_threshold,
        implementation_->configuration.exit_threshold,
        implementation_->configuration.previous_score_coefficient,
        implementation_->configuration.dilation_radius_tiles,
    };
    temporal_kernel<<<static_cast<unsigned int>((tile_count + 255U) / 256U), 256U, 0U,
                      implementation_->stream>>>(
        surface.canonical, implementation_->previous_luma, surface.features,
        surface.filtered_scores, surface.active, surface.automatic_levels,
        implementation_->previous_scores, implementation_->previous_active, width, height,
        surface.tile_width, tile_count, same_epoch, prior_luma_compatible, device_configuration);
    scheduled = implementation_->launch_ok("cuda_preprocess_temporal_kernel_launch");
    if (scheduled) {
      scheduled =
          cudaMemcpyAsync(implementation_->previous_luma, surface.canonical, pixel_count,
                          cudaMemcpyDeviceToDevice, implementation_->stream) == cudaSuccess &&
          cudaMemcpyAsync(implementation_->previous_scores, surface.filtered_scores,
                          tile_count * sizeof(double), cudaMemcpyDeviceToDevice,
                          implementation_->stream) == cudaSuccess &&
          cudaMemcpyAsync(implementation_->previous_active, surface.active, tile_count,
                          cudaMemcpyDeviceToDevice, implementation_->stream) == cudaSuccess;
      if (!scheduled) {
        implementation_->reason = "cuda_preprocess_temporal_state_copy_failed";
      }
    }
  }
  scheduled = scheduled && implementation_->record_event(surface, TimingEvent::temporal_end) &&
              implementation_->record_event(surface, TimingEvent::morphology_start);
  auto copy_rectangles = [&](const std::vector<SaliencyRectangle> &rectangles,
                             SaliencyRectangle *destination) {
    if (rectangles.empty()) {
      return true;
    }
    return cudaMemcpyAsync(destination, rectangles.data(),
                           rectangles.size() * sizeof(SaliencyRectangle), cudaMemcpyHostToDevice,
                           implementation_->stream) == cudaSuccess;
  };
  if (scheduled) {
    NvtxRange range_marker("glyphrelay.cuda.morphology_and_overrides");
    scheduled = copy_rectangles(options.overrides.exclusions, surface.exclusions) &&
                copy_rectangles(options.overrides.pins, surface.pins) &&
                copy_rectangles(options.overrides.cursor_halos, surface.cursor_halos);
    if (scheduled) {
      morphology_kernel<<<static_cast<unsigned int>((tile_count + 255U) / 256U), 256U, 0U,
                          implementation_->stream>>>(
          surface.automatic_levels, surface.final_levels, surface.tile_width, surface.tile_height,
          width, height, implementation_->configuration.dilation_radius_tiles, surface.exclusions,
          options.overrides.exclusions.size(), surface.pins, options.overrides.pins.size(),
          surface.cursor_halos, options.overrides.cursor_halos.size(),
          options.overrides.pin_minimum_level, options.overrides.cursor_minimum_level);
      scheduled = implementation_->launch_ok("cuda_preprocess_morphology_kernel_launch");
    } else {
      implementation_->reason = "cuda_preprocess_override_copy_failed";
    }
  }
  scheduled = scheduled && implementation_->record_event(surface, TimingEvent::morphology_end) &&
              implementation_->record_event(surface, TimingEvent::reduction_start);
  if (scheduled) {
    NvtxRange range_marker("glyphrelay.cuda.macroblock_reduction");
    macroblock_reduction_kernel<<<static_cast<unsigned int>((macroblock_count + 255U) / 256U), 256U,
                                  0U, implementation_->stream>>>(
        surface.final_levels, surface.device_map, surface.tile_width, surface.tile_height,
        surface.macroblock_width, macroblock_count);
    scheduled = implementation_->launch_ok("cuda_preprocess_reduction_kernel_launch");
  }
  scheduled = scheduled && implementation_->record_event(surface, TimingEvent::reduction_end) &&
              implementation_->record_event(surface, TimingEvent::map_copy_start);
  if (scheduled) {
    NvtxRange range_marker("glyphrelay.cuda.host_map_copy");
    const auto status = cudaMemcpyAsync(surface.host_map, surface.device_map, macroblock_count,
                                        cudaMemcpyDeviceToHost, implementation_->stream);
    if (status != cudaSuccess) {
      implementation_->reason = cuda_failure("cuda_preprocess_host_map_copy", status);
      scheduled = false;
    }
  }
  scheduled = scheduled && implementation_->record_event(surface, TimingEvent::map_copy_end) &&
              implementation_->record_event(surface, TimingEvent::total_end);
  if (!scheduled) {
    implementation_->abort_after_enqueue_failure(source, surface, reservation.token);
    return reject(implementation_->reason);
  }
  implementation_->has_prior = true;
  implementation_->previous_frame_id = frame.frame_id;
  implementation_->previous_geometry_epoch = frame.geometry.epoch;
  implementation_->previous_timestamp_ns = frame.monotonic_timestamp_ns;
  implementation_->previous_width = width;
  implementation_->previous_height = height;
  implementation_->previous_range = range;
  implementation_->reason = "cuda_preprocess_enqueued";
  return {true, implementation_->reason, reservation.token};
}

CudaPreprocessCompletion CudaPreprocessor::wait(const CudaPreprocessTicket &ticket) {
  std::scoped_lock state_lock(implementation_->state_mutex);
  CudaPreprocessCompletion result;
  result.reason = "cuda_preprocess_ticket_invalid";
  if (!ticket.passed || ticket.token.surface_slot >= implementation_->surfaces.size() ||
      ticket.token.source_slot >= implementation_->sources.size()) {
    return result;
  }
  auto &source = implementation_->sources[ticket.token.source_slot];
  auto &surface = implementation_->surfaces[ticket.token.surface_slot];
  if (!source.in_use || !surface.in_use || surface.waited ||
      surface.token.frame_id != ticket.token.frame_id ||
      surface.token.geometry_epoch != ticket.token.geometry_epoch) {
    result.reason = "cuda_preprocess_ticket_stale_or_completed";
    return result;
  }
  ScopedCudaContext context_guard(*implementation_->context);
  if (!context_guard.active()) {
    result.reason = context_guard.reason();
    implementation_->abort_after_enqueue_failure(source, surface, ticket.token);
    return result;
  }
  const auto sync_status =
      cudaEventSynchronize(surface.events[event_index(TimingEvent::total_end)]);
  if (sync_status != cudaSuccess) {
    result.reason = cuda_failure("cuda_preprocess_completion_wait", sync_status);
    implementation_->reason = result.reason;
    implementation_->abort_after_enqueue_failure(source, surface, ticket.token);
    return result;
  }
  const auto source_event_status = cudaEventQuery(source.read_complete);
  if (source_event_status != cudaSuccess) {
    result.reason = cuda_failure("cuda_preprocess_source_event_incomplete", source_event_status);
    implementation_->reason = result.reason;
    implementation_->abort_after_enqueue_failure(source, surface, ticket.token);
    return result;
  }
  const auto upload = implementation_->ownership.source_upload_complete(ticket.token);
  const auto source_release = implementation_->ownership.source_read_complete(ticket.token);
  const auto map_pending = implementation_->ownership.map_copy_pending(ticket.token);
  const auto submit_ready = implementation_->ownership.ready_to_submit(ticket.token);
  if (!upload.passed || !source_release.passed || !map_pending.passed || !submit_ready.passed) {
    result.reason = "cuda_preprocess_ownership_completion_invalid";
    implementation_->abort_after_enqueue_failure(source, surface, ticket.token);
    return result;
  }
  source.in_use = false;
  surface.waited = true;
  result.visible_width = surface.visible_width;
  result.visible_height = surface.visible_height;
  result.tile_width = surface.tile_width;
  result.tile_height = surface.tile_height;
  const auto macroblock_count = surface.macroblock_width * surface.macroblock_height;
  result.surface = {
      .frame_id = ticket.token.frame_id,
      .geometry_epoch = ticket.token.geometry_epoch,
      .context = implementation_->context->identity(),
      .memory_space = MemorySpace::cuda_device,
      .device_pointer = reinterpret_cast<std::uintptr_t>(surface.nv12),
      .coded_width = surface.coded_width,
      .coded_height = surface.coded_height,
      .pitch = implementation_->nv12_pitch,
      .allocation_bytes = surface.nv12_allocation_bytes,
      .contiguous = true,
      .cuda_ready = true,
  };
  result.emphasis_map = {
      .frame_id = ticket.token.frame_id,
      .geometry_epoch = ticket.token.geometry_epoch,
      .context = implementation_->context->identity(),
      .memory_space = MemorySpace::host_pinned,
      .host_pointer = reinterpret_cast<std::uintptr_t>(surface.host_map),
      .macroblock_width = surface.macroblock_width,
      .macroblock_height = surface.macroblock_height,
      .byte_size = macroblock_count,
      .values = std::span<const std::int8_t>(surface.host_map, macroblock_count),
      .device_to_host_ready = true,
  };
  const auto &events = surface.events;
  result.timings = {
      .input_upload = elapsed_event_ns(events[event_index(TimingEvent::upload_start)],
                                       events[event_index(TimingEvent::upload_end)]),
      .color_conversion = elapsed_event_ns(events[event_index(TimingEvent::conversion_start)],
                                           events[event_index(TimingEvent::conversion_end)]),
      .feature_extraction = elapsed_event_ns(events[event_index(TimingEvent::features_start)],
                                             events[event_index(TimingEvent::features_end)]),
      .temporal_hysteresis = elapsed_event_ns(events[event_index(TimingEvent::temporal_start)],
                                              events[event_index(TimingEvent::temporal_end)]),
      .morphology_and_overrides =
          elapsed_event_ns(events[event_index(TimingEvent::morphology_start)],
                           events[event_index(TimingEvent::morphology_end)]),
      .macroblock_reduction = elapsed_event_ns(events[event_index(TimingEvent::reduction_start)],
                                               events[event_index(TimingEvent::reduction_end)]),
      .host_map_copy = elapsed_event_ns(events[event_index(TimingEvent::map_copy_start)],
                                        events[event_index(TimingEvent::map_copy_end)]),
      .total_pipeline = elapsed_event_ns(events[event_index(TimingEvent::total_start)],
                                         events[event_index(TimingEvent::total_end)]),
  };
  if (surface.capture_debug) {
    const auto nv12_bytes = implementation_->nv12_pitch * surface.coded_height * 3U / 2U;
    result.debug_nv12.resize(nv12_bytes);
    std::vector<DeviceTileFeature> device_features(surface.tile_width * surface.tile_height);
    std::vector<std::uint8_t> final_levels(device_features.size());
    const auto nv12_status =
        cudaMemcpy(result.debug_nv12.data(), surface.nv12, nv12_bytes, cudaMemcpyDeviceToHost);
    const auto feature_status =
        cudaMemcpy(device_features.data(), surface.features,
                   device_features.size() * sizeof(DeviceTileFeature), cudaMemcpyDeviceToHost);
    const auto level_status = cudaMemcpy(final_levels.data(), surface.final_levels,
                                         final_levels.size(), cudaMemcpyDeviceToHost);
    if (nv12_status != cudaSuccess || feature_status != cudaSuccess ||
        level_status != cudaSuccess) {
      result.reason = "cuda_preprocess_debug_copy_failed";
      implementation_->reason = result.reason;
      static_cast<void>(implementation_->ownership.abort(ticket.token));
      surface.in_use = false;
      implementation_->fatal = true;
      implementation_->ready = false;
      return result;
    }
    result.debug_tiles.reserve(device_features.size());
    for (std::size_t index = 0U; index < device_features.size(); ++index) {
      const auto &feature = device_features[index];
      result.debug_tiles.push_back({
          .gradient_density = feature.gradient_density,
          .local_contrast = feature.local_contrast,
          .edge_pair_density = feature.edge_pair_density,
          .small_structure_density = feature.small_structure_density,
          .raw_score = feature.raw_score,
          .temporal_stability = feature.temporal_stability,
          .current_score = feature.current_score,
          .filtered_score = feature.filtered_score,
          .active = feature.active != 0U,
          .automatic_level = feature.automatic_level,
          .final_level = final_levels[index],
      });
    }
  }
  result.passed = true;
  result.reason = "cuda_preprocess_complete";
  implementation_->reason = result.reason;
  return result;
}

CudaPreprocessOperation CudaPreprocessor::mark_submitted(const CudaPreprocessTicket &ticket) {
  std::scoped_lock state_lock(implementation_->state_mutex);
  if (!ticket.passed || ticket.token.surface_slot >= implementation_->surfaces.size() ||
      !implementation_->surfaces[ticket.token.surface_slot].waited) {
    return {false, "cuda_preprocess_submit_ticket_invalid"};
  }
  const auto operation = implementation_->ownership.submitted(ticket.token);
  return {operation.passed, operation.reason};
}

CudaPreprocessOperation
CudaPreprocessor::mark_encoder_input_released(const CudaPreprocessTicket &ticket) {
  std::scoped_lock state_lock(implementation_->state_mutex);
  if (!ticket.passed || ticket.token.surface_slot >= implementation_->surfaces.size()) {
    return {false, "cuda_preprocess_encoder_release_ticket_invalid"};
  }
  const auto operation = implementation_->ownership.encoder_input_released(ticket.token);
  return {operation.passed, operation.reason};
}

CudaPreprocessOperation CudaPreprocessor::release(const CudaPreprocessTicket &ticket) {
  std::scoped_lock state_lock(implementation_->state_mutex);
  if (!ticket.passed || ticket.token.surface_slot >= implementation_->surfaces.size()) {
    return {false, "cuda_preprocess_release_ticket_invalid"};
  }
  auto &surface = implementation_->surfaces[ticket.token.surface_slot];
  const auto operation = implementation_->ownership.release_surface(ticket.token);
  if (operation.passed) {
    surface.in_use = false;
    surface.waited = false;
    surface.capture_debug = false;
    surface.token = {};
  }
  return {operation.passed, operation.reason};
}

CudaPreprocessOperation CudaPreprocessor::abort(const CudaPreprocessTicket &ticket) {
  std::scoped_lock state_lock(implementation_->state_mutex);
  if (!ticket.passed || ticket.token.surface_slot >= implementation_->surfaces.size() ||
      ticket.token.source_slot >= implementation_->sources.size()) {
    return {false, "cuda_preprocess_abort_ticket_invalid"};
  }
  auto &surface = implementation_->surfaces[ticket.token.surface_slot];
  const auto operation = implementation_->ownership.abort(ticket.token);
  if (operation.passed) {
    surface.in_use = false;
    surface.waited = false;
    surface.capture_debug = false;
    surface.token = {};
  }
  return {operation.passed, operation.reason};
}

void CudaPreprocessor::close_admission() {
  std::scoped_lock state_lock(implementation_->state_mutex);
  implementation_->ownership.close_admission();
}

PreprocessPoolDiagnostics CudaPreprocessor::diagnostics() const {
  std::scoped_lock state_lock(implementation_->state_mutex);
  return implementation_->ownership.diagnostics();
}

bool CudaPreprocessor::all_free() const {
  std::scoped_lock state_lock(implementation_->state_mutex);
  return implementation_->ownership.all_free();
}

} // namespace glyphrelay
