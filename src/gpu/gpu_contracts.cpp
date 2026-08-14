#include "glyphrelay/gpu_contracts.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace glyphrelay {
namespace {

bool checked_product(std::size_t left, std::size_t right, std::size_t &result) {
  if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

struct SubmissionFingerprint {
  std::uint64_t sequence = 0;
  std::uintptr_t output_bitstream = 0;
  NvencFrameMode mode = NvencFrameMode::uniform;
  bool force_idr = false;
  std::uint64_t frame_id = 0;
  std::uint64_t geometry_epoch = 0;
  CudaContextIdentity context;
  std::uintptr_t surface_pointer = 0;
  std::size_t surface_width = 0U;
  std::size_t surface_height = 0U;
  std::size_t surface_pitch = 0U;
  std::size_t surface_allocation_bytes = 0U;
  std::uintptr_t map_pointer = 0;
  std::vector<std::int8_t> map_values;

  friend bool operator==(const SubmissionFingerprint &, const SubmissionFingerprint &) = default;
};

SubmissionFingerprint fingerprint(const NvencSubmissionRequest &request) {
  return {
      request.submission_sequence,
      request.output_bitstream,
      request.mode,
      request.force_idr,
      request.surface.frame_id,
      request.surface.geometry_epoch,
      request.surface.context,
      request.surface.device_pointer,
      request.surface.coded_width,
      request.surface.coded_height,
      request.surface.pitch,
      request.surface.allocation_bytes,
      request.emphasis_map.host_pointer,
      {request.emphasis_map.values.begin(), request.emphasis_map.values.end()},
  };
}

bool absent_emphasis_map(const EmphasisMapDescriptor &map) {
  return map.frame_id == 0U && map.geometry_epoch == 0U && !map.context.valid() &&
         map.memory_space == MemorySpace::unknown && map.host_pointer == 0U &&
         map.macroblock_width == 0U && map.macroblock_height == 0U && map.byte_size == 0U &&
         map.values.empty() && !map.device_to_host_ready;
}

} // namespace

struct NvencSubmissionCoordinator::Slot {
  SubmissionState state = SubmissionState::free;
  std::optional<SubmissionFingerprint> fingerprint;
  bool output_ready = false;
  std::size_t busy_retries = 0U;
};

bool CudaContextIdentity::valid() const {
  return device_ordinal >= 0 && context_handle != 0U && generation != 0U;
}

ContractValidation validate_nvenc_submission(const NvencSubmissionRequest &request) {
  const auto &surface = request.surface;
  const auto &map = request.emphasis_map;
  if (request.output_bitstream == 0U) {
    return {false, "nvenc_output_bitstream_missing"};
  }
  if (!surface.context.valid()) {
    return {false, "nvenc_foreign_cuda_context"};
  }
  if (surface.memory_space != MemorySpace::cuda_device || surface.device_pointer == 0U ||
      !surface.contiguous) {
    return {false, "nvenc_surface_not_contiguous_cuda_device_nv12"};
  }
  if (surface.coded_width == 0U || surface.coded_height == 0U || surface.coded_width > 16'384U ||
      surface.coded_height > 16'384U || (surface.coded_width & 1U) != 0U ||
      (surface.coded_height & 1U) != 0U || surface.pitch < surface.coded_width) {
    return {false, "nvenc_surface_geometry_or_pitch_invalid"};
  }
  std::size_t luma_bytes = 0;
  if (!checked_product(surface.pitch, surface.coded_height, luma_bytes) ||
      luma_bytes > std::numeric_limits<std::size_t>::max() - luma_bytes / 2U ||
      surface.allocation_bytes < luma_bytes + luma_bytes / 2U) {
    return {false, "nvenc_surface_allocation_too_small"};
  }
  if (!surface.cuda_ready) {
    return {false, "nvenc_surface_cuda_event_not_ready"};
  }
  if (request.mode == NvencFrameMode::uniform) {
    return absent_emphasis_map(map)
               ? ContractValidation{true, "nvenc_uniform_submission_contract_valid"}
               : ContractValidation{false, "nvenc_uniform_submission_has_emphasis_map"};
  }
  if (!map.context.valid() || surface.context != map.context) {
    return {false, "nvenc_foreign_cuda_context"};
  }
  if (surface.frame_id != map.frame_id) {
    return {false, "nvenc_stale_emphasis_map_frame"};
  }
  if (surface.geometry_epoch != map.geometry_epoch) {
    return {false, "nvenc_stale_emphasis_map_geometry"};
  }
  const auto expected_mb_width = (surface.coded_width + 15U) / 16U;
  const auto expected_mb_height = (surface.coded_height + 15U) / 16U;
  std::size_t expected_map_bytes = 0;
  if (!checked_product(expected_mb_width, expected_mb_height, expected_map_bytes)) {
    return {false, "nvenc_emphasis_map_geometry_overflow"};
  }
  if (map.macroblock_width != expected_mb_width || map.macroblock_height != expected_mb_height ||
      map.byte_size != expected_map_bytes || map.values.size() != expected_map_bytes) {
    return {false, "nvenc_emphasis_map_size_mismatch"};
  }
  if (map.memory_space != MemorySpace::host_pinned || map.host_pointer == 0U) {
    return {false, "nvenc_emphasis_map_not_pinned_host_memory"};
  }
  if (reinterpret_cast<std::uintptr_t>(map.values.data()) != map.host_pointer) {
    return {false, "nvenc_emphasis_map_pointer_mismatch"};
  }
  if (!map.device_to_host_ready) {
    return {false, "nvenc_emphasis_map_event_not_ready"};
  }
  if (std::any_of(map.values.begin(), map.values.end(),
                  [](std::int8_t level) { return level < 0 || level > 5; })) {
    return {false, "nvenc_emphasis_level_out_of_range"};
  }
  return {true, "nvenc_emphasis_submission_contract_valid"};
}

bool valid_device_source_transition(DeviceSourceState from, DeviceSourceState to) {
  return (from == DeviceSourceState::free && to == DeviceSourceState::host_to_device_pending) ||
         (from == DeviceSourceState::host_to_device_pending &&
          to == DeviceSourceState::cuda_source_read_pending) ||
         (from == DeviceSourceState::cuda_source_read_pending && to == DeviceSourceState::free);
}

bool valid_imported_source_transition(ImportedSourceState from, ImportedSourceState to) {
  return (from == ImportedSourceState::pipewire_owned &&
          to == ImportedSourceState::cuda_source_read_pending) ||
         (from == ImportedSourceState::cuda_source_read_pending &&
          to == ImportedSourceState::pipewire_requeue_pending) ||
         (from == ImportedSourceState::pipewire_requeue_pending &&
          to == ImportedSourceState::released);
}

bool valid_encoder_surface_transition(EncoderSurfaceState from, EncoderSurfaceState to) {
  return (from == EncoderSurfaceState::free && to == EncoderSurfaceState::cuda_writing) ||
         (from == EncoderSurfaceState::cuda_writing &&
          to == EncoderSurfaceState::map_copy_pending) ||
         (from == EncoderSurfaceState::map_copy_pending &&
          to == EncoderSurfaceState::ready_to_submit) ||
         (from == EncoderSurfaceState::ready_to_submit && to == EncoderSurfaceState::submitted) ||
         (from == EncoderSurfaceState::submitted &&
          to == EncoderSurfaceState::encoder_input_released) ||
         (from == EncoderSurfaceState::encoder_input_released && to == EncoderSurfaceState::free);
}

NvencSubmissionCoordinator::NvencSubmissionCoordinator(std::size_t capacity,
                                                       std::size_t maximum_busy_retries)
    : slots_(capacity), maximum_busy_retries_(maximum_busy_retries) {
  if (capacity == 0U || capacity > 64U || maximum_busy_retries == 0U ||
      maximum_busy_retries > 10'000U) {
    throw std::invalid_argument("NVENC submission capacity or busy retry bound is invalid");
  }
}

NvencSubmissionCoordinator::~NvencSubmissionCoordinator() = default;

SubmissionOperation
NvencSubmissionCoordinator::submit(const NvencSubmissionRequest &request,
                                   const std::function<NvencSubmitStatus()> &driver_submit) {
  if (fatal_) {
    return {false, false, "nvenc_coordinator_fatal", SubmissionState::abort_pending};
  }
  if (end_of_stream_) {
    return {false, false, "nvenc_submission_after_eos", SubmissionState::free};
  }
  if (request.submission_slot_id >= slots_.size()) {
    return {false, false, "nvenc_submission_slot_out_of_range", SubmissionState::free};
  }
  const auto validation = validate_nvenc_submission(request);
  if (!validation.passed) {
    return {false, false, validation.reason, state(request.submission_slot_id)};
  }
  auto &slot = slots_[request.submission_slot_id];
  const auto request_fingerprint = fingerprint(request);
  if (slot.state == SubmissionState::free) {
    if (last_sequence_ && request.submission_sequence <= *last_sequence_) {
      return {false, false, "nvenc_submission_sequence_not_monotonic", slot.state};
    }
    const bool output_in_use =
        std::any_of(slots_.begin(), slots_.end(), [&request](const Slot &candidate) {
          return candidate.state != SubmissionState::free && candidate.fingerprint &&
                 candidate.fingerprint->output_bitstream == request.output_bitstream;
        });
    if (output_in_use) {
      return {false, false, "nvenc_output_bitstream_already_owned", slot.state};
    }
    slot.state = SubmissionState::reserved;
    slot.fingerprint = request_fingerprint;
    last_sequence_ = request.submission_sequence;
  } else if (slot.state == SubmissionState::submit_retry_pending) {
    if (!slot.fingerprint || *slot.fingerprint != request_fingerprint) {
      return {false, false, "nvenc_busy_retry_mutated", slot.state};
    }
    slot.state = SubmissionState::reserved;
  } else {
    return {false, false, "nvenc_submission_slot_not_reservable", slot.state};
  }
  if (!driver_submit) {
    enter_fatal_state();
    return {false, false, "nvenc_driver_callback_missing", slot.state};
  }

  ++driver_call_count_;
  NvencSubmitStatus driver_status = NvencSubmitStatus::fatal;
  try {
    driver_status = driver_submit();
  } catch (...) {
    enter_fatal_state();
    return {false, true, "nvenc_driver_callback_threw", slot.state};
  }
  switch (driver_status) {
  case NvencSubmitStatus::encoder_busy:
    ++slot.busy_retries;
    if (slot.busy_retries > maximum_busy_retries_) {
      enter_fatal_state();
      return {false, true, "nvenc_busy_retry_limit_exceeded", slot.state};
    }
    slot.state = SubmissionState::submit_retry_pending;
    return {true, true, "nvenc_submission_retry_pending", slot.state};
  case NvencSubmitStatus::fatal:
    enter_fatal_state();
    return {false, true, "nvenc_submission_fatal", slot.state};
  case NvencSubmitStatus::need_more_input: {
    slot.state = SubmissionState::submitted_pending_output;
    slot.output_ready = false;
    fifo_.push_back(request.submission_slot_id);
    make_fifo_head_lockable();
    return {true, true, "nvenc_submission_needs_more_input", slot.state};
  }
  case NvencSubmitStatus::success:
    for (const auto slot_id : fifo_) {
      slots_[slot_id].output_ready = true;
    }
    slot.state = SubmissionState::submitted_pending_output;
    slot.output_ready = true;
    fifo_.push_back(request.submission_slot_id);
    make_fifo_head_lockable();
    return {true, true, "nvenc_submission_accepted", slot.state};
  }
  enter_fatal_state();
  return {false, true, "nvenc_submission_status_unknown", slot.state};
}

SubmissionOperation NvencSubmissionCoordinator::begin_end_of_stream() {
  if (fatal_) {
    return {false, false, "nvenc_eos_after_fatal", SubmissionState::abort_pending};
  }
  if (end_of_stream_) {
    return {false, false, "nvenc_eos_already_started", SubmissionState::free};
  }
  end_of_stream_ = true;
  for (const auto slot_id : fifo_) {
    slots_[slot_id].output_ready = true;
  }
  make_fifo_head_lockable();
  return {true, false, "nvenc_eos_draining",
          fifo_.empty() ? SubmissionState::free : slots_[fifo_.front()].state};
}

SubmissionOperation NvencSubmissionCoordinator::fail() {
  if (fatal_) {
    return {true, false, "nvenc_coordinator_already_fatal", SubmissionState::abort_pending};
  }
  enter_fatal_state();
  return {true, false, "nvenc_coordinator_entered_fatal", SubmissionState::abort_pending};
}

std::optional<std::size_t> NvencSubmissionCoordinator::begin_bitstream_lock() {
  if (fifo_.empty()) {
    return std::nullopt;
  }
  auto &head = slots_[fifo_.front()];
  if (head.state != SubmissionState::bitstream_lockable) {
    return std::nullopt;
  }
  head.state = SubmissionState::bitstream_locked;
  return fifo_.front();
}

SubmissionOperation NvencSubmissionCoordinator::complete_bitstream(std::size_t submission_slot_id) {
  if (fifo_.empty() || fifo_.front() != submission_slot_id ||
      slots_[submission_slot_id].state != SubmissionState::bitstream_locked) {
    return {false, false, "nvenc_bitstream_completion_not_fifo_head", state(submission_slot_id)};
  }
  auto &slot = slots_[submission_slot_id];
  slot = {};
  fifo_.pop_front();
  if (!fatal_) {
    make_fifo_head_lockable();
  }
  return {true, false, "nvenc_bitstream_slot_released", SubmissionState::free};
}

SubmissionOperation NvencSubmissionCoordinator::confirm_abort(std::size_t submission_slot_id) {
  if (submission_slot_id >= slots_.size() ||
      slots_[submission_slot_id].state != SubmissionState::abort_pending) {
    return {false, false, "nvenc_abort_confirmation_invalid", state(submission_slot_id)};
  }
  slots_[submission_slot_id] = {};
  return {true, false, "nvenc_abort_slot_released", SubmissionState::free};
}

SubmissionState NvencSubmissionCoordinator::state(std::size_t submission_slot_id) const {
  return submission_slot_id < slots_.size() ? slots_[submission_slot_id].state
                                            : SubmissionState::free;
}

std::vector<std::size_t> NvencSubmissionCoordinator::pending_fifo() const {
  return {fifo_.begin(), fifo_.end()};
}

std::size_t NvencSubmissionCoordinator::capacity() const { return slots_.size(); }

std::size_t NvencSubmissionCoordinator::active_slots() const {
  return static_cast<std::size_t>(std::count_if(slots_.begin(), slots_.end(), [](const Slot &slot) {
    return slot.state != SubmissionState::free;
  }));
}

std::uint64_t NvencSubmissionCoordinator::driver_call_count() const { return driver_call_count_; }

bool NvencSubmissionCoordinator::fatal() const { return fatal_; }

void NvencSubmissionCoordinator::make_fifo_head_lockable() {
  if (fifo_.empty()) {
    return;
  }
  auto &head = slots_[fifo_.front()];
  if (head.state == SubmissionState::submitted_pending_output &&
      (head.output_ready || end_of_stream_)) {
    head.state = SubmissionState::bitstream_lockable;
  }
}

void NvencSubmissionCoordinator::enter_fatal_state() {
  fatal_ = true;
  const auto locked_head =
      !fifo_.empty() && slots_[fifo_.front()].state == SubmissionState::bitstream_locked
          ? std::optional<std::size_t>(fifo_.front())
          : std::nullopt;
  fifo_.clear();
  for (std::size_t slot_id = 0U; slot_id < slots_.size(); ++slot_id) {
    auto &slot = slots_[slot_id];
    if (slot.state != SubmissionState::free && slot_id != locked_head) {
      slot.state = SubmissionState::abort_pending;
    }
  }
  if (locked_head) {
    fifo_.push_back(*locked_head);
  }
}

ContractValidation CudaShutdownContract::advance(CudaShutdownPhase next) {
  const auto expected = static_cast<unsigned int>(phase_) + 1U;
  if (static_cast<unsigned int>(next) != expected) {
    return {false, "cuda_shutdown_phase_out_of_order"};
  }
  phase_ = next;
  return {true, "cuda_shutdown_phase_advanced"};
}

CudaShutdownPhase CudaShutdownContract::phase() const { return phase_; }

bool CudaShutdownContract::complete() const {
  return phase_ == CudaShutdownPhase::primary_context_released;
}

std::string memory_space_name(MemorySpace memory_space) {
  switch (memory_space) {
  case MemorySpace::unknown:
    return "unknown";
  case MemorySpace::host_pageable:
    return "host_pageable";
  case MemorySpace::host_pinned:
    return "host_pinned";
  case MemorySpace::cuda_device:
    return "cuda_device";
  case MemorySpace::imported_dmabuf:
    return "imported_dmabuf";
  }
  return "unknown";
}

std::string nvenc_frame_mode_name(NvencFrameMode mode) {
  switch (mode) {
  case NvencFrameMode::uniform:
    return "uniform";
  case NvencFrameMode::fixed_emphasis:
    return "fixed_emphasis";
  case NvencFrameMode::automatic_emphasis:
    return "automatic_emphasis";
  }
  return "unknown";
}

std::string submission_state_name(SubmissionState state) {
  switch (state) {
  case SubmissionState::free:
    return "free";
  case SubmissionState::reserved:
    return "reserved";
  case SubmissionState::submit_retry_pending:
    return "submit_retry_pending";
  case SubmissionState::submitted_pending_output:
    return "submitted_pending_output";
  case SubmissionState::bitstream_lockable:
    return "bitstream_lockable";
  case SubmissionState::bitstream_locked:
    return "bitstream_locked";
  case SubmissionState::abort_pending:
    return "abort_pending";
  }
  return "unknown";
}

std::string cuda_shutdown_phase_name(CudaShutdownPhase phase) {
  switch (phase) {
  case CudaShutdownPhase::active:
    return "active";
  case CudaShutdownPhase::admission_closed:
    return "admission_closed";
  case CudaShutdownPhase::producers_joined:
    return "producers_joined";
  case CudaShutdownPhase::events_resolved:
    return "events_resolved";
  case CudaShutdownPhase::nvenc_drained:
    return "nvenc_drained";
  case CudaShutdownPhase::resources_unregistered:
    return "resources_unregistered";
  case CudaShutdownPhase::streams_destroyed:
    return "streams_destroyed";
  case CudaShutdownPhase::context_workers_joined:
    return "context_workers_joined";
  case CudaShutdownPhase::primary_context_released:
    return "primary_context_released";
  }
  return "unknown";
}

} // namespace glyphrelay
