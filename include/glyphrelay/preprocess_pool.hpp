#pragma once

#include "glyphrelay/gpu_contracts.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace glyphrelay {

struct DeviceAllocationRange {
  std::uintptr_t begin = 0U;
  std::size_t byte_size = 0U;

  bool valid() const;
  friend bool operator==(const DeviceAllocationRange &, const DeviceAllocationRange &) = default;
};

struct PreprocessSlotToken {
  std::size_t source_slot = 0U;
  std::size_t surface_slot = 0U;
  std::uint64_t frame_id = 0U;
  std::uint64_t geometry_epoch = 0U;
};

struct PreprocessPoolOperation {
  bool passed = false;
  std::string reason;
  PreprocessSlotToken token;
};

struct PreprocessPoolDiagnostics {
  std::size_t source_capacity = 0U;
  std::size_t surface_capacity = 0U;
  std::size_t active_sources = 0U;
  std::size_t active_surfaces = 0U;
  std::uint64_t reservations = 0U;
  std::uint64_t rejected = 0U;
  bool admission_open = true;
};

class PreprocessOwnershipRing {
public:
  PreprocessOwnershipRing(std::size_t source_capacity, std::size_t surface_capacity);
  ~PreprocessOwnershipRing();
  PreprocessOwnershipRing(PreprocessOwnershipRing &&) noexcept;
  PreprocessOwnershipRing &operator=(PreprocessOwnershipRing &&) noexcept;
  PreprocessOwnershipRing(const PreprocessOwnershipRing &) = delete;
  PreprocessOwnershipRing &operator=(const PreprocessOwnershipRing &) = delete;

  PreprocessPoolOperation reserve(std::uint64_t frame_id, std::uint64_t geometry_epoch,
                                  DeviceAllocationRange packed_rgb_source,
                                  DeviceAllocationRange nv12_surface);
  PreprocessPoolOperation source_upload_complete(const PreprocessSlotToken &token);
  PreprocessPoolOperation source_read_complete(const PreprocessSlotToken &token);
  PreprocessPoolOperation map_copy_pending(const PreprocessSlotToken &token);
  PreprocessPoolOperation ready_to_submit(const PreprocessSlotToken &token);
  PreprocessPoolOperation submitted(const PreprocessSlotToken &token);
  PreprocessPoolOperation encoder_input_released(const PreprocessSlotToken &token);
  PreprocessPoolOperation release_surface(const PreprocessSlotToken &token);
  PreprocessPoolOperation abort(const PreprocessSlotToken &token);
  void close_admission();

  DeviceSourceState source_state(std::size_t slot) const;
  EncoderSurfaceState surface_state(std::size_t slot) const;
  PreprocessPoolDiagnostics diagnostics() const;
  bool all_free() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

bool device_ranges_overlap(DeviceAllocationRange left, DeviceAllocationRange right);

} // namespace glyphrelay
