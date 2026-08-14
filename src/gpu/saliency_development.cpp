#include "glyphrelay/saliency_development.hpp"

#include "glyphrelay/saliency.hpp"

#include <algorithm>

namespace glyphrelay {

SaliencySequenceMetrics::SaliencySequenceMetrics(std::size_t visible_macroblocks,
                                                 bool static_sequence)
    : visible_macroblocks_(visible_macroblocks), static_sequence_(static_sequence) {}

SaliencyMapOperation SaliencySequenceMetrics::observe(const SaliencyMapObservation &observation) {
  if (visible_macroblocks_ == 0U || observation.levels.size() != visible_macroblocks_ ||
      observation.glyph_truth.size() != visible_macroblocks_ ||
      observation.small_glyph_truth.size() != visible_macroblocks_ ||
      observation.ui_truth.size() != visible_macroblocks_) {
    return {false, "saliency_development_map_shape_invalid"};
  }
  if (std::any_of(observation.levels.begin(), observation.levels.end(),
                  [](std::int8_t level) {
                    return level < 0 || level > static_cast<std::int8_t>(kMaximumEmphasisLevel);
                  }) ||
      std::any_of(observation.glyph_truth.begin(), observation.glyph_truth.end(),
                  [](std::uint8_t value) { return value > 1U; }) ||
      std::any_of(observation.small_glyph_truth.begin(), observation.small_glyph_truth.end(),
                  [](std::uint8_t value) { return value > 1U; }) ||
      std::any_of(observation.ui_truth.begin(), observation.ui_truth.end(),
                  [](std::uint8_t value) { return value > 1U; })) {
    return {false, "saliency_development_map_value_invalid"};
  }

  std::size_t protected_count = 0U;
  std::size_t false_protected_count = 0U;
  std::size_t changed_count = 0U;
  for (std::size_t index = 0U; index < visible_macroblocks_; ++index) {
    if (observation.small_glyph_truth[index] != 0U && observation.glyph_truth[index] == 0U) {
      return {false, "saliency_development_small_truth_not_glyph"};
    }
    const bool protected_macroblock = observation.levels[index] != 0;
    const bool glyph = observation.glyph_truth[index] != 0U;
    const bool small_glyph = observation.small_glyph_truth[index] != 0U;
    const bool ui = observation.ui_truth[index] != 0U;
    protected_count += static_cast<std::size_t>(protected_macroblock);
    false_protected_count += static_cast<std::size_t>(protected_macroblock && !glyph && !ui);
    glyph_truth_count_ += static_cast<std::uint64_t>(glyph);
    protected_glyph_count_ += static_cast<std::uint64_t>(protected_macroblock && glyph);
    small_glyph_truth_count_ += static_cast<std::uint64_t>(small_glyph);
    protected_small_glyph_count_ += static_cast<std::uint64_t>(protected_macroblock && small_glyph);
    if (static_sequence_ && !previous_levels_.empty() &&
        observation.levels[index] != previous_levels_[index]) {
      ++changed_count;
    }
  }
  const auto visible = static_cast<double>(visible_macroblocks_);
  protected_fraction_sum_ += static_cast<double>(protected_count) / visible;
  false_protected_fraction_sum_ += static_cast<double>(false_protected_count) / visible;
  false_discovery_fraction_sum_ += static_cast<double>(false_protected_count) /
                                   static_cast<double>(std::max<std::size_t>(protected_count, 1U));
  if (static_sequence_ && !previous_levels_.empty()) {
    static_change_fraction_sum_ += static_cast<double>(changed_count) / visible;
    ++static_change_count_;
  }
  if (static_sequence_) {
    previous_levels_.assign(observation.levels.begin(), observation.levels.end());
  }
  ++observed_frames_;
  return {true, "saliency_development_map_observed"};
}

SaliencyMapMetrics SaliencySequenceMetrics::finalize() const {
  SaliencyMapMetrics output;
  if (observed_frames_ == 0U) {
    return output;
  }
  output.overall_glyph_recall =
      glyph_truth_count_ == 0U
          ? 0.0
          : static_cast<double>(protected_glyph_count_) / static_cast<double>(glyph_truth_count_);
  output.small_glyph_recall = small_glyph_truth_count_ == 0U
                                  ? 0.0
                                  : static_cast<double>(protected_small_glyph_count_) /
                                        static_cast<double>(small_glyph_truth_count_);
  const auto frames = static_cast<double>(observed_frames_);
  output.protected_fraction = protected_fraction_sum_ / frames;
  output.false_protected_fraction = false_protected_fraction_sum_ / frames;
  output.false_discovery_fraction = false_discovery_fraction_sum_ / frames;
  output.static_map_change_observed = static_change_count_ != 0U;
  output.static_map_change_fraction =
      static_change_count_ == 0U
          ? 0.0
          : static_change_fraction_sum_ / static_cast<double>(static_change_count_);
  return output;
}

std::size_t SaliencySequenceMetrics::observed_frames() const { return observed_frames_; }

} // namespace glyphrelay
