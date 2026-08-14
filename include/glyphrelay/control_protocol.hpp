#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace glyphrelay {

inline constexpr std::string_view kControlProtocol = "glyphrelay-control-v1";
inline constexpr std::size_t kMaximumControlBytes = 4U * 1024U;
inline constexpr std::size_t kMaximumReceiverControlMessagesPerSecond = 10U;

enum class SenderControlPhase {
  idle,
  connected,
  pause_pending,
  paused,
  resume_pending,
  end_pending,
  ended,
  failed,
};

enum class ReceiverControlEventKind {
  clock_response,
  receiver_stats,
  pause_acknowledged,
  resume_acknowledged,
  end_acknowledged,
  protocol_error,
  terminal,
};

struct ReceiverControlStats {
  std::uint64_t compositor_frames = 0;
  std::uint64_t decoded_frames = 0;
  std::uint64_t dropped_frames = 0;
  std::optional<std::uint32_t> latest_presented_rtp_timestamp;
};

struct ReceiverControlEvent {
  ReceiverControlEventKind kind = ReceiverControlEventKind::terminal;
  std::uint64_t request_sequence = 0;
  double sender_send_time_ms = 0.0;
  double receiver_receive_time_ms = 0.0;
  double receiver_send_time_ms = 0.0;
  ReceiverControlStats stats;
  std::string reason;
};

struct SenderControlOutput {
  bool valid = true;
  bool close_channel = false;
  std::vector<std::string> outbound_messages;
  std::vector<ReceiverControlEvent> events;
};

struct SenderControlDiagnostics {
  SenderControlPhase phase = SenderControlPhase::idle;
  std::uint64_t send_sequence = 0;
  std::uint64_t receive_sequence = 0;
  std::uint64_t media_epoch = 0;
  std::uint64_t dependency_epoch = 0;
  std::size_t outstanding_clock_requests = 0;
  std::uint64_t received_control_messages = 0;
  std::string reason;
};

class SenderControlProtocol {
public:
  explicit SenderControlProtocol(std::string session_id);

  SenderControlOutput begin(std::uint64_t media_epoch);
  SenderControlOutput request_clock(double sender_send_time_ms);
  SenderControlOutput pause(std::uint64_t closed_media_epoch);
  SenderControlOutput resume(std::uint64_t media_epoch, std::uint64_t dependency_epoch);
  SenderControlOutput end(std::uint64_t media_epoch, std::string_view reason);
  SenderControlOutput protocol_error(std::string_view code);
  SenderControlOutput receive(std::string_view encoded, double received_at_ms);
  SenderControlOutput channel_closed();
  SenderControlDiagnostics diagnostics() const;

private:
  SenderControlOutput fail(std::string reason);
  SenderControlOutput message(std::string_view type);
  bool admit_receiver_message(double received_at_ms);
  void clear_pending();

  std::string session_id_;
  SenderControlPhase phase_ = SenderControlPhase::idle;
  std::uint64_t send_sequence_ = 0;
  std::uint64_t receive_sequence_ = 0;
  std::uint64_t media_epoch_ = 0;
  std::uint64_t dependency_epoch_ = 0;
  std::uint64_t received_control_messages_ = 0;
  std::unordered_map<std::uint64_t, double> outstanding_clock_requests_;
  std::optional<std::uint64_t> pending_transition_sequence_;
  std::optional<double> last_clock_request_ms_;
  std::optional<double> last_receive_time_ms_;
  std::optional<ReceiverControlStats> last_stats_;
  std::deque<double> receiver_message_times_;
  std::uint64_t clock_requests_sent_ = 0;
  std::string reason_ = "CONTROL_IDLE";
};

std::string_view sender_control_phase_name(SenderControlPhase phase);

} // namespace glyphrelay
