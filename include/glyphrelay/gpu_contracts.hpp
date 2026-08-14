#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace glyphrelay {

struct CudaContextIdentity {
  int device_ordinal = -1;
  std::uintptr_t context_handle = 0;
  std::uint64_t generation = 0;

  bool valid() const;
  friend bool operator==(const CudaContextIdentity &, const CudaContextIdentity &) = default;
};

enum class MemorySpace {
  unknown,
  host_pageable,
  host_pinned,
  cuda_device,
  imported_dmabuf,
};

struct Nv12SurfaceDescriptor {
  std::uint64_t frame_id = 0;
  std::uint64_t geometry_epoch = 0;
  CudaContextIdentity context;
  MemorySpace memory_space = MemorySpace::unknown;
  std::uintptr_t device_pointer = 0;
  std::size_t coded_width = 0;
  std::size_t coded_height = 0;
  std::size_t pitch = 0;
  std::size_t allocation_bytes = 0;
  bool contiguous = false;
  bool cuda_ready = false;
};

struct EmphasisMapDescriptor {
  std::uint64_t frame_id = 0;
  std::uint64_t geometry_epoch = 0;
  CudaContextIdentity context;
  MemorySpace memory_space = MemorySpace::unknown;
  std::uintptr_t host_pointer = 0;
  std::size_t macroblock_width = 0;
  std::size_t macroblock_height = 0;
  std::size_t byte_size = 0;
  std::span<const std::int8_t> values;
  bool device_to_host_ready = false;
};

enum class NvencFrameMode {
  uniform,
  fixed_emphasis,
  automatic_emphasis,
};

struct NvencSubmissionRequest {
  std::size_t submission_slot_id = 0;
  std::uint64_t submission_sequence = 0;
  std::uintptr_t output_bitstream = 0;
  NvencFrameMode mode = NvencFrameMode::automatic_emphasis;
  bool force_idr = false;
  Nv12SurfaceDescriptor surface;
  EmphasisMapDescriptor emphasis_map;
};

struct ContractValidation {
  bool passed = false;
  std::string reason;
};

ContractValidation validate_nvenc_submission(const NvencSubmissionRequest &request);

enum class DeviceSourceState {
  free,
  host_to_device_pending,
  cuda_source_read_pending,
};

enum class ImportedSourceState {
  pipewire_owned,
  cuda_source_read_pending,
  pipewire_requeue_pending,
  released,
};

enum class EncoderSurfaceState {
  free,
  cuda_writing,
  map_copy_pending,
  ready_to_submit,
  submitted,
  encoder_input_released,
};

bool valid_device_source_transition(DeviceSourceState from, DeviceSourceState to);
bool valid_imported_source_transition(ImportedSourceState from, ImportedSourceState to);
bool valid_encoder_surface_transition(EncoderSurfaceState from, EncoderSurfaceState to);

enum class NvencSubmitStatus {
  success,
  need_more_input,
  encoder_busy,
  fatal,
};

enum class SubmissionState {
  free,
  reserved,
  submit_retry_pending,
  submitted_pending_output,
  bitstream_lockable,
  bitstream_locked,
  abort_pending,
};

struct SubmissionOperation {
  bool passed = false;
  bool driver_invoked = false;
  std::string reason;
  SubmissionState state = SubmissionState::free;
};

class NvencSubmissionCoordinator {
public:
  explicit NvencSubmissionCoordinator(std::size_t capacity,
                                      std::size_t maximum_busy_retries = 100U);
  ~NvencSubmissionCoordinator();

  NvencSubmissionCoordinator(const NvencSubmissionCoordinator &) = delete;
  NvencSubmissionCoordinator &operator=(const NvencSubmissionCoordinator &) = delete;
  NvencSubmissionCoordinator(NvencSubmissionCoordinator &&) noexcept = delete;
  NvencSubmissionCoordinator &operator=(NvencSubmissionCoordinator &&) noexcept = delete;

  SubmissionOperation submit(const NvencSubmissionRequest &request,
                             const std::function<NvencSubmitStatus()> &driver_submit);
  SubmissionOperation fail();
  SubmissionOperation begin_end_of_stream();
  std::optional<std::size_t> begin_bitstream_lock();
  SubmissionOperation complete_bitstream(std::size_t submission_slot_id);
  SubmissionOperation confirm_abort(std::size_t submission_slot_id);

  SubmissionState state(std::size_t submission_slot_id) const;
  std::vector<std::size_t> pending_fifo() const;
  std::size_t capacity() const;
  std::size_t active_slots() const;
  std::uint64_t driver_call_count() const;
  bool fatal() const;

private:
  struct Slot;

  void make_fifo_head_lockable();
  void enter_fatal_state();

  std::vector<Slot> slots_;
  std::deque<std::size_t> fifo_;
  std::optional<std::uint64_t> last_sequence_;
  std::uint64_t driver_call_count_ = 0;
  std::size_t maximum_busy_retries_ = 0U;
  bool end_of_stream_ = false;
  bool fatal_ = false;
};

enum class CudaShutdownPhase {
  active,
  admission_closed,
  producers_joined,
  events_resolved,
  nvenc_drained,
  resources_unregistered,
  streams_destroyed,
  context_workers_joined,
  primary_context_released,
};

class CudaShutdownContract {
public:
  ContractValidation advance(CudaShutdownPhase next);
  CudaShutdownPhase phase() const;
  bool complete() const;

private:
  CudaShutdownPhase phase_ = CudaShutdownPhase::active;
};

std::string memory_space_name(MemorySpace memory_space);
std::string nvenc_frame_mode_name(NvencFrameMode mode);
std::string submission_state_name(SubmissionState state);
std::string cuda_shutdown_phase_name(CudaShutdownPhase phase);

} // namespace glyphrelay
