#include "glyphrelay/cuda_context.hpp"
#include "glyphrelay/gpu_contracts.hpp"
#include "glyphrelay/nvenc_probe.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

struct RequestFixture {
  std::vector<std::int8_t> map = std::vector<std::int8_t>(120U * 68U, 0);

  glyphrelay::NvencSubmissionRequest request(std::size_t slot_id, std::uint64_t sequence,
                                             std::uintptr_t output) {
    const glyphrelay::CudaContextIdentity context{0, 0xC0DAU, 7U};
    return {
        .submission_slot_id = slot_id,
        .submission_sequence = sequence,
        .output_bitstream = output,
        .mode = glyphrelay::NvencFrameMode::automatic_emphasis,
        .force_idr = sequence == 1U,
        .surface =
            {
                .frame_id = sequence + 100U,
                .geometry_epoch = 4U,
                .context = context,
                .memory_space = glyphrelay::MemorySpace::cuda_device,
                .device_pointer = 0x100000U + sequence * 0x1000U,
                .coded_width = 1920U,
                .coded_height = 1088U,
                .pitch = 2048U,
                .allocation_bytes = 2048U * 1088U * 3U / 2U,
                .contiguous = true,
                .cuda_ready = true,
            },
        .emphasis_map =
            {
                .frame_id = sequence + 100U,
                .geometry_epoch = 4U,
                .context = context,
                .memory_space = glyphrelay::MemorySpace::host_pinned,
                .host_pointer = reinterpret_cast<std::uintptr_t>(map.data()),
                .macroblock_width = 120U,
                .macroblock_height = 68U,
                .byte_size = map.size(),
                .values = map,
                .device_to_host_ready = true,
            },
    };
  }
};

void test_pre_submit_validation() {
  RequestFixture fixture;
  const auto valid = fixture.request(0U, 1U, 0xB100U);
  require(glyphrelay::validate_nvenc_submission(valid).passed,
          "the complete NVENC request contract must pass");

  auto uniform = valid;
  uniform.mode = glyphrelay::NvencFrameMode::uniform;
  uniform.emphasis_map = {};
  require(glyphrelay::validate_nvenc_submission(uniform).passed,
          "uniform NVENC must be representable without a fabricated emphasis map");
  uniform.emphasis_map = valid.emphasis_map;
  require(glyphrelay::validate_nvenc_submission(uniform).reason ==
              "nvenc_uniform_submission_has_emphasis_map",
          "uniform NVENC must reject a silently retained map");

  auto reject = [&valid](auto mutate, const char *reason) {
    auto request = valid;
    mutate(request);
    const auto result = glyphrelay::validate_nvenc_submission(request);
    require(!result.passed && result.reason == reason,
            "the seeded NVENC request defect must fail for its intended reason");
  };
  reject([](auto &request) { --request.emphasis_map.byte_size; },
         "nvenc_emphasis_map_size_mismatch");
  reject([](auto &request) { ++request.emphasis_map.frame_id; }, "nvenc_stale_emphasis_map_frame");
  reject([](auto &request) { ++request.emphasis_map.geometry_epoch; },
         "nvenc_stale_emphasis_map_geometry");
  reject([](auto &request) { ++request.emphasis_map.context.generation; },
         "nvenc_foreign_cuda_context");
  reject(
      [](auto &request) {
        request.emphasis_map.memory_space = glyphrelay::MemorySpace::cuda_device;
      },
      "nvenc_emphasis_map_not_pinned_host_memory");
  reject([](auto &request) { request.emphasis_map.host_pointer += 1U; },
         "nvenc_emphasis_map_pointer_mismatch");
  reject([](auto &request) { request.emphasis_map.device_to_host_ready = false; },
         "nvenc_emphasis_map_event_not_ready");
  reject([](auto &request) { request.surface.cuda_ready = false; },
         "nvenc_surface_cuda_event_not_ready");
  reject([](auto &request) { request.surface.allocation_bytes = 1U; },
         "nvenc_surface_allocation_too_small");
  fixture.map.back() = 6;
  require(glyphrelay::validate_nvenc_submission(valid).reason ==
              "nvenc_emphasis_level_out_of_range",
          "an out-of-range emphasis level must fail before submission");
  fixture.map.back() = 0;

  std::mt19937_64 random(0x6d305f6e76656e63ULL);
  for (std::size_t iteration = 0U; iteration < 1000U; ++iteration) {
    auto request = valid;
    request.emphasis_map.byte_size = static_cast<std::size_t>(random() % fixture.map.size());
    if (request.emphasis_map.byte_size == fixture.map.size()) {
      --request.emphasis_map.byte_size;
    }
    require(!glyphrelay::validate_nvenc_submission(request).passed,
            "randomized wrong map sizes must all fail closed");
  }
}

void test_submission_fifo_and_retry() {
  RequestFixture fixture;
  glyphrelay::NvencSubmissionCoordinator coordinator(3U);
  auto first = fixture.request(0U, 1U, 0xB100U);
  std::size_t calls = 0U;
  auto busy = coordinator.submit(first, [&calls]() {
    ++calls;
    return glyphrelay::NvencSubmitStatus::encoder_busy;
  });
  require(busy.passed && busy.driver_invoked &&
              coordinator.state(0U) == glyphrelay::SubmissionState::submit_retry_pending &&
              coordinator.pending_fifo().empty() && calls == 1U,
          "ENCODER_BUSY must preserve the slot without adding a FIFO entry");

  auto mutated_retry = first;
  mutated_retry.output_bitstream = 0xB101U;
  const auto mutation = coordinator.submit(mutated_retry, [&calls]() {
    ++calls;
    return glyphrelay::NvencSubmitStatus::success;
  });
  require(!mutation.passed && !mutation.driver_invoked &&
              mutation.reason == "nvenc_busy_retry_mutated" && calls == 1U,
          "a mutated busy retry must fail before the driver call");

  auto mutated_map_retry = first;
  fixture.map.front() = 4;
  const auto map_mutation = coordinator.submit(mutated_map_retry, [&calls]() {
    ++calls;
    return glyphrelay::NvencSubmitStatus::success;
  });
  require(!map_mutation.passed && !map_mutation.driver_invoked &&
              map_mutation.reason == "nvenc_busy_retry_mutated" && calls == 1U,
          "an in-place map mutation must invalidate an ENCODER_BUSY retry");
  fixture.map.front() = 0;

  const auto delayed = coordinator.submit(first, [&calls]() {
    ++calls;
    return glyphrelay::NvencSubmitStatus::need_more_input;
  });
  require(delayed.passed && coordinator.pending_fifo() == std::vector<std::size_t>{0U} &&
              !coordinator.begin_bitstream_lock() && calls == 2U,
          "NEED_MORE_INPUT must append exactly once and remain non-lockable");

  auto second_delayed = fixture.request(1U, 2U, 0xB200U);
  const auto still_delayed = coordinator.submit(second_delayed, [&calls]() {
    ++calls;
    return glyphrelay::NvencSubmitStatus::need_more_input;
  });
  require(still_delayed.passed && !coordinator.begin_bitstream_lock() && calls == 3U,
          "consecutive NEED_MORE_INPUT statuses must not fabricate ready output");

  auto third = fixture.request(2U, 3U, 0xB300U);
  const auto accepted = coordinator.submit(third, [&calls]() {
    ++calls;
    return glyphrelay::NvencSubmitStatus::success;
  });
  require(accepted.passed && coordinator.pending_fifo() == std::vector<std::size_t>({0U, 1U, 2U}) &&
              calls == 4U,
          "a later accepted submission must preserve FIFO order");
  const auto locked_first = coordinator.begin_bitstream_lock();
  require(locked_first && *locked_first == 0U,
          "only the delayed FIFO head may become bitstream-lockable");
  require(!coordinator.complete_bitstream(1U).passed,
          "a non-head bitstream may not complete out of order");
  require(coordinator.complete_bitstream(0U).passed,
          "the locked FIFO head must release successfully");
  const auto locked_second = coordinator.begin_bitstream_lock();
  require(locked_second && *locked_second == 1U,
          "the next ready submission must advance after head release");
  require(coordinator.complete_bitstream(1U).passed,
          "the second delayed submission must release in FIFO order");
  const auto locked_third = coordinator.begin_bitstream_lock();
  require(locked_third && *locked_third == 2U && coordinator.complete_bitstream(2U).passed &&
              coordinator.active_slots() == 0U,
          "completed submissions must return every slot to FREE");

  auto duplicate_output_a = fixture.request(0U, 4U, 0xB300U);
  require(coordinator
              .submit(duplicate_output_a,
                      []() { return glyphrelay::NvencSubmitStatus::need_more_input; })
              .passed,
          "the output-ownership fixture must accept its first submission");
  auto duplicate_output_b = fixture.request(1U, 5U, 0xB300U);
  const auto duplicate = coordinator.submit(
      duplicate_output_b, []() { return glyphrelay::NvencSubmitStatus::success; });
  require(!duplicate.passed && !duplicate.driver_invoked &&
              duplicate.reason == "nvenc_output_bitstream_already_owned",
          "one output bitstream may belong to only one live submission");
  require(coordinator.begin_end_of_stream().passed,
          "EOS must begin an ordered drain for delayed submissions");
  const auto eos_head = coordinator.begin_bitstream_lock();
  require(eos_head && *eos_head == 0U && coordinator.complete_bitstream(0U).passed,
          "EOS must make the delayed FIFO head drainable");
}

void test_fatal_abort_and_preflight_call_boundary() {
  RequestFixture fixture;
  glyphrelay::NvencSubmissionCoordinator invalid_coordinator(1U);
  auto invalid = fixture.request(0U, 1U, 0xB100U);
  invalid.emphasis_map.frame_id += 1U;
  std::size_t calls = 0U;
  const auto rejected = invalid_coordinator.submit(invalid, [&calls]() {
    ++calls;
    return glyphrelay::NvencSubmitStatus::success;
  });
  require(!rejected.passed && !rejected.driver_invoked && calls == 0U &&
              invalid_coordinator.driver_call_count() == 0U,
          "preflight rejection must occur before the NVENC driver boundary");

  glyphrelay::NvencSubmissionCoordinator fatal_coordinator(2U);
  auto first = fixture.request(0U, 1U, 0xB100U);
  require(fatal_coordinator
              .submit(first, []() { return glyphrelay::NvencSubmitStatus::need_more_input; })
              .passed,
          "the fatal fixture must first own one delayed submission");
  auto second = fixture.request(1U, 2U, 0xB200U);
  const auto fatal =
      fatal_coordinator.submit(second, []() { return glyphrelay::NvencSubmitStatus::fatal; });
  require(!fatal.passed && fatal.driver_invoked && fatal_coordinator.fatal() &&
              fatal_coordinator.pending_fifo().empty() &&
              fatal_coordinator.state(0U) == glyphrelay::SubmissionState::abort_pending &&
              fatal_coordinator.state(1U) == glyphrelay::SubmissionState::abort_pending,
          "a fatal status must move every owned submission through the abort path");
  require(fatal_coordinator.confirm_abort(0U).passed &&
              fatal_coordinator.confirm_abort(1U).passed && fatal_coordinator.active_slots() == 0U,
          "driver-confirmed aborts must release all submission ownership");

  glyphrelay::NvencSubmissionCoordinator lock_in_flight(2U);
  require(
      lock_in_flight.submit(first, []() { return glyphrelay::NvencSubmitStatus::success; }).passed,
      "the in-flight lock fixture must accept its FIFO head");
  const auto locked = lock_in_flight.begin_bitstream_lock();
  require(locked && *locked == 0U, "the in-flight lock fixture must own its head");
  const auto fatal_while_locked =
      lock_in_flight.submit(second, []() { return glyphrelay::NvencSubmitStatus::fatal; });
  require(!fatal_while_locked.passed && lock_in_flight.fatal() &&
              lock_in_flight.state(0U) == glyphrelay::SubmissionState::bitstream_locked &&
              lock_in_flight.state(1U) == glyphrelay::SubmissionState::abort_pending,
          "fatal submission must not revoke a bitstream lock still owned by the output worker");
  require(!lock_in_flight.confirm_abort(0U).passed && lock_in_flight.confirm_abort(1U).passed &&
              lock_in_flight.complete_bitstream(0U).passed && lock_in_flight.active_slots() == 0U,
          "fatal cleanup must release the driver-owned lock before its slot becomes free");

  glyphrelay::NvencSubmissionCoordinator bounded_busy(1U, 2U);
  auto busy_request = fixture.request(0U, 1U, 0xB100U);
  require(
      bounded_busy
              .submit(busy_request, []() { return glyphrelay::NvencSubmitStatus::encoder_busy; })
              .passed &&
          bounded_busy
              .submit(busy_request, []() { return glyphrelay::NvencSubmitStatus::encoder_busy; })
              .passed,
      "busy retries within the configured bound must retain ownership");
  const auto exhausted = bounded_busy.submit(
      busy_request, []() { return glyphrelay::NvencSubmitStatus::encoder_busy; });
  require(!exhausted.passed && exhausted.reason == "nvenc_busy_retry_limit_exceeded" &&
              bounded_busy.fatal(),
          "the configured busy retry bound must end in an explicit fatal path");

  glyphrelay::NvencSubmissionCoordinator throwing_driver(1U);
  const auto threw = throwing_driver.submit(busy_request, []() -> glyphrelay::NvencSubmitStatus {
    throw std::runtime_error("seeded driver exception");
  });
  require(!threw.passed && threw.driver_invoked && threw.reason == "nvenc_driver_callback_threw" &&
              throwing_driver.fatal(),
          "an unexpected driver exception must enter the explicit fatal path");
}

void test_ownership_and_shutdown_order() {
  using glyphrelay::DeviceSourceState;
  using glyphrelay::EncoderSurfaceState;
  using glyphrelay::ImportedSourceState;
  require(
      glyphrelay::valid_device_source_transition(DeviceSourceState::free,
                                                 DeviceSourceState::host_to_device_pending) &&
          glyphrelay::valid_device_source_transition(DeviceSourceState::host_to_device_pending,
                                                     DeviceSourceState::cuda_source_read_pending) &&
          glyphrelay::valid_device_source_transition(DeviceSourceState::cuda_source_read_pending,
                                                     DeviceSourceState::free),
      "the shared-memory device-source ownership cycle must be exact");
  require(!glyphrelay::valid_device_source_transition(DeviceSourceState::free,
                                                      DeviceSourceState::cuda_source_read_pending),
          "a shared-memory source may not skip its copy ownership boundary");
  require(glyphrelay::valid_imported_source_transition(
              ImportedSourceState::pipewire_owned, ImportedSourceState::cuda_source_read_pending) &&
              glyphrelay::valid_imported_source_transition(
                  ImportedSourceState::cuda_source_read_pending,
                  ImportedSourceState::pipewire_requeue_pending) &&
              glyphrelay::valid_imported_source_transition(
                  ImportedSourceState::pipewire_requeue_pending, ImportedSourceState::released),
          "the imported source must return through the PipeWire requeue boundary");

  const std::array surface_cycle = {
      EncoderSurfaceState::free,
      EncoderSurfaceState::cuda_writing,
      EncoderSurfaceState::map_copy_pending,
      EncoderSurfaceState::ready_to_submit,
      EncoderSurfaceState::submitted,
      EncoderSurfaceState::encoder_input_released,
      EncoderSurfaceState::free,
  };
  for (std::size_t index = 1U; index < surface_cycle.size(); ++index) {
    require(glyphrelay::valid_encoder_surface_transition(surface_cycle[index - 1U],
                                                         surface_cycle[index]),
            "the NV12 encoder-surface ownership cycle must preserve every boundary");
  }
  require(!glyphrelay::valid_encoder_surface_transition(EncoderSurfaceState::submitted,
                                                        EncoderSurfaceState::free),
          "an NVENC-owned surface may not return directly to FREE");

  glyphrelay::CudaShutdownContract shutdown;
  require(!shutdown.advance(glyphrelay::CudaShutdownPhase::nvenc_drained).passed,
          "shutdown may not drain NVENC before closing and joining producers");
  const std::array phases = {
      glyphrelay::CudaShutdownPhase::admission_closed,
      glyphrelay::CudaShutdownPhase::producers_joined,
      glyphrelay::CudaShutdownPhase::events_resolved,
      glyphrelay::CudaShutdownPhase::nvenc_drained,
      glyphrelay::CudaShutdownPhase::resources_unregistered,
      glyphrelay::CudaShutdownPhase::streams_destroyed,
      glyphrelay::CudaShutdownPhase::context_workers_joined,
      glyphrelay::CudaShutdownPhase::primary_context_released,
  };
  for (const auto phase : phases) {
    require(shutdown.advance(phase).passed, "the declared CUDA teardown phase must advance");
  }
  require(shutdown.complete(), "primary-context release must be the final teardown phase");
}

void test_unavailable_runtime_is_truthful() {
  const auto version = [](std::uint32_t major, std::uint32_t minor) {
    return major | (minor << 24U);
  };
  require(glyphrelay::nvenc_api_version_compatible(version(13U, 1U), version(13U, 1U)) &&
              glyphrelay::nvenc_api_version_compatible(version(13U, 2U), version(13U, 1U)) &&
              glyphrelay::nvenc_api_version_compatible(version(14U, 0U), version(13U, 1U)) &&
              !glyphrelay::nvenc_api_version_compatible(version(13U, 0U), version(13U, 1U)) &&
              !glyphrelay::nvenc_api_version_compatible(version(12U, 2U), version(13U, 1U)),
          "NVENC API compatibility must compare major and minor fields semantically");
  glyphrelay::CudaPrimaryContext invalid_context(-1);
  require(!invalid_context.available() && invalid_context.reason() == "cuda_device_ordinal_invalid",
          "an invalid CUDA device must fail before retaining a context");
  const auto report = glyphrelay::probe_nvenc_capabilities(invalid_context);
  require(!report.passed && report.reason == "nvenc_cuda_primary_context_unavailable",
          "the NVENC probe must not fabricate capabilities without a primary context");
  const auto json = glyphrelay::nvenc_capability_report_json(report);
  require(json.find("\"passed\":false") != std::string::npos &&
              json.find("context_handle") == std::string::npos,
          "the probe JSON must be truthful and omit raw context handles");
  require(invalid_context.shutdown(), "an unretained CUDA context must shut down idempotently");
  require(invalid_context.shutdown(), "repeated CUDA context shutdown must remain safe");
}

} // namespace

int main() {
  test_pre_submit_validation();
  test_submission_fifo_and_retry();
  test_fatal_abort_and_preflight_call_boundary();
  test_ownership_and_shutdown_order();
  test_unavailable_runtime_is_truthful();
  return 0;
}
