#pragma once

#include "glyphrelay/controller.hpp"
#include "glyphrelay/encoded_fanout.hpp"
#include "glyphrelay/peer_sender.hpp"
#include "glyphrelay/record_command.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace glyphrelay {

enum class ShareTransportEventKind {
  session_created,
  join_created,
  peer_ready,
  peer_disconnected,
  recovery_requested,
  feedback,
  terminal,
};

struct ShareTransportFeedback {
  std::optional<double> loss_fraction;
  std::optional<double> round_trip_time_milliseconds;
  std::optional<double> remb_bits_per_second;
  bool remb_payload_type_valid = false;
  bool remb_rtcp_source_valid = false;
  std::optional<ReceiverControlStats> receiver_stats;
};

struct ShareTransportEvent {
  ShareTransportEventKind kind = ShareTransportEventKind::terminal;
  std::string value;
  ShareTransportFeedback feedback;
};

struct ShareTransportDiagnostics {
  bool started = false;
  bool peer_ready = false;
  bool stopped = false;
  std::uint64_t access_units_sent = 0;
  std::uint64_t bytes_sent = 0;
  PeerSenderDiagnostics peer;
  std::string reason;
};

class ShareTransport {
public:
  virtual ~ShareTransport() = default;
  virtual bool start() = 0;
  virtual std::optional<ShareTransportEvent> poll_event(std::chrono::milliseconds timeout) = 0;
  virtual bool send_access_unit(const RecordedAccessUnit &access_unit) = 0;
  virtual bool set_pacing_target_bits_per_second(double target_bits_per_second) = 0;
  virtual void stop(std::string_view reason) = 0;
  virtual ShareTransportDiagnostics diagnostics() const = 0;
};

struct ShareCommandOptions {
  std::string signaling_origin;
  std::optional<std::filesystem::path> signaling_ca_path;
  std::string bitrate_profile = "2m";
  std::optional<std::filesystem::path> recording_path;
  bool json = false;
};

struct ShareRunResult {
  int exit_code = 8;
  std::string reason;
  std::string join_url;
  std::string connection_state = "idle";
  std::string recording_error;
  std::uint64_t captured_frames = 0;
  std::uint64_t encoded_access_units = 0;
  std::uint64_t transported_access_units = 0;
  std::uint64_t frame_rate_drops = 0;
  std::uint64_t controller_ticks = 0;
  std::uint64_t controller_feedback_events = 0;
  std::uint64_t controller_actions = 0;
  std::uint64_t elementary_stream_bytes = 0;
  CapturePoolDiagnostics capture;
  RecorderDiagnostics recorder;
  EncodedTransportQueueDiagnostics transport_queue;
  ShareTransportDiagnostics transport;
  ControllerLevelStack controller_levels;
  ControllerState controller_state = ControllerState::stable;
  std::string last_controller_trace;
};

using ShareStatusCallback = std::function<void(std::string_view, std::string_view)>;

bool valid_share_options(const ShareCommandOptions &options, std::string &reason);
ShareRunResult run_share_pipeline(const ShareCommandOptions &options, RecordFrameSource &source,
                                  ShareTransport &transport,
                                  RecordStopPredicate stop_requested = {},
                                  ShareStatusCallback status = {});
ShareRunResult run_interactive_share(const ShareCommandOptions &options,
                                     RecordStopPredicate stop_requested = {},
                                     ShareStatusCallback status = {});

std::string_view share_transport_event_name(ShareTransportEventKind kind);

} // namespace glyphrelay
