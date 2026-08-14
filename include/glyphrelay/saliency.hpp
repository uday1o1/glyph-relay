#pragma once

#include "glyphrelay/color_conversion.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace glyphrelay {

constexpr std::size_t kSaliencyTileSize = 8U;
constexpr std::size_t kH264MacroblockSize = 16U;
constexpr std::uint8_t kMaximumEmphasisLevel = 5U;

struct LumaPlaneView {
  std::span<const std::uint8_t> codes;
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t pitch = 0;
  ColorRange range = ColorRange::limited;
  std::uint64_t frame_id = 0;
  std::uint64_t geometry_epoch = 0;
  std::uint64_t monotonic_timestamp_ns = 0;
};

struct SaliencyConfiguration {
  double gradient_weight = 0.35;
  double contrast_weight = 0.25;
  double edge_pair_weight = 0.25;
  double small_structure_weight = 0.15;
  double entry_threshold = 0.55;
  double exit_threshold = 0.40;
  double previous_score_coefficient = 0.60;
  std::size_t dilation_radius_tiles = 1U;
};

struct SaliencyRectangle {
  std::size_t x = 0;
  std::size_t y = 0;
  std::size_t width = 0;
  std::size_t height = 0;
};

struct SaliencyOverrides {
  std::vector<SaliencyRectangle> exclusions;
  std::vector<SaliencyRectangle> pins;
  std::vector<SaliencyRectangle> cursor_halos;
  std::uint8_t pin_minimum_level = 4U;
  std::uint8_t cursor_minimum_level = 3U;
};

struct SaliencyProcessOptions {
  SaliencyOverrides overrides;
  bool generate_preview = false;
};

struct TileFeatureVector {
  double gradient_density = 0.0;
  double local_contrast = 0.0;
  double edge_pair_density = 0.0;
  double small_structure_density = 0.0;
  double raw_score = 0.0;
  double temporal_stability = 1.0;
  double current_score = 0.0;
  double filtered_score = 0.0;
  bool active = false;
  std::uint8_t automatic_level = 0U;
  std::uint8_t final_level = 0U;
};

struct SaliencyStageTimingsNs {
  std::uint64_t luma_normalization = 0U;
  std::uint64_t feature_extraction = 0U;
  std::uint64_t temporal_hysteresis = 0U;
  std::uint64_t dilation_and_overrides = 0U;
  std::uint64_t macroblock_reduction = 0U;
  std::uint64_t preview_generation = 0U;

  std::uint64_t total() const;
};

struct SaliencyPreview {
  std::size_t width = 0;
  std::size_t height = 0;
  std::vector<std::uint8_t> rgba;
};

struct SaliencyOutput {
  std::uint64_t frame_id = 0;
  std::uint64_t geometry_epoch = 0;
  std::size_t visible_width = 0;
  std::size_t visible_height = 0;
  std::size_t tile_width = 0;
  std::size_t tile_height = 0;
  std::size_t macroblock_width = 0;
  std::size_t macroblock_height = 0;
  std::vector<TileFeatureVector> tiles;
  std::vector<std::int8_t> macroblock_levels;
  std::array<std::uint64_t, kMaximumEmphasisLevel + 1U> level_histogram{};
  double protected_fraction = 0.0;
  SaliencyStageTimingsNs timings;
  SaliencyPreview preview;
};

struct SaliencyResult {
  bool passed = false;
  std::string reason;
  SaliencyOutput output;
};

class SaliencyReference {
public:
  explicit SaliencyReference(SaliencyConfiguration configuration = {});
  ~SaliencyReference();
  SaliencyReference(SaliencyReference &&) noexcept;
  SaliencyReference &operator=(SaliencyReference &&) noexcept;
  SaliencyReference(const SaliencyReference &) = delete;
  SaliencyReference &operator=(const SaliencyReference &) = delete;

  SaliencyResult process(const LumaPlaneView &frame, const SaliencyProcessOptions &options = {});
  void reset();
  const SaliencyConfiguration &configuration() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

class BoundedSaliencyTimingWindow {
public:
  explicit BoundedSaliencyTimingWindow(std::size_t capacity);

  void observe(const SaliencyStageTimingsNs &timings);
  SaliencyStageTimingsNs p95() const;
  std::uint64_t p95_total() const;
  std::size_t size() const;
  std::size_t capacity() const;

private:
  std::size_t capacity_ = 0U;
  std::vector<SaliencyStageTimingsNs> samples_;
};

std::uint8_t canonical_luma_code(std::uint8_t code, ColorRange range);
bool valid_saliency_configuration(const SaliencyConfiguration &configuration);
std::vector<std::uint8_t> dilate_automatic_levels(std::span<const std::uint8_t> levels,
                                                  std::size_t tile_width, std::size_t tile_height,
                                                  std::size_t radius);
std::vector<std::int8_t> reduce_tile_levels_to_macroblocks(std::span<const std::uint8_t> levels,
                                                           std::size_t tile_width,
                                                           std::size_t tile_height);

} // namespace glyphrelay
