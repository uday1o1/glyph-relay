#include "glyphrelay/share_command.hpp"

#include "glyphrelay/color_conversion.hpp"
#include "glyphrelay/i420.hpp"
#include "glyphrelay/nv12_scaler.hpp"
#include "glyphrelay/openh264_encoder.hpp"
#include "glyphrelay/recording_profile.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace glyphrelay {
namespace {

constexpr std::size_t kShareWidth = 1280U;
constexpr std::size_t kShareHeight = 720U;
constexpr unsigned int kShareFramesPerSecond = 30U;
constexpr std::uint64_t kFrameIntervalNs = 1'000'000'000ULL / kShareFramesPerSecond;
constexpr std::uint64_t kAbsoluteSessionNs = 8ULL * 60ULL * 60ULL * 1'000'000'000ULL;

std::uint64_t monotonic_now_ns() {
  const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  return value < 0 ? 0U : static_cast<std::uint64_t>(value);
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

int capture_terminal_exit(CaptureState state) {
  return state == CaptureState::revoked || state == CaptureState::cancelled ? 4 : 3;
}

bool output_initialization_reason(std::string_view reason) {
  return reason == "OUTPUT_EXISTS" || reason == "OUTPUT_INCOMPLETE_EXISTS" ||
         reason == "OUTPUT_PATH_UNSAFE" || reason == "OUTPUT_DIRECTORY_UNSAFE" ||
         reason == "OUTPUT_PERMISSION_DENIED" || reason == "RENAME_NOREPLACE_UNAVAILABLE";
}

void report_status(const ShareStatusCallback &callback, std::string_view state,
                   std::string_view detail) {
  if (!callback) {
    return;
  }
  try {
    callback(state, detail);
  } catch (...) {
  }
}

} // namespace

bool valid_share_options(const ShareCommandOptions &options, std::string &reason) {
  if (options.signaling_origin.empty()) {
    reason = "signaling_origin_unconfigured";
    return false;
  }
  if (!record_bitrate_bps(options.bitrate_profile)) {
    reason = "share_bitrate_invalid";
    return false;
  }
  if (options.recording_path &&
      (options.recording_path->empty() || options.recording_path->extension() != ".h264")) {
    reason = "share_recording_path_invalid";
    return false;
  }
  if (options.signaling_ca_path && options.signaling_ca_path->empty()) {
    reason = "signaling_ca_path_invalid";
    return false;
  }
  if (options.signaling_ca_path) {
    std::error_code error;
    const bool regular = std::filesystem::is_regular_file(*options.signaling_ca_path, error);
    if (error || !regular) {
      reason = "signaling_ca_path_unavailable";
      return false;
    }
  }
  reason = "share_options_valid";
  return true;
}

ShareRunResult run_share_pipeline(const ShareCommandOptions &options, RecordFrameSource &source,
                                  ShareTransport &transport, RecordStopPredicate stop_requested,
                                  ShareStatusCallback status) {
  ShareRunResult result;
  std::string validation_reason;
  if (!valid_share_options(options, validation_reason)) {
    result.exit_code = 2;
    result.reason = std::move(validation_reason);
    return result;
  }

  const auto bitrate = *record_bitrate_bps(options.bitrate_profile);
  std::unique_ptr<OpenH264Encoder> encoder;
  if (options.recording_path) {
    encoder = std::make_unique<OpenH264Encoder>(OpenH264EncoderConfig{
        .width = kShareWidth,
        .height = kShareHeight,
        .frames_per_second = kShareFramesPerSecond,
        .target_bitrate_bps = bitrate,
        .maximum_bitrate_bps = bitrate,
        .gop_frames = 60U,
        .level_idc = 31U,
    });
    if (!encoder->available()) {
      result.exit_code = 5;
      result.reason = encoder->initialization_reason();
      return result;
    }
  }

  const auto selection = source.select_window();
  if (!selection.passed) {
    result.exit_code = selection.cancelled ? 4 : 3;
    result.reason = selection.reason;
    result.capture = source.diagnostics();
    return result;
  }
  report_status(status, "window_selected", "portal_window_selected");

  std::unique_ptr<DurableRecorder> recorder;
  bool recorder_active = false;
  if (options.recording_path) {
    recorder = std::make_unique<DurableRecorder>(RecorderConfig{
        .output_path = *options.recording_path,
        .session_id = "share_record",
        .recording_profile_sha256 = recording_profile_candidate_sha256(),
        .maximum_queue_bytes = 64U * 1024U * 1024U,
        .maximum_queue_age_ns = 2'000'000'000ULL,
        .group_commit_interval_ns = 250'000'000ULL,
        .event_callback = {},
    });
    if (!recorder->ready()) {
      result.reason = recorder->initialization_reason();
      result.exit_code = output_initialization_reason(result.reason) ? 2 : 5;
      result.capture = source.diagnostics();
      result.recorder = recorder->diagnostics();
      source.stop(CaptureState::closed);
      return result;
    }
    recorder_active = true;
    report_status(status, "recording_ready", "recording_initialized");
  }

  bool capture_started = false;
  if (recorder_active) {
    const auto capture_start = source.start_capture();
    if (!capture_start.passed) {
      source.stop(capture_start.cancelled ? CaptureState::cancelled : CaptureState::disconnected);
      static_cast<void>(recorder->finalize());
      result.exit_code = capture_start.cancelled ? 4 : 3;
      result.reason = capture_start.reason;
      result.capture = source.diagnostics();
      result.recorder = recorder->diagnostics();
      return result;
    }
    capture_started = true;
    report_status(status, "capturing", "recording_before_receiver");
  }

  bool transport_started = transport.start();
  bool transport_active = transport_started;
  if (!transport_started) {
    result.connection_state = "signaling_failed";
    result.reason = transport.diagnostics().reason;
    report_status(status, result.connection_state, result.reason);
    if (!recorder_active) {
      source.stop(CaptureState::closed);
      result.exit_code = 6;
      result.capture = source.diagnostics();
      result.transport = transport.diagnostics();
      return result;
    }
  } else {
    result.connection_state = "signaling";
    report_status(status, result.connection_state, "owner_session_starting");
  }

  EncodedTransportQueue transport_queue;
  std::unique_ptr<EncodedAccessUnitFanout> fanout;
  bool peer_ready = false;
  bool force_idr = true;
  bool transport_terminal = !transport_started;
  std::optional<std::uint64_t> first_timestamp_ns;
  std::optional<std::uint64_t> last_frame_slot;
  std::uint64_t dependency_epoch = 1U;
  std::uint64_t last_geometry_epoch = 0U;
  const auto session_started = std::chrono::steady_clock::now();
  int terminal_exit = 0;
  std::string terminal_reason = "share_stopped";
  CaptureState stop_state = CaptureState::closed;

  const auto disable_remote = [&](std::string reason) {
    if (!transport_active) {
      return;
    }
    transport_active = false;
    transport_terminal = true;
    peer_ready = false;
    if (fanout) {
      fanout->disable_transport();
    } else {
      transport_queue.stop();
    }
    transport.stop(reason);
    result.connection_state = "revoked";
    report_status(status, result.connection_state, reason);
  };

  while (true) {
    const bool explicit_stop = stop_requested && stop_requested();
    if (explicit_stop || std::chrono::steady_clock::now() - session_started >=
                             std::chrono::nanoseconds(kAbsoluteSessionNs)) {
      terminal_reason = explicit_stop ? "share_stopped" : "share_absolute_expiry";
      break;
    }

    if (transport_active) {
      while (const auto event = transport.poll_event(std::chrono::milliseconds(0))) {
        switch (event->kind) {
        case ShareTransportEventKind::session_created:
          result.connection_state = "owner_only";
          report_status(status, result.connection_state, "session_created");
          break;
        case ShareTransportEventKind::join_created:
          result.join_url = event->value;
          result.connection_state = "join_open";
          report_status(status, result.connection_state, result.join_url);
          break;
        case ShareTransportEventKind::peer_ready:
          if (!peer_ready) {
            peer_ready = true;
            if (capture_started) {
              ++dependency_epoch;
              static_cast<void>(transport_queue.require_recovery());
            }
            force_idr = true;
            if (!recorder_active || !recorder) {
              fanout = std::make_unique<EncodedAccessUnitFanout>(&transport_queue);
            } else {
              fanout = std::make_unique<EncodedAccessUnitFanout>(
                  &transport_queue, [&recorder](RecordedAccessUnit access_unit) {
                    return recorder->enqueue(std::move(access_unit));
                  });
            }
            result.connection_state = "connected";
            report_status(status, result.connection_state, "receiver_ready");
          }
          break;
        case ShareTransportEventKind::recovery_requested:
          if (peer_ready) {
            ++dependency_epoch;
            static_cast<void>(transport_queue.require_recovery());
            force_idr = true;
            report_status(status, "recovering", event->value);
          }
          break;
        case ShareTransportEventKind::peer_disconnected:
        case ShareTransportEventKind::terminal:
          disable_remote(event->value.empty() ? "remote_transport_ended" : event->value);
          break;
        }
        if (!transport_active) {
          break;
        }
      }
    }

    if (peer_ready && !encoder) {
      encoder = std::make_unique<OpenH264Encoder>(OpenH264EncoderConfig{
          .width = kShareWidth,
          .height = kShareHeight,
          .frames_per_second = kShareFramesPerSecond,
          .target_bitrate_bps = bitrate,
          .maximum_bitrate_bps = bitrate,
          .gop_frames = 60U,
          .level_idc = 31U,
      });
      if (!encoder->available()) {
        terminal_exit = 5;
        terminal_reason = encoder->initialization_reason();
        disable_remote(terminal_reason);
        break;
      }
    }
    if (peer_ready && !capture_started) {
      const auto capture_start = source.start_capture();
      if (!capture_start.passed) {
        stop_state = capture_start.cancelled ? CaptureState::cancelled : CaptureState::disconnected;
        terminal_exit = capture_start.cancelled ? 4 : 3;
        terminal_reason = capture_start.reason;
        disable_remote(terminal_reason);
        break;
      }
      capture_started = true;
      report_status(status, "capturing", "receiver_ready");
    }

    if (!capture_started) {
      if (transport_terminal && !recorder_active) {
        terminal_exit = 6;
        terminal_reason = transport.diagnostics().reason;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    if (const auto terminal = source.poll_terminal()) {
      stop_state = *terminal;
      terminal_exit = *terminal == CaptureState::closed ? 0 : capture_terminal_exit(*terminal);
      terminal_reason = std::string("capture_") + std::string(capture_state_name(*terminal));
      disable_remote(terminal_reason);
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
    const auto scaled = scale_nv12_letterbox(converted.image, kShareWidth, kShareHeight);
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
      terminal_reason = "share_i420_conversion_failed";
      break;
    }
    if (last_geometry_epoch != 0U && last_geometry_epoch != frame.geometry.epoch) {
      ++dependency_epoch;
      if (peer_ready) {
        static_cast<void>(transport_queue.require_recovery());
      }
      force_idr = true;
    }
    last_geometry_epoch = frame.geometry.epoch;
    auto encoded = encoder->encode(i420, force_idr);
    if (!encoded.passed) {
      terminal_exit = 5;
      terminal_reason = encoded.reason;
      break;
    }
    if (encoded.skipped) {
      continue;
    }
    force_idr = false;
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

    ++result.encoded_access_units;
    if (peer_ready && fanout) {
      const auto published = fanout->publish(access_unit, monotonic_now_ns());
      if (published.recorder_failed) {
        recorder_active = false;
        result.recording_error = published.recorder_reason;
        fanout->disable_recorder();
        report_status(status, "recording_failed", result.recording_error);
      }
      if (published.transport_unusable) {
        terminal_exit = 6;
        terminal_reason = published.transport_reason;
        disable_remote(terminal_reason);
        if (!recorder_active) {
          break;
        }
      } else if (published.transport_recovery_required) {
        ++dependency_epoch;
        force_idr = true;
      }
      while (peer_ready) {
        auto next = transport_queue.dequeue(monotonic_now_ns());
        if (!next.access_unit) {
          if (next.recovery_required) {
            ++dependency_epoch;
            force_idr = true;
          }
          break;
        }
        if (!transport.send_access_unit(*next.access_unit)) {
          terminal_exit = 6;
          terminal_reason = transport.diagnostics().reason;
          disable_remote(terminal_reason);
          break;
        }
        ++result.transported_access_units;
      }
    } else if (recorder_active && recorder) {
      const auto admitted = recorder->enqueue(std::move(access_unit));
      if (!admitted.accepted) {
        recorder_active = false;
        result.recording_error = admitted.reason;
        report_status(status, "recording_failed", result.recording_error);
        if (transport_terminal) {
          terminal_exit = 5;
          terminal_reason = admitted.reason;
          break;
        }
      }
    }
  }

  if (fanout) {
    fanout->disable_transport();
    fanout->disable_recorder();
  } else {
    transport_queue.stop();
  }
  if (transport_active) {
    transport.stop(terminal_reason);
    transport_active = false;
  }
  source.stop(stop_state);
  if (recorder) {
    const auto finalized = recorder->finalize();
    result.recorder = recorder->diagnostics();
    if (!finalized.passed && result.recording_error.empty()) {
      result.recording_error = finalized.reason;
      if (terminal_exit == 0 && !peer_ready) {
        terminal_exit = 5;
        terminal_reason = finalized.reason;
      }
    }
  }
  result.capture = source.diagnostics();
  result.transport_queue = transport_queue.diagnostics();
  result.transport = transport.diagnostics();
  result.exit_code = terminal_exit;
  result.reason = std::move(terminal_reason);
  result.connection_state = "stopped";
  report_status(status, result.connection_state, result.reason);
  return result;
}

std::string_view share_transport_event_name(ShareTransportEventKind kind) {
  switch (kind) {
  case ShareTransportEventKind::session_created:
    return "SESSION_CREATED";
  case ShareTransportEventKind::join_created:
    return "JOIN_CREATED";
  case ShareTransportEventKind::peer_ready:
    return "PEER_READY";
  case ShareTransportEventKind::peer_disconnected:
    return "PEER_DISCONNECTED";
  case ShareTransportEventKind::recovery_requested:
    return "RECOVERY_REQUESTED";
  case ShareTransportEventKind::terminal:
    return "TERMINAL";
  }
  return "UNKNOWN";
}

} // namespace glyphrelay
