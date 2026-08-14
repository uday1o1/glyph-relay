#include "glyphrelay/record_command.hpp"

#include "glyphrelay/color_conversion.hpp"
#include "glyphrelay/i420.hpp"
#include "glyphrelay/nv12_scaler.hpp"
#include "glyphrelay/openh264_encoder.hpp"
#include "glyphrelay/recording_profile.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace glyphrelay {
namespace {

constexpr std::size_t kRecordWidth = 1920U;
constexpr std::size_t kRecordHeight = 1080U;
constexpr unsigned int kRecordFramesPerSecond = 30U;
constexpr std::uint64_t kFrameIntervalNs = 1'000'000'000ULL / kRecordFramesPerSecond;
constexpr std::uint64_t kAbsoluteSessionNs = 8ULL * 60ULL * 60ULL * 1'000'000'000ULL;

int capture_terminal_exit(CaptureState state) {
  return state == CaptureState::revoked || state == CaptureState::cancelled ? 4 : 3;
}

bool output_initialization_reason(std::string_view reason) {
  return reason == "OUTPUT_EXISTS" || reason == "OUTPUT_INCOMPLETE_EXISTS" ||
         reason == "OUTPUT_PATH_UNSAFE" || reason == "OUTPUT_DIRECTORY_UNSAFE" ||
         reason == "OUTPUT_PERMISSION_DENIED" || reason == "RENAME_NOREPLACE_UNAVAILABLE";
}

std::uint64_t rtp_timestamp(std::uint64_t relative_ns) {
  constexpr std::uint64_t clock_rate = 90'000U;
  const auto seconds = relative_ns / 1'000'000'000ULL;
  const auto remainder = relative_ns % 1'000'000'000ULL;
  if (seconds > (std::numeric_limits<std::uint64_t>::max() - clock_rate) / clock_rate) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return clock_rate + seconds * clock_rate + remainder * clock_rate / 1'000'000'000ULL;
}

} // namespace

std::optional<unsigned int> record_bitrate_bps(std::string_view profile) {
  if (profile == "500k") {
    return 500'000U;
  }
  if (profile == "1m") {
    return 1'000'000U;
  }
  if (profile == "2m") {
    return 2'000'000U;
  }
  if (profile == "4m") {
    return 4'000'000U;
  }
  return std::nullopt;
}

bool valid_window_label(std::string_view label) {
  if (label.size() > 128U) {
    return false;
  }
  for (const char value : label) {
    const auto character = static_cast<unsigned char>(value);
    if (character < 0x20U || character == 0x7FU) {
      return false;
    }
  }
  return true;
}

RecordRunResult run_record_pipeline(const RecordCommandOptions &options, RecordFrameSource &source,
                                    RecordStopPredicate stop_requested,
                                    WindowSelectedCallback window_selected) {
  RecordRunResult result;
  const auto bitrate = record_bitrate_bps(options.bitrate_profile);
  if (options.output_path.empty() || options.output_path.extension() != ".h264" || !bitrate ||
      !valid_window_label(options.window_label)) {
    result.exit_code = 2;
    result.reason = "record_arguments_invalid";
    return result;
  }

  OpenH264Encoder encoder({
      .width = kRecordWidth,
      .height = kRecordHeight,
      .frames_per_second = kRecordFramesPerSecond,
      .target_bitrate_bps = *bitrate,
      .maximum_bitrate_bps = *bitrate,
      .gop_frames = 60U,
      .level_idc = 40U,
  });
  if (!encoder.available()) {
    result.exit_code = 5;
    result.reason = encoder.initialization_reason();
    return result;
  }

  const auto selection = source.select_window();
  if (!selection.passed) {
    result.exit_code = selection.cancelled ? 4 : 3;
    result.reason = selection.reason;
    result.capture = source.diagnostics();
    return result;
  }
  if (!options.window_label.empty() && window_selected) {
    window_selected(options.window_label);
  }

  DurableRecorder recorder({
      .output_path = options.output_path,
      .session_id = "record_only",
      .recording_profile_sha256 = recording_profile_candidate_sha256(),
      .maximum_queue_bytes = 64U * 1024U * 1024U,
      .maximum_queue_age_ns = 2'000'000'000ULL,
      .group_commit_interval_ns = 250'000'000ULL,
      .event_callback = {},
  });
  if (!recorder.ready()) {
    result.reason = recorder.initialization_reason();
    result.exit_code = output_initialization_reason(result.reason) ? 2 : 5;
    result.capture = source.diagnostics();
    result.recorder = recorder.diagnostics();
    source.stop(CaptureState::closed);
    return result;
  }

  const auto capture_start = source.start_capture();
  if (!capture_start.passed) {
    source.stop(capture_start.cancelled ? CaptureState::cancelled : CaptureState::disconnected);
    static_cast<void>(recorder.finalize());
    result.exit_code = capture_start.cancelled ? 4 : 3;
    result.reason = capture_start.reason;
    result.capture = source.diagnostics();
    result.recorder = recorder.diagnostics();
    return result;
  }

  const auto session_started = std::chrono::steady_clock::now();
  std::optional<std::uint64_t> first_timestamp_ns;
  std::optional<std::uint64_t> last_frame_slot;
  std::uint64_t dependency_epoch = 1U;
  std::uint64_t last_geometry_epoch = 0U;
  int terminal_exit = 0;
  std::string terminal_reason = "recording_stopped";
  CaptureState stop_state = CaptureState::closed;
  while (true) {
    const bool explicit_stop = stop_requested && stop_requested();
    if (explicit_stop || std::chrono::steady_clock::now() - session_started >=
                             std::chrono::nanoseconds(kAbsoluteSessionNs)) {
      terminal_reason = explicit_stop ? "recording_stopped" : "recording_absolute_expiry";
      break;
    }
    if (const auto terminal = source.poll_terminal()) {
      stop_state = *terminal;
      terminal_exit = *terminal == CaptureState::closed ? 0 : capture_terminal_exit(*terminal);
      terminal_reason = std::string("capture_") + std::string(capture_state_name(*terminal));
      break;
    }
    auto lease = source.take_latest();
    if (!lease) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    const auto &frame = lease->frame();
    ++result.captured_frames;
    if (!first_timestamp_ns) {
      first_timestamp_ns = frame.monotonic_timestamp_ns;
    }
    if (frame.monotonic_timestamp_ns < *first_timestamp_ns) {
      terminal_exit = 8;
      terminal_reason = "capture_timestamp_regressed";
      break;
    }
    const auto relative_ns = frame.monotonic_timestamp_ns - *first_timestamp_ns;
    if (relative_ns >= kAbsoluteSessionNs) {
      terminal_exit = 8;
      terminal_reason = "capture_timestamp_out_of_range";
      break;
    }
    const auto frame_slot = relative_ns / kFrameIntervalNs;
    if (last_frame_slot && frame_slot <= *last_frame_slot) {
      ++result.frame_rate_drops;
      continue;
    }
    last_frame_slot = frame_slot;

    const auto converted =
        convert_bgra_or_rgba_to_nv12(frame, ColorRange::limited, ColorConversionBackend::automatic);
    if (!converted.passed) {
      terminal_exit = 5;
      terminal_reason = converted.reason;
      break;
    }
    const auto scaled = scale_nv12_letterbox(converted.image, kRecordWidth, kRecordHeight);
    if (!scaled.passed) {
      terminal_exit = 5;
      terminal_reason = scaled.reason;
      break;
    }
    I420Frame i420;
    try {
      i420 = nv12_to_i420(scaled.image.bytes, scaled.image.coded_width, scaled.image.coded_height,
                          scaled.image.pitch, scaled.image.pitch, scaled.image.visible_width,
                          scaled.image.visible_height);
    } catch (...) {
      terminal_exit = 5;
      terminal_reason = "record_i420_conversion_failed";
      break;
    }
    bool force_idr = false;
    if (last_geometry_epoch != 0U && last_geometry_epoch != frame.geometry.epoch) {
      ++dependency_epoch;
      force_idr = true;
    }
    last_geometry_epoch = frame.geometry.epoch;
    auto encoded = encoder.encode(i420, force_idr);
    if (!encoded.passed) {
      terminal_exit = 5;
      terminal_reason = encoded.reason;
      break;
    }
    if (encoded.skipped) {
      continue;
    }
    const bool parameter_sets_present =
        encoded.access_unit.contains(7U) && encoded.access_unit.contains(8U);
    auto bytes =
        std::make_shared<const std::vector<std::uint8_t>>(std::move(encoded.access_unit.bytes));
    RecordedAccessUnit access_unit = {
        .bytes = std::move(bytes),
        .media_epoch = 1U,
        .dependency_epoch = dependency_epoch,
        .geometry_epoch = frame.geometry.epoch,
        .encoder_configuration_epoch = 1U,
        .configuration_sha256 = recording_profile_candidate_sha256(),
        .source_frame_id = frame.frame_id,
        .extended_rtp_timestamp = rtp_timestamp(relative_ns),
        .picture_type =
            encoded.keyframe ? RecordingPictureType::idr : RecordingPictureType::predicted,
        .keyframe = encoded.keyframe,
        .parameter_sets_present = parameter_sets_present,
        .presentation_timestamp_ns = relative_ns + 1U,
    };
    const auto admitted = recorder.enqueue(std::move(access_unit));
    if (!admitted.accepted) {
      terminal_exit = 5;
      terminal_reason = admitted.reason;
      break;
    }
    ++result.encoded_access_units;
  }

  source.stop(stop_state);
  const auto finalized = recorder.finalize();
  result.capture = source.diagnostics();
  result.recorder = recorder.diagnostics();
  if (!finalized.passed) {
    result.exit_code = terminal_exit == 3 || terminal_exit == 4 ? terminal_exit : 5;
    result.reason = terminal_exit == 0 ? finalized.reason : terminal_reason;
    return result;
  }
  const auto inspection = inspect_recording(options.output_path);
  if (!inspection.passed || inspection.state != RecordingInspectionState::complete) {
    result.exit_code = 8;
    result.reason = inspection.reason;
    return result;
  }
  result.exit_code = terminal_exit;
  result.reason = terminal_reason;
  return result;
}

} // namespace glyphrelay
