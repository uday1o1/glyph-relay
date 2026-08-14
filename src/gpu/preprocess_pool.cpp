#include "glyphrelay/preprocess_pool.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace glyphrelay {
namespace {

struct SlotOwner {
  std::uint64_t frame_id = 0U;
  std::uint64_t geometry_epoch = 0U;

  bool matches(const PreprocessSlotToken &token) const {
    return frame_id == token.frame_id && geometry_epoch == token.geometry_epoch;
  }
};

template <typename Slot>
std::optional<std::size_t> compatible_free_slot(const std::vector<Slot> &slots,
                                                DeviceAllocationRange allocation) {
  for (std::size_t index = 0U; index < slots.size(); ++index) {
    if (slots[index].free() &&
        (!slots[index].allocation.has_value() || *slots[index].allocation == allocation)) {
      return index;
    }
  }
  return std::nullopt;
}

} // namespace

struct PreprocessOwnershipRing::Implementation {
  struct SourceSlot {
    DeviceSourceState state = DeviceSourceState::free;
    std::optional<DeviceAllocationRange> allocation;
    SlotOwner owner;

    bool free() const { return state == DeviceSourceState::free; }
  };

  struct SurfaceSlot {
    EncoderSurfaceState state = EncoderSurfaceState::free;
    std::optional<DeviceAllocationRange> allocation;
    SlotOwner owner;

    bool free() const { return state == EncoderSurfaceState::free; }
  };

  Implementation(std::size_t source_capacity, std::size_t surface_capacity)
      : sources(source_capacity), surfaces(surface_capacity) {}

  std::vector<SourceSlot> sources;
  std::vector<SurfaceSlot> surfaces;
  std::uint64_t reservations = 0U;
  std::uint64_t rejected = 0U;
  bool admission_open = true;
};

bool DeviceAllocationRange::valid() const {
  return begin != 0U && byte_size != 0U &&
         byte_size <= std::numeric_limits<std::uintptr_t>::max() - begin;
}

bool device_ranges_overlap(DeviceAllocationRange left, DeviceAllocationRange right) {
  if (!left.valid() || !right.valid()) {
    return true;
  }
  return left.begin < right.begin + right.byte_size && right.begin < left.begin + left.byte_size;
}

PreprocessOwnershipRing::PreprocessOwnershipRing(std::size_t source_capacity,
                                                 std::size_t surface_capacity) {
  if (source_capacity == 0U || source_capacity > 64U || surface_capacity == 0U ||
      surface_capacity > 64U) {
    throw std::invalid_argument("preprocess pool capacity is invalid");
  }
  implementation_ = std::make_unique<Implementation>(source_capacity, surface_capacity);
}

PreprocessOwnershipRing::~PreprocessOwnershipRing() = default;
PreprocessOwnershipRing::PreprocessOwnershipRing(PreprocessOwnershipRing &&) noexcept = default;
PreprocessOwnershipRing &
PreprocessOwnershipRing::operator=(PreprocessOwnershipRing &&) noexcept = default;

PreprocessPoolOperation PreprocessOwnershipRing::reserve(std::uint64_t frame_id,
                                                         std::uint64_t geometry_epoch,
                                                         DeviceAllocationRange packed_rgb_source,
                                                         DeviceAllocationRange nv12_surface) {
  auto reject = [&](std::string reason) {
    ++implementation_->rejected;
    return PreprocessPoolOperation{false, std::move(reason), {}};
  };
  if (!implementation_->admission_open) {
    return reject("preprocess_admission_closed");
  }
  if (frame_id == 0U || geometry_epoch == 0U) {
    return reject("preprocess_identity_invalid");
  }
  if (!packed_rgb_source.valid() || !nv12_surface.valid()) {
    return reject("preprocess_device_allocation_invalid");
  }
  if (device_ranges_overlap(packed_rgb_source, nv12_surface)) {
    return reject("preprocess_source_surface_alias");
  }

  for (const auto &slot : implementation_->sources) {
    if (slot.allocation && device_ranges_overlap(*slot.allocation, nv12_surface)) {
      return reject("preprocess_source_surface_alias");
    }
  }
  for (const auto &slot : implementation_->surfaces) {
    if (slot.allocation && device_ranges_overlap(*slot.allocation, packed_rgb_source)) {
      return reject("preprocess_source_surface_alias");
    }
  }

  const auto source_slot = compatible_free_slot(implementation_->sources, packed_rgb_source);
  const auto surface_slot = compatible_free_slot(implementation_->surfaces, nv12_surface);
  if (!source_slot || !surface_slot) {
    return reject("preprocess_pool_exhausted_or_allocation_unregistered");
  }

  for (std::size_t index = 0U; index < implementation_->sources.size(); ++index) {
    const auto &slot = implementation_->sources[index];
    if (slot.allocation && index != *source_slot &&
        device_ranges_overlap(*slot.allocation, packed_rgb_source)) {
      return reject("preprocess_source_allocation_alias");
    }
  }
  for (std::size_t index = 0U; index < implementation_->surfaces.size(); ++index) {
    const auto &slot = implementation_->surfaces[index];
    if (slot.allocation && index != *surface_slot &&
        device_ranges_overlap(*slot.allocation, nv12_surface)) {
      return reject("preprocess_surface_allocation_alias");
    }
  }

  auto &source = implementation_->sources[*source_slot];
  auto &surface = implementation_->surfaces[*surface_slot];
  source.allocation = packed_rgb_source;
  source.owner = {frame_id, geometry_epoch};
  source.state = DeviceSourceState::host_to_device_pending;
  surface.allocation = nv12_surface;
  surface.owner = {frame_id, geometry_epoch};
  surface.state = EncoderSurfaceState::cuda_writing;
  ++implementation_->reservations;
  return {true,
          "preprocess_slots_reserved",
          {.source_slot = *source_slot,
           .surface_slot = *surface_slot,
           .frame_id = frame_id,
           .geometry_epoch = geometry_epoch}};
}

PreprocessPoolOperation
PreprocessOwnershipRing::source_upload_complete(const PreprocessSlotToken &token) {
  if (token.source_slot >= implementation_->sources.size()) {
    return {false, "preprocess_source_slot_out_of_range", token};
  }
  auto &slot = implementation_->sources[token.source_slot];
  if (!slot.owner.matches(token) || slot.state != DeviceSourceState::host_to_device_pending) {
    return {false, "preprocess_source_upload_transition_invalid", token};
  }
  slot.state = DeviceSourceState::cuda_source_read_pending;
  return {true, "preprocess_source_read_pending", token};
}

PreprocessPoolOperation
PreprocessOwnershipRing::source_read_complete(const PreprocessSlotToken &token) {
  if (token.source_slot >= implementation_->sources.size()) {
    return {false, "preprocess_source_slot_out_of_range", token};
  }
  auto &slot = implementation_->sources[token.source_slot];
  if (!slot.owner.matches(token) || slot.state != DeviceSourceState::cuda_source_read_pending) {
    return {false, "preprocess_source_read_completion_invalid", token};
  }
  slot.state = DeviceSourceState::free;
  slot.owner = {};
  return {true, "preprocess_source_released", token};
}

PreprocessPoolOperation
PreprocessOwnershipRing::map_copy_pending(const PreprocessSlotToken &token) {
  if (token.surface_slot >= implementation_->surfaces.size()) {
    return {false, "preprocess_surface_slot_out_of_range", token};
  }
  auto &slot = implementation_->surfaces[token.surface_slot];
  if (!slot.owner.matches(token) || slot.state != EncoderSurfaceState::cuda_writing) {
    return {false, "preprocess_map_copy_transition_invalid", token};
  }
  slot.state = EncoderSurfaceState::map_copy_pending;
  return {true, "preprocess_map_copy_pending", token};
}

PreprocessPoolOperation PreprocessOwnershipRing::ready_to_submit(const PreprocessSlotToken &token) {
  if (token.surface_slot >= implementation_->surfaces.size()) {
    return {false, "preprocess_surface_slot_out_of_range", token};
  }
  auto &slot = implementation_->surfaces[token.surface_slot];
  if (!slot.owner.matches(token) || slot.state != EncoderSurfaceState::map_copy_pending) {
    return {false, "preprocess_ready_transition_invalid", token};
  }
  slot.state = EncoderSurfaceState::ready_to_submit;
  return {true, "preprocess_ready_to_submit", token};
}

PreprocessPoolOperation PreprocessOwnershipRing::submitted(const PreprocessSlotToken &token) {
  if (token.surface_slot >= implementation_->surfaces.size()) {
    return {false, "preprocess_surface_slot_out_of_range", token};
  }
  auto &slot = implementation_->surfaces[token.surface_slot];
  if (!slot.owner.matches(token) || slot.state != EncoderSurfaceState::ready_to_submit) {
    return {false, "preprocess_submit_transition_invalid", token};
  }
  slot.state = EncoderSurfaceState::submitted;
  return {true, "preprocess_surface_submitted", token};
}

PreprocessPoolOperation
PreprocessOwnershipRing::encoder_input_released(const PreprocessSlotToken &token) {
  if (token.surface_slot >= implementation_->surfaces.size()) {
    return {false, "preprocess_surface_slot_out_of_range", token};
  }
  auto &slot = implementation_->surfaces[token.surface_slot];
  if (!slot.owner.matches(token) || slot.state != EncoderSurfaceState::submitted) {
    return {false, "preprocess_encoder_release_transition_invalid", token};
  }
  slot.state = EncoderSurfaceState::encoder_input_released;
  return {true, "preprocess_encoder_input_released", token};
}

PreprocessPoolOperation PreprocessOwnershipRing::release_surface(const PreprocessSlotToken &token) {
  if (token.surface_slot >= implementation_->surfaces.size()) {
    return {false, "preprocess_surface_slot_out_of_range", token};
  }
  auto &slot = implementation_->surfaces[token.surface_slot];
  if (!slot.owner.matches(token) || slot.state != EncoderSurfaceState::encoder_input_released) {
    return {false, "preprocess_surface_release_transition_invalid", token};
  }
  slot.state = EncoderSurfaceState::free;
  slot.owner = {};
  return {true, "preprocess_surface_released", token};
}

PreprocessPoolOperation PreprocessOwnershipRing::abort(const PreprocessSlotToken &token) {
  if (token.source_slot >= implementation_->sources.size() ||
      token.surface_slot >= implementation_->surfaces.size()) {
    return {false, "preprocess_abort_slot_out_of_range", token};
  }
  auto &source = implementation_->sources[token.source_slot];
  auto &surface = implementation_->surfaces[token.surface_slot];
  const bool source_owned = !source.free() && source.owner.matches(token);
  const bool surface_owned = !surface.free() && surface.owner.matches(token);
  if (!source_owned && !surface_owned) {
    return {false, "preprocess_abort_owner_mismatch", token};
  }
  if (source_owned) {
    source.state = DeviceSourceState::free;
    source.owner = {};
  }
  if (surface_owned) {
    surface.state = EncoderSurfaceState::free;
    surface.owner = {};
  }
  return {true, "preprocess_slots_aborted", token};
}

void PreprocessOwnershipRing::close_admission() { implementation_->admission_open = false; }

DeviceSourceState PreprocessOwnershipRing::source_state(std::size_t slot) const {
  return slot < implementation_->sources.size() ? implementation_->sources[slot].state
                                                : DeviceSourceState::free;
}

EncoderSurfaceState PreprocessOwnershipRing::surface_state(std::size_t slot) const {
  return slot < implementation_->surfaces.size() ? implementation_->surfaces[slot].state
                                                 : EncoderSurfaceState::free;
}

PreprocessPoolDiagnostics PreprocessOwnershipRing::diagnostics() const {
  return {
      .source_capacity = implementation_->sources.size(),
      .surface_capacity = implementation_->surfaces.size(),
      .active_sources = static_cast<std::size_t>(
          std::count_if(implementation_->sources.begin(), implementation_->sources.end(),
                        [](const auto &slot) { return !slot.free(); })),
      .active_surfaces = static_cast<std::size_t>(
          std::count_if(implementation_->surfaces.begin(), implementation_->surfaces.end(),
                        [](const auto &slot) { return !slot.free(); })),
      .reservations = implementation_->reservations,
      .rejected = implementation_->rejected,
      .admission_open = implementation_->admission_open,
  };
}

bool PreprocessOwnershipRing::all_free() const {
  const auto state = diagnostics();
  return state.active_sources == 0U && state.active_surfaces == 0U;
}

} // namespace glyphrelay
