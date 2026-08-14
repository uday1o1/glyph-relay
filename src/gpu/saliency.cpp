#include "glyphrelay/saliency.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace glyphrelay {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kTemporalResetGapNs = 200'000'000U;
constexpr int kScharrScale = 4'080;

std::uint64_t elapsed_ns(Clock::time_point start) {
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
  return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U;
}

bool checked_product(std::size_t left, std::size_t right, std::size_t &result) {
  if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

std::uint64_t round_divide_ties_even(std::uint64_t numerator, std::uint64_t denominator) {
  auto quotient = numerator / denominator;
  const auto remainder = numerator % denominator;
  if (remainder > denominator / 2U || (remainder * 2U == denominator && (quotient & 1U) != 0U)) {
    ++quotient;
  }
  return quotient;
}

double clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

bool high_component(int component) {
  return static_cast<std::int64_t>(std::abs(component)) * 100LL >
         static_cast<std::int64_t>(kScharrScale) * 12LL;
}

bool high_gradient(int horizontal, int vertical) {
  return static_cast<std::int64_t>(std::abs(horizontal) + std::abs(vertical)) * 100LL >
         static_cast<std::int64_t>(kScharrScale) * 12LL;
}

std::size_t saturating_add(std::size_t left, std::size_t right) {
  return right > std::numeric_limits<std::size_t>::max() - left
             ? std::numeric_limits<std::size_t>::max()
             : left + right;
}

bool rectangle_intersects(const SaliencyRectangle &rectangle, std::size_t left, std::size_t top,
                          std::size_t right, std::size_t bottom) {
  if (rectangle.width == 0U || rectangle.height == 0U) {
    return false;
  }
  const auto rectangle_right = saturating_add(rectangle.x, rectangle.width);
  const auto rectangle_bottom = saturating_add(rectangle.y, rectangle.height);
  return rectangle.x < right && rectangle_right > left && rectangle.y < bottom &&
         rectangle_bottom > top;
}

bool any_intersection(const std::vector<SaliencyRectangle> &rectangles, std::size_t left,
                      std::size_t top, std::size_t right, std::size_t bottom) {
  return std::any_of(rectangles.begin(), rectangles.end(), [&](const auto &rectangle) {
    return rectangle_intersects(rectangle, left, top, right, bottom);
  });
}

std::uint8_t quantize_active_level(double score) {
  if (score >= 0.85) {
    return 4U;
  }
  if (score >= 0.70) {
    return 3U;
  }
  if (score >= 0.55) {
    return 2U;
  }
  return 1U;
}

std::array<std::uint8_t, 4U> preview_color(std::uint8_t level) {
  constexpr std::array<std::array<std::uint8_t, 4U>, kMaximumEmphasisLevel + 1U> colors = {
      std::array<std::uint8_t, 4U>{0U, 0U, 0U, 0U},
      std::array<std::uint8_t, 4U>{64U, 128U, 255U, 80U},
      std::array<std::uint8_t, 4U>{40U, 200U, 255U, 96U},
      std::array<std::uint8_t, 4U>{255U, 220U, 32U, 112U},
      std::array<std::uint8_t, 4U>{255U, 128U, 24U, 128U},
      std::array<std::uint8_t, 4U>{255U, 32U, 32U, 144U},
  };
  return colors[std::min<std::size_t>(level, kMaximumEmphasisLevel)];
}

std::uint64_t p95_field(const std::vector<SaliencyStageTimingsNs> &samples,
                        std::uint64_t SaliencyStageTimingsNs::*field) {
  if (samples.empty()) {
    return 0U;
  }
  std::vector<std::uint64_t> values;
  values.reserve(samples.size());
  for (const auto &sample : samples) {
    values.push_back(sample.*field);
  }
  std::sort(values.begin(), values.end());
  const auto rank = (95U * values.size() + 99U) / 100U;
  return values[rank - 1U];
}

} // namespace

struct SaliencyReference::Implementation {
  explicit Implementation(SaliencyConfiguration initial) : configuration(std::move(initial)) {}

  SaliencyConfiguration configuration;
  bool has_prior = false;
  std::uint64_t previous_frame_id = 0U;
  std::uint64_t previous_geometry_epoch = 0U;
  std::uint64_t previous_timestamp_ns = 0U;
  std::size_t previous_width = 0U;
  std::size_t previous_height = 0U;
  ColorRange previous_range = ColorRange::limited;
  std::vector<std::uint8_t> previous_luma;
  std::vector<double> previous_scores;
  std::vector<bool> active_tiles;
};

std::uint64_t SaliencyStageTimingsNs::total() const {
  return luma_normalization + feature_extraction + temporal_hysteresis + dilation_and_overrides +
         macroblock_reduction + preview_generation;
}

std::uint8_t canonical_luma_code(std::uint8_t code, ColorRange range) {
  if (range == ColorRange::full) {
    return code;
  }
  const auto clamped = std::clamp<std::uint16_t>(code, 16U, 235U);
  const auto expanded = round_divide_ties_even((clamped - 16U) * 255U, 219U);
  return static_cast<std::uint8_t>(std::min<std::uint64_t>(expanded, 255U));
}

bool valid_saliency_configuration(const SaliencyConfiguration &configuration) {
  const std::array weights = {
      configuration.gradient_weight,
      configuration.contrast_weight,
      configuration.edge_pair_weight,
      configuration.small_structure_weight,
  };
  if (std::any_of(weights.begin(), weights.end(),
                  [](double weight) { return !std::isfinite(weight) || weight < 0.0; })) {
    return false;
  }
  const auto sum = configuration.gradient_weight + configuration.contrast_weight +
                   configuration.edge_pair_weight + configuration.small_structure_weight;
  return std::abs(sum - 1.0) <= 1e-12 && std::isfinite(configuration.entry_threshold) &&
         configuration.entry_threshold >= 0.0 && configuration.entry_threshold <= 1.0 &&
         std::isfinite(configuration.exit_threshold) && configuration.exit_threshold >= 0.0 &&
         configuration.exit_threshold < configuration.entry_threshold &&
         std::isfinite(configuration.previous_score_coefficient) &&
         configuration.previous_score_coefficient >= 0.0 &&
         configuration.previous_score_coefficient < 1.0 &&
         configuration.dilation_radius_tiles <= 2U;
}

std::vector<std::uint8_t> dilate_automatic_levels(std::span<const std::uint8_t> levels,
                                                  std::size_t tile_width, std::size_t tile_height,
                                                  std::size_t radius) {
  std::size_t expected = 0U;
  if (tile_width == 0U || tile_height == 0U || radius > 2U ||
      !checked_product(tile_width, tile_height, expected) || levels.size() != expected ||
      std::any_of(levels.begin(), levels.end(),
                  [](std::uint8_t level) { return level > kMaximumEmphasisLevel; })) {
    throw std::invalid_argument("saliency dilation shape or level is invalid");
  }
  std::vector<std::uint8_t> output(expected, 0U);
  for (std::size_t tile_y = 0U; tile_y < tile_height; ++tile_y) {
    for (std::size_t tile_x = 0U; tile_x < tile_width; ++tile_x) {
      const auto minimum_y = tile_y > radius ? tile_y - radius : 0U;
      const auto maximum_y = tile_y > tile_height - 1U - std::min(radius, tile_height - 1U)
                                 ? tile_height - 1U
                                 : tile_y + radius;
      const auto minimum_x = tile_x > radius ? tile_x - radius : 0U;
      const auto maximum_x = tile_x > tile_width - 1U - std::min(radius, tile_width - 1U)
                                 ? tile_width - 1U
                                 : tile_x + radius;
      std::uint8_t level = 0U;
      for (std::size_t source_y = minimum_y; source_y <= maximum_y; ++source_y) {
        for (std::size_t source_x = minimum_x; source_x <= maximum_x; ++source_x) {
          level = std::max(level, levels[source_y * tile_width + source_x]);
        }
      }
      output[tile_y * tile_width + tile_x] = level;
    }
  }
  return output;
}

std::vector<std::int8_t> reduce_tile_levels_to_macroblocks(std::span<const std::uint8_t> levels,
                                                           std::size_t tile_width,
                                                           std::size_t tile_height) {
  std::size_t expected = 0U;
  if (tile_width == 0U || tile_height == 0U ||
      !checked_product(tile_width, tile_height, expected) || levels.size() != expected ||
      std::any_of(levels.begin(), levels.end(),
                  [](std::uint8_t level) { return level > kMaximumEmphasisLevel; })) {
    throw std::invalid_argument("saliency macroblock reduction shape or level is invalid");
  }
  const auto macroblock_width = (tile_width + 1U) / 2U;
  const auto macroblock_height = (tile_height + 1U) / 2U;
  std::vector<std::int8_t> output(macroblock_width * macroblock_height, 0);
  for (std::size_t macroblock_y = 0U; macroblock_y < macroblock_height; ++macroblock_y) {
    for (std::size_t macroblock_x = 0U; macroblock_x < macroblock_width; ++macroblock_x) {
      std::uint8_t level = 0U;
      for (std::size_t tile_y = macroblock_y * 2U;
           tile_y < std::min(macroblock_y * 2U + 2U, tile_height); ++tile_y) {
        for (std::size_t tile_x = macroblock_x * 2U;
             tile_x < std::min(macroblock_x * 2U + 2U, tile_width); ++tile_x) {
          level = std::max(level, levels[tile_y * tile_width + tile_x]);
        }
      }
      output[macroblock_y * macroblock_width + macroblock_x] = static_cast<std::int8_t>(level);
    }
  }
  return output;
}

SaliencyReference::SaliencyReference(SaliencyConfiguration configuration)
    : implementation_(std::make_unique<Implementation>(std::move(configuration))) {
  if (!valid_saliency_configuration(implementation_->configuration)) {
    throw std::invalid_argument("saliency_v1 configuration is invalid");
  }
}

SaliencyReference::~SaliencyReference() = default;
SaliencyReference::SaliencyReference(SaliencyReference &&) noexcept = default;
SaliencyReference &SaliencyReference::operator=(SaliencyReference &&) noexcept = default;

SaliencyResult SaliencyReference::process(const LumaPlaneView &frame,
                                          const SaliencyProcessOptions &options) {
  SaliencyResult result;
  auto &output = result.output;
  output.frame_id = frame.frame_id;
  output.geometry_epoch = frame.geometry_epoch;
  output.visible_width = frame.width;
  output.visible_height = frame.height;

  if (frame.frame_id == 0U || frame.geometry_epoch == 0U || frame.width == 0U ||
      frame.height == 0U || frame.pitch < frame.width) {
    result.reason = "saliency_frame_geometry_invalid";
    return result;
  }
  std::size_t required = 0U;
  if (!checked_product(frame.height - 1U, frame.pitch, required) ||
      frame.width > std::numeric_limits<std::size_t>::max() - required ||
      frame.codes.size() < required + frame.width) {
    result.reason = "saliency_frame_buffer_too_small";
    return result;
  }
  if (options.overrides.pin_minimum_level > kMaximumEmphasisLevel ||
      options.overrides.cursor_minimum_level > kMaximumEmphasisLevel) {
    result.reason = "saliency_override_level_invalid";
    return result;
  }
  if (implementation_->has_prior && frame.frame_id <= implementation_->previous_frame_id) {
    result.reason = "saliency_frame_id_not_monotonic";
    return result;
  }
  if (implementation_->has_prior &&
      frame.monotonic_timestamp_ns < implementation_->previous_timestamp_ns) {
    result.reason = "saliency_timestamp_regressed";
    return result;
  }
  const bool same_epoch = implementation_->has_prior &&
                          frame.geometry_epoch == implementation_->previous_geometry_epoch;
  if (same_epoch && (frame.width != implementation_->previous_width ||
                     frame.height != implementation_->previous_height ||
                     frame.range != implementation_->previous_range)) {
    result.reason = "saliency_geometry_changed_without_new_epoch";
    return result;
  }

  std::size_t pixel_count = 0U;
  if (!checked_product(frame.width, frame.height, pixel_count)) {
    result.reason = "saliency_pixel_count_overflow";
    return result;
  }
  output.tile_width = (frame.width + kSaliencyTileSize - 1U) / kSaliencyTileSize;
  output.tile_height = (frame.height + kSaliencyTileSize - 1U) / kSaliencyTileSize;
  std::size_t tile_count = 0U;
  if (!checked_product(output.tile_width, output.tile_height, tile_count)) {
    result.reason = "saliency_tile_count_overflow";
    return result;
  }

  const auto normalization_start = Clock::now();
  std::vector<std::uint8_t> canonical(pixel_count);
  for (std::size_t y = 0U; y < frame.height; ++y) {
    for (std::size_t x = 0U; x < frame.width; ++x) {
      canonical[y * frame.width + x] =
          canonical_luma_code(frame.codes[y * frame.pitch + x], frame.range);
    }
  }
  output.timings.luma_normalization = elapsed_ns(normalization_start);

  const auto feature_start = Clock::now();
  std::vector<int> gradient_x(pixel_count, 0);
  std::vector<int> gradient_y(pixel_count, 0);
  std::vector<bool> high(pixel_count, false);
  constexpr std::array<std::array<int, 3U>, 3U> scharr_x = {
      std::array<int, 3U>{-3, 0, 3},
      std::array<int, 3U>{-10, 0, 10},
      std::array<int, 3U>{-3, 0, 3},
  };
  constexpr std::array<std::array<int, 3U>, 3U> scharr_y = {
      std::array<int, 3U>{-3, -10, -3},
      std::array<int, 3U>{0, 0, 0},
      std::array<int, 3U>{3, 10, 3},
  };
  for (std::size_t y = 0U; y < frame.height; ++y) {
    for (std::size_t x = 0U; x < frame.width; ++x) {
      int horizontal = 0;
      int vertical = 0;
      for (std::size_t kernel_y = 0U; kernel_y < 3U; ++kernel_y) {
        const auto source_y = std::clamp<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(y) + static_cast<std::ptrdiff_t>(kernel_y) - 1, 0,
            static_cast<std::ptrdiff_t>(frame.height - 1U));
        for (std::size_t kernel_x = 0U; kernel_x < 3U; ++kernel_x) {
          const auto source_x = std::clamp<std::ptrdiff_t>(
              static_cast<std::ptrdiff_t>(x) + static_cast<std::ptrdiff_t>(kernel_x) - 1, 0,
              static_cast<std::ptrdiff_t>(frame.width - 1U));
          const auto sample =
              static_cast<int>(canonical[static_cast<std::size_t>(source_y) * frame.width +
                                         static_cast<std::size_t>(source_x)]);
          horizontal += sample * scharr_x[kernel_y][kernel_x];
          vertical += sample * scharr_y[kernel_y][kernel_x];
        }
      }
      const auto index = y * frame.width + x;
      gradient_x[index] = horizontal;
      gradient_y[index] = vertical;
      high[index] = high_gradient(horizontal, vertical);
    }
  }

  output.tiles.resize(tile_count);
  std::vector<double> raw_scores(tile_count, 0.0);
  for (std::size_t tile_y = 0U; tile_y < output.tile_height; ++tile_y) {
    for (std::size_t tile_x = 0U; tile_x < output.tile_width; ++tile_x) {
      const auto left = tile_x * kSaliencyTileSize;
      const auto top = tile_y * kSaliencyTileSize;
      const auto right = std::min(left + kSaliencyTileSize, frame.width);
      const auto bottom = std::min(top + kSaliencyTileSize, frame.height);
      const auto tile_pixel_count = (right - left) * (bottom - top);
      std::vector<std::uint8_t> sorted_codes;
      sorted_codes.reserve(tile_pixel_count);
      std::size_t high_count = 0U;
      std::size_t small_count = 0U;
      std::size_t eligible_anchors = 0U;
      std::size_t qualifying_anchors = 0U;

      for (std::size_t y = top; y < bottom; ++y) {
        for (std::size_t x = left; x < right; ++x) {
          const auto index = y * frame.width + x;
          sorted_codes.push_back(canonical[index]);
          if (!high[index]) {
            continue;
          }
          ++high_count;
          std::size_t horizontal_run = 1U;
          for (auto scan = x; scan > left && high[y * frame.width + scan - 1U]; --scan) {
            ++horizontal_run;
          }
          for (auto scan = x + 1U; scan < right && high[y * frame.width + scan]; ++scan) {
            ++horizontal_run;
          }
          std::size_t vertical_run = 1U;
          for (auto scan = y; scan > top && high[(scan - 1U) * frame.width + x]; --scan) {
            ++vertical_run;
          }
          for (auto scan = y + 1U; scan < bottom && high[scan * frame.width + x]; ++scan) {
            ++vertical_run;
          }
          if ((horizontal_run >= 1U && horizontal_run <= 3U) ||
              (vertical_run >= 1U && vertical_run <= 3U)) {
            ++small_count;
          }
        }
      }

      for (std::size_t y = top; y < bottom; ++y) {
        for (std::size_t x = left; x < right; ++x) {
          const auto index = y * frame.width + x;
          if (x + 1U < right) {
            ++eligible_anchors;
            const auto maximum_distance = std::min<std::size_t>(12U, right - x - 1U);
            bool matched = false;
            for (std::size_t distance = 1U; distance <= maximum_distance; ++distance) {
              const auto partner = y * frame.width + x + distance;
              if (high_component(gradient_x[index]) && high_component(gradient_x[partner]) &&
                  static_cast<std::int64_t>(gradient_x[index]) * gradient_x[partner] < 0) {
                matched = true;
                break;
              }
            }
            qualifying_anchors += static_cast<std::size_t>(matched);
          }
          if (y + 1U < bottom) {
            ++eligible_anchors;
            const auto maximum_distance = std::min<std::size_t>(12U, bottom - y - 1U);
            bool matched = false;
            for (std::size_t distance = 1U; distance <= maximum_distance; ++distance) {
              const auto partner = (y + distance) * frame.width + x;
              if (high_component(gradient_y[index]) && high_component(gradient_y[partner]) &&
                  static_cast<std::int64_t>(gradient_y[index]) * gradient_y[partner] < 0) {
                matched = true;
                break;
              }
            }
            qualifying_anchors += static_cast<std::size_t>(matched);
          }
        }
      }

      std::sort(sorted_codes.begin(), sorted_codes.end());
      const auto percentile_index = [tile_pixel_count](double percentile) {
        return static_cast<std::size_t>(
            std::floor(percentile * static_cast<double>(tile_pixel_count - 1U)));
      };
      auto &features = output.tiles[tile_y * output.tile_width + tile_x];
      features.gradient_density =
          clamp01(static_cast<double>(high_count) / static_cast<double>(tile_pixel_count));
      features.local_contrast = clamp01(static_cast<double>(sorted_codes[percentile_index(0.90)] -
                                                            sorted_codes[percentile_index(0.10)]) /
                                        255.0);
      features.edge_pair_density = eligible_anchors == 0U
                                       ? 0.0
                                       : clamp01(static_cast<double>(qualifying_anchors) /
                                                 static_cast<double>(eligible_anchors));
      features.small_structure_density =
          high_count == 0U
              ? 0.0
              : clamp01(static_cast<double>(small_count) / static_cast<double>(high_count));
      features.raw_score = clamp01(
          implementation_->configuration.gradient_weight * features.gradient_density +
          implementation_->configuration.contrast_weight * features.local_contrast +
          implementation_->configuration.edge_pair_weight * features.edge_pair_density +
          implementation_->configuration.small_structure_weight * features.small_structure_density);
      raw_scores[tile_y * output.tile_width + tile_x] = features.raw_score;
    }
  }
  output.timings.feature_extraction = elapsed_ns(feature_start);

  const auto temporal_start = Clock::now();
  const bool prior_luma_compatible =
      same_epoch &&
      frame.monotonic_timestamp_ns - implementation_->previous_timestamp_ns <= kTemporalResetGapNs;
  std::vector<double> next_scores(tile_count, 0.0);
  std::vector<bool> next_active(tile_count, false);
  for (std::size_t tile_y = 0U; tile_y < output.tile_height; ++tile_y) {
    for (std::size_t tile_x = 0U; tile_x < output.tile_width; ++tile_x) {
      const auto tile_index = tile_y * output.tile_width + tile_x;
      auto &features = output.tiles[tile_index];
      if (prior_luma_compatible) {
        const auto left = tile_x * kSaliencyTileSize;
        const auto top = tile_y * kSaliencyTileSize;
        const auto right = std::min(left + kSaliencyTileSize, frame.width);
        const auto bottom = std::min(top + kSaliencyTileSize, frame.height);
        std::uint64_t absolute_change = 0U;
        for (std::size_t y = top; y < bottom; ++y) {
          for (std::size_t x = left; x < right; ++x) {
            const auto index = y * frame.width + x;
            absolute_change += static_cast<std::uint64_t>(
                std::abs(static_cast<int>(canonical[index]) -
                         static_cast<int>(implementation_->previous_luma[index])));
          }
        }
        const auto count = (right - left) * (bottom - top);
        const auto mean_code_change =
            static_cast<double>(absolute_change) / static_cast<double>(count);
        features.temporal_stability = 1.0 - std::min(mean_code_change / 32.0, 1.0);
      }
      features.current_score =
          clamp01(raw_scores[tile_index] * (0.75 + 0.25 * features.temporal_stability));
      features.filtered_score =
          !same_epoch ? features.current_score
                      : clamp01(implementation_->configuration.previous_score_coefficient *
                                    implementation_->previous_scores[tile_index] +
                                (1.0 - implementation_->configuration.previous_score_coefficient) *
                                    features.current_score);
      const bool was_active = same_epoch && implementation_->active_tiles[tile_index];
      features.active =
          was_active ? features.filtered_score >= implementation_->configuration.exit_threshold
                     : features.filtered_score >= implementation_->configuration.entry_threshold;
      features.automatic_level =
          features.active ? quantize_active_level(features.filtered_score) : 0U;
      next_scores[tile_index] = features.filtered_score;
      next_active[tile_index] = features.active;
    }
  }
  output.timings.temporal_hysteresis = elapsed_ns(temporal_start);

  const auto dilation_start = Clock::now();
  std::vector<std::uint8_t> automatic_tile_levels(tile_count, 0U);
  for (std::size_t index = 0U; index < tile_count; ++index) {
    automatic_tile_levels[index] = output.tiles[index].automatic_level;
  }
  auto final_tile_levels =
      dilate_automatic_levels(automatic_tile_levels, output.tile_width, output.tile_height,
                              implementation_->configuration.dilation_radius_tiles);
  for (std::size_t tile_y = 0U; tile_y < output.tile_height; ++tile_y) {
    for (std::size_t tile_x = 0U; tile_x < output.tile_width; ++tile_x) {
      auto &level = final_tile_levels[tile_y * output.tile_width + tile_x];
      const auto left = tile_x * kSaliencyTileSize;
      const auto top = tile_y * kSaliencyTileSize;
      const auto right = std::min(left + kSaliencyTileSize, frame.width);
      const auto bottom = std::min(top + kSaliencyTileSize, frame.height);
      if (any_intersection(options.overrides.cursor_halos, left, top, right, bottom)) {
        level = std::max(level, options.overrides.cursor_minimum_level);
      }
      if (any_intersection(options.overrides.pins, left, top, right, bottom)) {
        level = std::max(level, options.overrides.pin_minimum_level);
      }
      if (any_intersection(options.overrides.exclusions, left, top, right, bottom)) {
        level = 0U;
      }
      final_tile_levels[tile_y * output.tile_width + tile_x] = level;
      output.tiles[tile_y * output.tile_width + tile_x].final_level = level;
    }
  }
  output.timings.dilation_and_overrides = elapsed_ns(dilation_start);

  const auto reduction_start = Clock::now();
  output.macroblock_width = (frame.width + kH264MacroblockSize - 1U) / kH264MacroblockSize;
  output.macroblock_height = (frame.height + kH264MacroblockSize - 1U) / kH264MacroblockSize;
  std::size_t macroblock_count = 0U;
  if (!checked_product(output.macroblock_width, output.macroblock_height, macroblock_count)) {
    result.reason = "saliency_macroblock_count_overflow";
    return result;
  }
  output.macroblock_levels =
      reduce_tile_levels_to_macroblocks(final_tile_levels, output.tile_width, output.tile_height);
  std::size_t protected_macroblocks = 0U;
  for (const auto signed_level : output.macroblock_levels) {
    const auto level = static_cast<std::uint8_t>(signed_level);
    ++output.level_histogram[level];
    protected_macroblocks += static_cast<std::size_t>(level != 0U);
  }
  output.protected_fraction = macroblock_count == 0U ? 0.0
                                                     : static_cast<double>(protected_macroblocks) /
                                                           static_cast<double>(macroblock_count);
  output.timings.macroblock_reduction = elapsed_ns(reduction_start);

  if (options.generate_preview) {
    const auto preview_start = Clock::now();
    output.preview.width = frame.width;
    output.preview.height = frame.height;
    std::size_t preview_bytes = 0U;
    if (!checked_product(pixel_count, 4U, preview_bytes)) {
      result.reason = "saliency_preview_size_overflow";
      return result;
    }
    output.preview.rgba.resize(preview_bytes);
    for (std::size_t y = 0U; y < frame.height; ++y) {
      for (std::size_t x = 0U; x < frame.width; ++x) {
        const auto tile_index = y / kSaliencyTileSize * output.tile_width + x / kSaliencyTileSize;
        const auto color = preview_color(final_tile_levels[tile_index]);
        const auto offset = (y * frame.width + x) * 4U;
        std::copy(color.begin(), color.end(),
                  output.preview.rgba.begin() + static_cast<std::ptrdiff_t>(offset));
      }
    }
    output.timings.preview_generation = elapsed_ns(preview_start);
  }

  implementation_->has_prior = true;
  implementation_->previous_frame_id = frame.frame_id;
  implementation_->previous_geometry_epoch = frame.geometry_epoch;
  implementation_->previous_timestamp_ns = frame.monotonic_timestamp_ns;
  implementation_->previous_width = frame.width;
  implementation_->previous_height = frame.height;
  implementation_->previous_range = frame.range;
  implementation_->previous_luma = std::move(canonical);
  implementation_->previous_scores = std::move(next_scores);
  implementation_->active_tiles = std::move(next_active);
  result.passed = true;
  result.reason = "saliency_v1_complete";
  return result;
}

void SaliencyReference::reset() {
  implementation_->has_prior = false;
  implementation_->previous_frame_id = 0U;
  implementation_->previous_geometry_epoch = 0U;
  implementation_->previous_timestamp_ns = 0U;
  implementation_->previous_width = 0U;
  implementation_->previous_height = 0U;
  implementation_->previous_luma.clear();
  implementation_->previous_scores.clear();
  implementation_->active_tiles.clear();
}

const SaliencyConfiguration &SaliencyReference::configuration() const {
  return implementation_->configuration;
}

BoundedSaliencyTimingWindow::BoundedSaliencyTimingWindow(std::size_t capacity)
    : capacity_(capacity) {
  if (capacity == 0U || capacity > 1'000'000U) {
    throw std::invalid_argument("saliency timing window capacity is invalid");
  }
  samples_.reserve(capacity);
}

void BoundedSaliencyTimingWindow::observe(const SaliencyStageTimingsNs &timings) {
  if (samples_.size() == capacity_) {
    samples_.erase(samples_.begin());
  }
  samples_.push_back(timings);
}

SaliencyStageTimingsNs BoundedSaliencyTimingWindow::p95() const {
  return {
      .luma_normalization = p95_field(samples_, &SaliencyStageTimingsNs::luma_normalization),
      .feature_extraction = p95_field(samples_, &SaliencyStageTimingsNs::feature_extraction),
      .temporal_hysteresis = p95_field(samples_, &SaliencyStageTimingsNs::temporal_hysteresis),
      .dilation_and_overrides =
          p95_field(samples_, &SaliencyStageTimingsNs::dilation_and_overrides),
      .macroblock_reduction = p95_field(samples_, &SaliencyStageTimingsNs::macroblock_reduction),
      .preview_generation = p95_field(samples_, &SaliencyStageTimingsNs::preview_generation),
  };
}

std::uint64_t BoundedSaliencyTimingWindow::p95_total() const {
  if (samples_.empty()) {
    return 0U;
  }
  std::vector<std::uint64_t> values;
  values.reserve(samples_.size());
  for (const auto &sample : samples_) {
    values.push_back(sample.total());
  }
  std::sort(values.begin(), values.end());
  const auto rank = (95U * values.size() + 99U) / 100U;
  return values[rank - 1U];
}

std::size_t BoundedSaliencyTimingWindow::size() const { return samples_.size(); }
std::size_t BoundedSaliencyTimingWindow::capacity() const { return capacity_; }

} // namespace glyphrelay
