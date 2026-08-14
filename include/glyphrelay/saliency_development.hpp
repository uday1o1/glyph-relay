#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace glyphrelay {

struct SaliencyMapMetrics {
  double overall_glyph_recall = 0.0;
  double small_glyph_recall = 0.0;
  double protected_fraction = 0.0;
  double false_protected_fraction = 0.0;
  double false_discovery_fraction = 0.0;
  double static_map_change_fraction = 0.0;
  bool static_map_change_observed = false;
};

struct SaliencyMapObservation {
  std::span<const std::int8_t> levels;
  std::span<const std::uint8_t> glyph_truth;
  std::span<const std::uint8_t> small_glyph_truth;
  std::span<const std::uint8_t> ui_truth;
};

struct SaliencyMapOperation {
  bool passed = false;
  std::string reason;
};

class SaliencySequenceMetrics {
public:
  SaliencySequenceMetrics(std::size_t visible_macroblocks, bool static_sequence);

  SaliencyMapOperation observe(const SaliencyMapObservation &observation);
  SaliencyMapMetrics finalize() const;
  std::size_t observed_frames() const;

private:
  std::size_t visible_macroblocks_ = 0U;
  bool static_sequence_ = false;
  std::size_t observed_frames_ = 0U;
  std::uint64_t glyph_truth_count_ = 0U;
  std::uint64_t protected_glyph_count_ = 0U;
  std::uint64_t small_glyph_truth_count_ = 0U;
  std::uint64_t protected_small_glyph_count_ = 0U;
  double protected_fraction_sum_ = 0.0;
  double false_protected_fraction_sum_ = 0.0;
  double false_discovery_fraction_sum_ = 0.0;
  double static_change_fraction_sum_ = 0.0;
  std::size_t static_change_count_ = 0U;
  std::vector<std::int8_t> previous_levels_;
};

} // namespace glyphrelay
