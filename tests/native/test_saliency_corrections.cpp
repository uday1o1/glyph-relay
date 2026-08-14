#include "glyphrelay/saliency_corrections.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

void test_bounded_revisioned_correction_state() {
  glyphrelay::SaliencyCorrectionState corrections(64U, 48U, 7U, 2U);
  const auto pin = corrections.add(glyphrelay::SaliencyCorrectionKind::pin, {8U, 8U, 16U, 8U}, 0U);
  require(pin.passed && pin.region_id == 1U && pin.revision == 1U,
          "the first correction must receive a stable identity and revision");
  const auto stale =
      corrections.add(glyphrelay::SaliencyCorrectionKind::exclusion, {8U, 8U, 8U, 8U}, 0U);
  require(!stale.passed && stale.reason == "saliency_correction_revision_stale" &&
              corrections.regions().size() == 1U,
          "a stale correction must fail without changing state");
  const auto overflow =
      corrections.add(glyphrelay::SaliencyCorrectionKind::exclusion,
                      {std::numeric_limits<std::size_t>::max() - 1U, 0U, 4U, 1U}, 1U);
  require(!overflow.passed && overflow.reason == "saliency_correction_rectangle_invalid",
          "overflowing source-visible coordinates must fail closed");
  const auto exclusion =
      corrections.add(glyphrelay::SaliencyCorrectionKind::exclusion, {12U, 8U, 8U, 8U}, 1U);
  require(exclusion.passed && exclusion.region_id == 2U && exclusion.revision == 2U,
          "a bounded exclusion must be accepted at the current revision");
  const auto capacity =
      corrections.add(glyphrelay::SaliencyCorrectionKind::pin, {32U, 8U, 8U, 8U}, 2U);
  require(!capacity.passed && capacity.reason == "saliency_correction_capacity_reached",
          "the correction collection must remain bounded");

  const std::array cursor_halos = {glyphrelay::SaliencyRectangle{40U, 16U, 8U, 8U}};
  const auto overrides = corrections.overrides(cursor_halos);
  require(overrides.pins.size() == 1U && overrides.exclusions.size() == 1U &&
              overrides.cursor_halos.size() == 1U,
          "corrections and cursor state must compose into one saliency override contract");
  const auto removed = corrections.remove(pin.region_id, 2U);
  require(removed.passed && removed.revision == 3U && corrections.regions().size() == 1U,
          "removal must use stable identity and advance the revision");
  const auto missing = corrections.remove(pin.region_id, 3U);
  require(!missing.passed && missing.reason == "saliency_correction_not_found",
          "repeated removal must not report success");
  const auto reset = corrections.reset_geometry(80U, 60U, 8U, 3U);
  require(reset.passed && reset.revision == 4U && corrections.regions().empty() &&
              corrections.visible_width() == 80U && corrections.visible_height() == 60U &&
              corrections.geometry_epoch() == 8U,
          "a new geometry epoch must clear source-visible corrections atomically");
}

void test_conflict_overlay_preserves_exclusion_map() {
  glyphrelay::SaliencyOverrides overrides;
  overrides.pins = {{8U, 0U, 8U, 8U}};
  overrides.exclusions = {{12U, 0U, 8U, 8U}};
  const auto conflicts = glyphrelay::saliency_correction_conflict_tiles(24U, 8U, overrides);
  require(conflicts == std::vector<std::uint8_t>({0U, 1U, 0U}),
          "only the tile intersecting both correction kinds must be marked conflicted");

  std::vector<std::uint8_t> luma(24U * 8U, 0U);
  glyphrelay::SaliencyReference saliency;
  glyphrelay::SaliencyProcessOptions options;
  options.generate_preview = true;
  options.overrides = overrides;
  const auto result = saliency.process({.codes = luma,
                                        .width = 24U,
                                        .height = 8U,
                                        .pitch = 24U,
                                        .range = glyphrelay::ColorRange::full,
                                        .frame_id = 1U,
                                        .geometry_epoch = 1U,
                                        .monotonic_timestamp_ns = 1U},
                                       options);
  require(result.passed && result.output.macroblock_levels == std::vector<std::int8_t>({0, 0}),
          "an exclusion must continue to win in the encoder-facing map");
  auto preview = result.output.preview;
  glyphrelay::overlay_saliency_correction_conflicts(preview, conflicts, 3U, 1U);
  const auto conflict_offset = (0U * 24U + 8U) * 4U;
  require(preview.rgba[conflict_offset] == 224U && preview.rgba[conflict_offset + 1U] == 48U &&
              preview.rgba[conflict_offset + 2U] == 255U &&
              preview.rgba[conflict_offset + 3U] == 208U,
          "the local preview must visibly distinguish a pin-exclusion conflict");
}

void test_invalid_construction_and_preview_contracts() {
  bool threw = false;
  try {
    static_cast<void>(glyphrelay::SaliencyCorrectionState(0U, 1U, 1U));
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  require(threw, "invalid correction geometry must fail at construction");

  glyphrelay::SaliencyPreview preview{.width = 8U, .height = 8U, .rgba = {}};
  threw = false;
  try {
    const std::vector<std::uint8_t> conflicts = {1U};
    glyphrelay::overlay_saliency_correction_conflicts(preview, conflicts, 1U, 1U);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  require(threw, "a malformed preview buffer must fail instead of writing out of bounds");
}

} // namespace

int main() {
  test_bounded_revisioned_correction_state();
  test_conflict_overlay_preserves_exclusion_map();
  test_invalid_construction_and_preview_contracts();
  return 0;
}
