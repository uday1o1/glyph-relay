#pragma once

#include "glyphrelay/saliency.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace glyphrelay {

enum class SaliencyCorrectionKind {
  pin,
  exclusion,
};

struct SaliencyCorrectionRegion {
  std::uint64_t id = 0U;
  SaliencyCorrectionKind kind = SaliencyCorrectionKind::pin;
  SaliencyRectangle rectangle;
};

struct SaliencyCorrectionOperation {
  bool passed = false;
  std::string reason;
  std::uint64_t revision = 0U;
  std::uint64_t region_id = 0U;
};

class SaliencyCorrectionState {
public:
  SaliencyCorrectionState(std::size_t visible_width, std::size_t visible_height,
                          std::uint64_t geometry_epoch, std::size_t capacity = 64U);

  SaliencyCorrectionOperation add(SaliencyCorrectionKind kind, SaliencyRectangle rectangle,
                                  std::uint64_t expected_revision);
  SaliencyCorrectionOperation remove(std::uint64_t region_id, std::uint64_t expected_revision);
  SaliencyCorrectionOperation reset_geometry(std::size_t visible_width, std::size_t visible_height,
                                             std::uint64_t geometry_epoch,
                                             std::uint64_t expected_revision);
  SaliencyOverrides overrides(std::span<const SaliencyRectangle> cursor_halos = {}) const;

  std::size_t visible_width() const;
  std::size_t visible_height() const;
  std::uint64_t geometry_epoch() const;
  std::uint64_t revision() const;
  std::size_t capacity() const;
  const std::vector<SaliencyCorrectionRegion> &regions() const;

private:
  std::size_t visible_width_ = 0U;
  std::size_t visible_height_ = 0U;
  std::uint64_t geometry_epoch_ = 0U;
  std::uint64_t revision_ = 0U;
  std::uint64_t next_id_ = 1U;
  std::size_t capacity_ = 0U;
  std::vector<SaliencyCorrectionRegion> regions_;
};

std::vector<std::uint8_t> saliency_correction_conflict_tiles(std::size_t visible_width,
                                                             std::size_t visible_height,
                                                             const SaliencyOverrides &overrides);

void overlay_saliency_correction_conflicts(SaliencyPreview &preview,
                                           std::span<const std::uint8_t> conflict_tiles,
                                           std::size_t tile_width, std::size_t tile_height);

} // namespace glyphrelay
