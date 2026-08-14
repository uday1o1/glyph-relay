#include "glyphrelay/saliency.hpp"
#include "glyphrelay/saliency_development.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

void require_near(double actual, double expected, double tolerance, const char *message) {
  require(std::abs(actual - expected) <= tolerance, message);
}

glyphrelay::LumaPlaneView view(const std::vector<std::uint8_t> &codes, std::size_t width,
                               std::size_t height, std::uint64_t frame_id = 1U,
                               std::uint64_t geometry_epoch = 1U, std::uint64_t timestamp_ns = 0U,
                               glyphrelay::ColorRange range = glyphrelay::ColorRange::full) {
  return {
      .codes = codes,
      .width = width,
      .height = height,
      .pitch = width,
      .range = range,
      .frame_id = frame_id,
      .geometry_epoch = geometry_epoch,
      .monotonic_timestamp_ns = timestamp_ns,
  };
}

std::vector<std::uint8_t> vertical_step() {
  std::vector<std::uint8_t> codes(64U, 0U);
  for (std::size_t y = 0U; y < 8U; ++y) {
    for (std::size_t x = 4U; x < 8U; ++x) {
      codes[y * 8U + x] = 255U;
    }
  }
  return codes;
}

std::vector<std::uint8_t> vertical_stripe() {
  std::vector<std::uint8_t> codes(64U, 0U);
  for (std::size_t y = 0U; y < 8U; ++y) {
    codes[y * 8U + 3U] = 255U;
    codes[y * 8U + 4U] = 255U;
  }
  return codes;
}

std::vector<std::uint8_t> isolated_pixel(std::size_t x, std::size_t y) {
  std::vector<std::uint8_t> codes(64U, 0U);
  codes[y * 8U + x] = 255U;
  return codes;
}

glyphrelay::SaliencyConfiguration no_dilation() {
  glyphrelay::SaliencyConfiguration configuration;
  configuration.dilation_radius_tiles = 0U;
  return configuration;
}

void test_luma_expansion_and_configuration_contract() {
  require(glyphrelay::canonical_luma_code(0U, glyphrelay::ColorRange::limited) == 0U &&
              glyphrelay::canonical_luma_code(16U, glyphrelay::ColorRange::limited) == 0U &&
              glyphrelay::canonical_luma_code(126U, glyphrelay::ColorRange::limited) == 128U &&
              glyphrelay::canonical_luma_code(235U, glyphrelay::ColorRange::limited) == 255U &&
              glyphrelay::canonical_luma_code(255U, glyphrelay::ColorRange::limited) == 255U &&
              glyphrelay::canonical_luma_code(126U, glyphrelay::ColorRange::full) == 126U,
          "limited luma must expand canonically without per-frame normalization");
  require(glyphrelay::valid_saliency_configuration(no_dilation()),
          "the frozen initial saliency configuration must be valid");
  auto invalid = no_dilation();
  invalid.gradient_weight = 0.36;
  require(!glyphrelay::valid_saliency_configuration(invalid),
          "feature weights that do not sum to one must fail");
  invalid = no_dilation();
  invalid.exit_threshold = invalid.entry_threshold;
  require(!glyphrelay::valid_saliency_configuration(invalid),
          "the exit threshold must remain strictly below entry");
}

void test_hand_calculated_feature_vectors() {
  const std::vector<std::uint8_t> uniform(64U, 73U);
  glyphrelay::SaliencyReference uniform_reference(no_dilation());
  const auto uniform_result = uniform_reference.process(view(uniform, 8U, 8U));
  require(uniform_result.passed && uniform_result.output.tiles.size() == 1U,
          "the uniform golden must produce one tile");
  const auto &uniform_features = uniform_result.output.tiles.front();
  require(uniform_features.gradient_density == 0.0 && uniform_features.local_contrast == 0.0 &&
              uniform_features.edge_pair_density == 0.0 &&
              uniform_features.small_structure_density == 0.0 &&
              uniform_features.filtered_score == 0.0 && !uniform_features.active,
          "the uniform tile must have the all-zero hand-calculated feature vector");

  const auto step = vertical_step();
  glyphrelay::SaliencyReference step_reference(no_dilation());
  const auto step_result = step_reference.process(view(step, 8U, 8U));
  require(step_result.passed, "the single-edge golden must process");
  const auto &step_features = step_result.output.tiles.front();
  require_near(step_features.gradient_density, 0.25, 1e-12,
               "a centered single edge has sixteen high-gradient pixels");
  require_near(step_features.local_contrast, 1.0, 1e-12,
               "the single-edge percentile contrast must span the full code range");
  require_near(step_features.edge_pair_density, 0.0, 1e-12,
               "one edge has no opposite-sign edge pair");
  require_near(step_features.small_structure_density, 1.0, 1e-12,
               "the two-column high-gradient run is a small structure");
  require_near(step_features.raw_score, 0.4875, 1e-12,
               "the single-edge weighted score must match the frozen formula");
  require(!step_features.active && step_features.automatic_level == 0U,
          "a 0.4875 tile remains below the initial entry threshold");

  const auto stripe = vertical_stripe();
  glyphrelay::SaliencyReference stripe_reference(no_dilation());
  const auto stripe_result = stripe_reference.process(view(stripe, 8U, 8U));
  require(stripe_result.passed, "the opposite-edge golden must process");
  const auto &stripe_features = stripe_result.output.tiles.front();
  require_near(stripe_features.gradient_density, 0.5, 1e-12,
               "the two-pixel stripe has thirty-two high-gradient pixels");
  require_near(stripe_features.local_contrast, 1.0, 1e-12,
               "the stripe percentile contrast must span the full code range");
  require_near(stripe_features.edge_pair_density, 1.0 / 7.0, 1e-12,
               "canonical forward anchors count each opposite-edge pair once");
  require_near(stripe_features.small_structure_density, 0.0, 1e-12,
               "a four-column by eight-row gradient component is not a small structure");
  require_near(stripe_features.raw_score, 0.4607142857142857, 1e-12,
               "the opposite-edge weighted score must match the frozen formula");

  const auto isolated = isolated_pixel(3U, 3U);
  glyphrelay::SaliencyReference isolated_reference(no_dilation());
  const auto isolated_result = isolated_reference.process(view(isolated, 8U, 8U));
  require(isolated_result.passed, "the isolated-pixel golden must process");
  const auto &isolated_features = isolated_result.output.tiles.front();
  require_near(isolated_features.gradient_density, 0.125, 1e-12,
               "an interior isolated pixel has eight high-gradient neighbors");
  require_near(isolated_features.local_contrast, 0.0, 1e-12,
               "one bright sample is below the frozen ninetieth percentile index");
  require_near(isolated_features.edge_pair_density, 3.0 / 56.0, 1e-12,
               "the isolated-pixel ring has six canonical opposite-edge anchors");
  require_near(isolated_features.small_structure_density, 1.0, 1e-12,
               "every isolated-pixel gradient belongs to a short run");
  require_near(isolated_features.raw_score, 0.20714285714285713, 1e-12,
               "the isolated-pixel score must match the hand-calculated vector");

  const auto border = isolated_pixel(0U, 0U);
  glyphrelay::SaliencyReference border_reference(no_dilation());
  const auto border_result = border_reference.process(view(border, 8U, 8U));
  require(border_result.passed, "the border golden must process");
  const auto &border_features = border_result.output.tiles.front();
  require_near(border_features.gradient_density, 0.0625, 1e-12,
               "clamp-to-edge extension gives a two-by-two border gradient");
  require_near(border_features.local_contrast, 0.0, 1e-12,
               "the border sample remains below the frozen percentile index");
  require_near(border_features.edge_pair_density, 0.0, 1e-12,
               "the border gradient has no opposite-sign component pair");
  require_near(border_features.small_structure_density, 1.0, 1e-12,
               "every border gradient belongs to a two-pixel run");
  require_near(border_features.raw_score, 0.171875, 1e-12,
               "the border score must match the hand-calculated vector");
}

void test_temporal_hysteresis_dropped_frame_and_geometry_reset() {
  auto configuration = no_dilation();
  configuration.entry_threshold = 0.10;
  configuration.exit_threshold = 0.05;
  glyphrelay::SaliencyReference reference(configuration);
  const std::vector<std::uint8_t> uniform(64U, 0U);
  const auto first = reference.process(view(uniform, 8U, 8U, 1U, 1U, 1U));
  const auto step = vertical_step();
  const auto changed = reference.process(view(step, 8U, 8U, 2U, 1U, 33'333'334U));
  require(first.passed && changed.passed, "the temporal fixture must process in order");
  const auto &changed_features = changed.output.tiles.front();
  require_near(changed_features.temporal_stability, 0.0, 1e-12,
               "a half-frame full-code change saturates temporal instability");
  require_near(changed_features.current_score, 0.365625, 1e-12,
               "unstable new content keeps the frozen 0.75 score floor");
  require_near(changed_features.filtered_score, 0.14625, 1e-12,
               "the previous-frame coefficient must use only past processed state");
  require(changed_features.active && changed_features.automatic_level == 1U,
          "the configured low entry threshold must activate the filtered tile");

  const auto stable = reference.process(view(step, 8U, 8U, 3U, 1U, 66'666'667U));
  require(stable.passed && stable.output.tiles.front().temporal_stability == 1.0,
          "an unchanged processed successor must be fully stable");
  require_near(stable.output.tiles.front().filtered_score, 0.28275, 1e-12,
               "stable temporal filtering must use the immediately prior score");

  const auto dropped_gap = reference.process(view(step, 8U, 8U, 7U, 1U, 300'000'001U));
  require(dropped_gap.passed && dropped_gap.output.tiles.front().temporal_stability == 1.0,
          "a predecessor gap above 200 ms must reset only the luma-change comparison");
  const auto reset_epoch = reference.process(view(step, 8U, 8U, 8U, 2U, 333'333'334U));
  require(reset_epoch.passed && reset_epoch.output.tiles.front().filtered_score ==
                                    reset_epoch.output.tiles.front().current_score,
          "a geometry epoch change must reset score and activity history");

  const auto duplicate = reference.process(view(step, 8U, 8U, 8U, 2U, 333'333'335U));
  require(!duplicate.passed && duplicate.reason == "saliency_frame_id_not_monotonic",
          "a duplicate frame identity must fail before mutating temporal state");
}

void test_morphology_reduction_overrides_and_preview() {
  const std::array<std::uint8_t, 6U> levels = {0U, 0U, 0U, 0U, 4U, 0U};
  require(glyphrelay::dilate_automatic_levels(levels, 3U, 2U, 1U) ==
                  std::vector<std::uint8_t>({4U, 4U, 4U, 4U, 4U, 4U}) &&
              glyphrelay::dilate_automatic_levels(levels, 3U, 2U, 0U) ==
                  std::vector<std::uint8_t>(levels.begin(), levels.end()),
          "Chebyshev dilation must clip at every partial-grid boundary");
  const std::array<std::uint8_t, 9U> reduction = {1U, 2U, 3U, 4U, 5U, 0U, 1U, 0U, 2U};
  require(glyphrelay::reduce_tile_levels_to_macroblocks(reduction, 3U, 3U) ==
              std::vector<std::int8_t>({5, 3, 1, 2}),
          "macroblock reduction must take raster-order maxima over clipped two-by-two tiles");

  const std::vector<std::uint8_t> uniform(17U * 17U, 0U);
  glyphrelay::SaliencyReference reference(no_dilation());
  glyphrelay::SaliencyProcessOptions options;
  options.generate_preview = true;
  options.overrides.pins = {{16U, 0U, 1U, 1U}, {0U, 0U, 1U, 1U}};
  options.overrides.cursor_halos = {{0U, 16U, 1U, 1U}};
  options.overrides.exclusions = {{0U, 0U, 1U, 1U}};
  const auto result = reference.process(view(uniform, 17U, 17U), options);
  require(result.passed && result.output.tile_width == 3U && result.output.tile_height == 3U &&
              result.output.macroblock_width == 2U && result.output.macroblock_height == 2U &&
              result.output.macroblock_levels == std::vector<std::int8_t>({0, 4, 3, 0}),
          "exclusion, pin, cursor, and partial macroblock precedence must be deterministic");
  require(result.output.preview.width == 17U && result.output.preview.height == 17U &&
              result.output.preview.rgba.size() == 17U * 17U * 4U,
          "the local protected-region preview must cover the exact visible geometry");
  const auto pinned_offset = (0U * 17U + 16U) * 4U;
  require(result.output.preview.rgba[pinned_offset] == 255U &&
              result.output.preview.rgba[pinned_offset + 1U] == 128U &&
              result.output.preview.rgba[pinned_offset + 2U] == 24U &&
              result.output.preview.rgba[pinned_offset + 3U] == 128U,
          "the preview must encode the frozen level-four overlay color");
}

void test_boundaries_determinism_and_timing_window() {
  const std::vector<std::uint8_t> one_pixel = {235U};
  glyphrelay::SaliencyReference boundary_reference(no_dilation());
  const auto boundary = boundary_reference.process(
      view(one_pixel, 1U, 1U, 1U, 1U, 0U, glyphrelay::ColorRange::limited));
  require(boundary.passed && boundary.output.tile_width == 1U &&
              boundary.output.tile_height == 1U &&
              boundary.output.macroblock_levels == std::vector<std::int8_t>{0},
          "a one-pixel partial tile and macroblock must remain in bounds");

  std::mt19937 random(0x53414C31U);
  std::uniform_int_distribution<unsigned int> byte(0U, 255U);
  glyphrelay::SaliencyReference left(no_dilation());
  glyphrelay::SaliencyReference right(no_dilation());
  for (std::uint64_t frame_id = 1U; frame_id <= 20U; ++frame_id) {
    std::vector<std::uint8_t> codes(23U * 17U);
    for (auto &code : codes) {
      code = static_cast<std::uint8_t>(byte(random));
    }
    const auto frame = view(codes, 23U, 17U, frame_id, 4U, frame_id * 33'333'333U);
    const auto left_result = left.process(frame);
    const auto right_result = right.process(frame);
    require(left_result.passed && right_result.passed &&
                left_result.output.macroblock_levels == right_result.output.macroblock_levels &&
                left_result.output.tiles.size() == right_result.output.tiles.size(),
            "the same randomized frame sequence must produce identical maps");
    for (std::size_t index = 0U; index < left_result.output.tiles.size(); ++index) {
      const auto &a = left_result.output.tiles[index];
      const auto &b = right_result.output.tiles[index];
      require(a.gradient_density == b.gradient_density && a.local_contrast == b.local_contrast &&
                  a.edge_pair_density == b.edge_pair_density &&
                  a.small_structure_density == b.small_structure_density &&
                  a.filtered_score == b.filtered_score && a.active == b.active &&
                  a.final_level == b.final_level,
              "determinism must cover every feature and hysteresis field");
    }
  }

  glyphrelay::BoundedSaliencyTimingWindow timings(3U);
  timings.observe({.feature_extraction = 100U});
  timings.observe({.feature_extraction = 300U});
  timings.observe({.feature_extraction = 200U});
  timings.observe({.feature_extraction = 400U});
  require(
      timings.size() == 3U && timings.capacity() == 3U &&
          timings.p95().feature_extraction == 400U && timings.p95_total() == 400U,
      "the timing series must evict oldest samples and report stage and total nearest-rank P95");
  glyphrelay::BoundedSaliencyTimingWindow independent_stages(2U);
  independent_stages.observe({.luma_normalization = 400U});
  independent_stages.observe({.feature_extraction = 400U});
  require(independent_stages.p95().total() == 800U && independent_stages.p95_total() == 400U,
          "total P95 must rank sample totals instead of summing non-coincident stage percentiles");
}

void test_development_map_metric_formulas() {
  glyphrelay::SaliencySequenceMetrics metrics(4U, true);
  const std::array<std::uint8_t, 4U> glyph = {1U, 1U, 0U, 0U};
  const std::array<std::uint8_t, 4U> small = {1U, 0U, 0U, 0U};
  const std::array<std::uint8_t, 4U> ui = {0U, 0U, 1U, 0U};
  const std::array<std::int8_t, 4U> first = {1, 0, 2, 0};
  const std::array<std::int8_t, 4U> second = {0, 0, 2, 3};
  require(metrics.observe({first, glyph, small, ui}).passed &&
              metrics.observe({second, glyph, small, ui}).passed,
          "valid development maps must be observed");
  const auto result = metrics.finalize();
  require_near(result.overall_glyph_recall, 0.25, 1e-12,
               "sequence glyph recall must use total protected glyph macroblocks");
  require_near(result.small_glyph_recall, 0.5, 1e-12,
               "small-glyph recall must use only the frozen small subset");
  require_near(result.protected_fraction, 0.5, 1e-12,
               "protected fraction must average sampled-frame fractions");
  require_near(result.false_protected_fraction, 0.125, 1e-12,
               "false protection must exclude glyph and declared UI truth");
  require_near(result.false_discovery_fraction, 0.25, 1e-12,
               "false discovery must divide by protected macroblocks per frame");
  require(result.static_map_change_observed,
          "a static sequence with two samples must report map change");
  require_near(result.static_map_change_fraction, 0.5, 1e-12,
               "static map change must compare exact adjacent emphasis levels");

  glyphrelay::SaliencySequenceMetrics invalid(1U, false);
  const std::array<std::int8_t, 1U> level = {1};
  const std::array<std::uint8_t, 1U> absent = {0U};
  const std::array<std::uint8_t, 1U> present = {1U};
  require(!invalid.observe({level, absent, present, absent}).passed,
          "small-glyph truth outside glyph truth must fail closed");
}

} // namespace

int main() {
  test_luma_expansion_and_configuration_contract();
  test_hand_calculated_feature_vectors();
  test_temporal_hysteresis_dropped_frame_and_geometry_reset();
  test_morphology_reduction_overrides_and_preview();
  test_boundaries_determinism_and_timing_window();
  test_development_map_metric_formulas();
  return 0;
}
