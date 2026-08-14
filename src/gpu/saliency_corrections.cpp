#include "glyphrelay/saliency_corrections.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace glyphrelay {
namespace {

bool checked_add(std::size_t left, std::size_t right, std::size_t &result) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool valid_geometry(std::size_t width, std::size_t height, std::uint64_t epoch) {
  return width > 0U && height > 0U && width <= 16'384U && height <= 16'384U && epoch > 0U;
}

bool rectangle_inside(SaliencyRectangle rectangle, std::size_t width, std::size_t height) {
  std::size_t right = 0U;
  std::size_t bottom = 0U;
  return rectangle.width > 0U && rectangle.height > 0U &&
         checked_add(rectangle.x, rectangle.width, right) &&
         checked_add(rectangle.y, rectangle.height, bottom) && right <= width && bottom <= height;
}

bool intersects(SaliencyRectangle rectangle, std::size_t left, std::size_t top, std::size_t right,
                std::size_t bottom) {
  std::size_t rectangle_right = 0U;
  std::size_t rectangle_bottom = 0U;
  return checked_add(rectangle.x, rectangle.width, rectangle_right) &&
         checked_add(rectangle.y, rectangle.height, rectangle_bottom) && rectangle.x < right &&
         rectangle_right > left && rectangle.y < bottom && rectangle_bottom > top;
}

bool any_intersection(std::span<const SaliencyRectangle> rectangles, std::size_t left,
                      std::size_t top, std::size_t right, std::size_t bottom) {
  return std::any_of(rectangles.begin(), rectangles.end(), [&](SaliencyRectangle rectangle) {
    return intersects(rectangle, left, top, right, bottom);
  });
}

} // namespace

SaliencyCorrectionState::SaliencyCorrectionState(std::size_t visible_width,
                                                 std::size_t visible_height,
                                                 std::uint64_t geometry_epoch, std::size_t capacity)
    : visible_width_(visible_width), visible_height_(visible_height),
      geometry_epoch_(geometry_epoch), capacity_(capacity) {
  if (!valid_geometry(visible_width, visible_height, geometry_epoch) || capacity == 0U ||
      capacity > 1'024U) {
    throw std::invalid_argument("saliency_correction_state_invalid");
  }
  regions_.reserve(capacity);
}

SaliencyCorrectionOperation SaliencyCorrectionState::add(SaliencyCorrectionKind kind,
                                                         SaliencyRectangle rectangle,
                                                         std::uint64_t expected_revision) {
  if (expected_revision != revision_) {
    return {false, "saliency_correction_revision_stale", revision_, 0U};
  }
  if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
    return {false, "saliency_correction_revision_exhausted", revision_, 0U};
  }
  if (!rectangle_inside(rectangle, visible_width_, visible_height_)) {
    return {false, "saliency_correction_rectangle_invalid", revision_, 0U};
  }
  if (regions_.size() == capacity_) {
    return {false, "saliency_correction_capacity_reached", revision_, 0U};
  }
  if (next_id_ == 0U) {
    return {false, "saliency_correction_identity_exhausted", revision_, 0U};
  }
  const auto id = next_id_++;
  regions_.push_back({id, kind, rectangle});
  ++revision_;
  return {true, "saliency_correction_added", revision_, id};
}

SaliencyCorrectionOperation SaliencyCorrectionState::remove(std::uint64_t region_id,
                                                            std::uint64_t expected_revision) {
  if (expected_revision != revision_) {
    return {false, "saliency_correction_revision_stale", revision_, 0U};
  }
  if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
    return {false, "saliency_correction_revision_exhausted", revision_, 0U};
  }
  if (region_id == 0U) {
    return {false, "saliency_correction_identity_invalid", revision_, 0U};
  }
  const auto match = std::find_if(regions_.begin(), regions_.end(),
                                  [&](const auto &region) { return region.id == region_id; });
  if (match == regions_.end()) {
    return {false, "saliency_correction_not_found", revision_, 0U};
  }
  regions_.erase(match);
  ++revision_;
  return {true, "saliency_correction_removed", revision_, region_id};
}

SaliencyCorrectionOperation
SaliencyCorrectionState::reset_geometry(std::size_t visible_width, std::size_t visible_height,
                                        std::uint64_t geometry_epoch,
                                        std::uint64_t expected_revision) {
  if (expected_revision != revision_) {
    return {false, "saliency_correction_revision_stale", revision_, 0U};
  }
  if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
    return {false, "saliency_correction_revision_exhausted", revision_, 0U};
  }
  if (!valid_geometry(visible_width, visible_height, geometry_epoch) ||
      geometry_epoch <= geometry_epoch_) {
    return {false, "saliency_correction_geometry_invalid", revision_, 0U};
  }
  visible_width_ = visible_width;
  visible_height_ = visible_height;
  geometry_epoch_ = geometry_epoch;
  regions_.clear();
  ++revision_;
  return {true, "saliency_correction_geometry_reset", revision_, 0U};
}

SaliencyOverrides
SaliencyCorrectionState::overrides(std::span<const SaliencyRectangle> cursor_halos) const {
  SaliencyOverrides result;
  result.cursor_halos.assign(cursor_halos.begin(), cursor_halos.end());
  for (const auto &region : regions_) {
    if (region.kind == SaliencyCorrectionKind::pin) {
      result.pins.push_back(region.rectangle);
    } else {
      result.exclusions.push_back(region.rectangle);
    }
  }
  return result;
}

std::size_t SaliencyCorrectionState::visible_width() const { return visible_width_; }
std::size_t SaliencyCorrectionState::visible_height() const { return visible_height_; }
std::uint64_t SaliencyCorrectionState::geometry_epoch() const { return geometry_epoch_; }
std::uint64_t SaliencyCorrectionState::revision() const { return revision_; }
std::size_t SaliencyCorrectionState::capacity() const { return capacity_; }
const std::vector<SaliencyCorrectionRegion> &SaliencyCorrectionState::regions() const {
  return regions_;
}

std::vector<std::uint8_t> saliency_correction_conflict_tiles(std::size_t visible_width,
                                                             std::size_t visible_height,
                                                             const SaliencyOverrides &overrides) {
  if (visible_width == 0U || visible_height == 0U || visible_width > 16'384U ||
      visible_height > 16'384U) {
    throw std::invalid_argument("saliency_correction_preview_geometry_invalid");
  }
  const auto tile_width = (visible_width + kSaliencyTileSize - 1U) / kSaliencyTileSize;
  const auto tile_height = (visible_height + kSaliencyTileSize - 1U) / kSaliencyTileSize;
  std::vector<std::uint8_t> conflicts(tile_width * tile_height, 0U);
  for (std::size_t tile_y = 0U; tile_y < tile_height; ++tile_y) {
    for (std::size_t tile_x = 0U; tile_x < tile_width; ++tile_x) {
      const auto left = tile_x * kSaliencyTileSize;
      const auto top = tile_y * kSaliencyTileSize;
      const auto right = std::min(left + kSaliencyTileSize, visible_width);
      const auto bottom = std::min(top + kSaliencyTileSize, visible_height);
      const bool pinned = any_intersection(overrides.pins, left, top, right, bottom);
      const bool excluded = any_intersection(overrides.exclusions, left, top, right, bottom);
      conflicts[tile_y * tile_width + tile_x] = static_cast<std::uint8_t>(pinned && excluded);
    }
  }
  return conflicts;
}

void overlay_saliency_correction_conflicts(SaliencyPreview &preview,
                                           std::span<const std::uint8_t> conflict_tiles,
                                           std::size_t tile_width, std::size_t tile_height) {
  if (preview.width == 0U || preview.height == 0U || tile_width == 0U || tile_height == 0U ||
      tile_width != (preview.width + kSaliencyTileSize - 1U) / kSaliencyTileSize ||
      tile_height != (preview.height + kSaliencyTileSize - 1U) / kSaliencyTileSize ||
      conflict_tiles.size() != tile_width * tile_height ||
      preview.rgba.size() != preview.width * preview.height * 4U ||
      std::any_of(conflict_tiles.begin(), conflict_tiles.end(),
                  [](std::uint8_t value) { return value > 1U; })) {
    throw std::invalid_argument("saliency_correction_preview_invalid");
  }
  constexpr std::array<std::uint8_t, 4U> conflict_color = {224U, 48U, 255U, 208U};
  for (std::size_t y = 0U; y < preview.height; ++y) {
    for (std::size_t x = 0U; x < preview.width; ++x) {
      const auto tile = y / kSaliencyTileSize * tile_width + x / kSaliencyTileSize;
      if (conflict_tiles[tile] == 0U) {
        continue;
      }
      const auto offset = (y * preview.width + x) * 4U;
      std::copy(conflict_color.begin(), conflict_color.end(),
                preview.rgba.begin() + static_cast<std::ptrdiff_t>(offset));
    }
  }
}

} // namespace glyphrelay
