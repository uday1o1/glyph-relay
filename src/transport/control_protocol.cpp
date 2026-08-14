#include "glyphrelay/control_protocol.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace glyphrelay {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kMaximumSafeInteger = 9'007'199'254'740'991ULL;
constexpr std::uint64_t kMaximumRtpTimestamp = 0xFFFF'FFFFULL;
constexpr std::uint64_t kInitialClockRequestCount = 5U;
constexpr std::size_t kMaximumOutstandingClockRequests = 8U;
constexpr double kClockRequestIntervalMs = 5'000.0;
constexpr double kReceiverRateWindowMs = 1'000.0;

const std::regex kSessionPattern("^[A-Za-z0-9_-]{22}$");
const std::regex kReasonPattern("^[A-Z0-9_]{1,64}$");

bool valid_time(const Json &value) {
  if (!value.is_number()) {
    return false;
  }
  const auto number = value.get<double>();
  return std::isfinite(number) && number >= 0.0 &&
         number <= static_cast<double>(kMaximumSafeInteger);
}

bool valid_safe_integer(const Json &value, bool allow_zero = false) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    return (allow_zero || number > 0U) && number <= kMaximumSafeInteger;
  }
  if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    return (allow_zero ? number >= 0 : number > 0) &&
           static_cast<std::uint64_t>(number) <= kMaximumSafeInteger;
  }
  return false;
}

std::uint64_t safe_integer(const Json &value) {
  return value.is_number_unsigned() ? value.get<std::uint64_t>()
                                    : static_cast<std::uint64_t>(value.get<std::int64_t>());
}

bool exact_keys(const Json &value, std::initializer_list<std::string_view> expected) {
  if (!value.is_object() || value.size() != expected.size()) {
    return false;
  }
  return std::all_of(expected.begin(), expected.end(),
                     [&](std::string_view key) { return value.contains(std::string(key)); });
}

bool valid_reason(const Json &value, std::string_view key) {
  const auto iterator = value.find(std::string(key));
  return iterator != value.end() && iterator->is_string() &&
         std::regex_match(iterator->get<std::string>(), kReasonPattern);
}

Json strict_parse(std::string_view encoded, bool &duplicate_key) {
  std::vector<std::unordered_set<std::string>> object_keys;
  const auto reject_duplicate_keys = [&](int, Json::parse_event_t event, Json &parsed) {
    if (event == Json::parse_event_t::object_start) {
      object_keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (object_keys.empty() || !object_keys.back().insert(parsed.get<std::string>()).second) {
        duplicate_key = true;
      }
    } else if (event == Json::parse_event_t::object_end && !object_keys.empty()) {
      object_keys.pop_back();
    }
    return true;
  };
  return Json::parse(encoded.begin(), encoded.end(), reject_duplicate_keys, false, false);
}

std::string serialize(const Json &value) {
  const auto encoded = value.dump();
  if (encoded.empty() || encoded.size() > kMaximumControlBytes) {
    throw std::runtime_error("CONTROL_MESSAGE_SIZE_INVALID");
  }
  return encoded;
}

bool active_phase(SenderControlPhase phase) {
  return phase == SenderControlPhase::connected || phase == SenderControlPhase::pause_pending ||
         phase == SenderControlPhase::paused || phase == SenderControlPhase::resume_pending ||
         phase == SenderControlPhase::end_pending;
}

} // namespace

SenderControlProtocol::SenderControlProtocol(std::string session_id)
    : session_id_(std::move(session_id)) {
  if (!std::regex_match(session_id_, kSessionPattern)) {
    throw std::invalid_argument("control_session_id_invalid");
  }
}

SenderControlOutput SenderControlProtocol::begin(std::uint64_t media_epoch) {
  if (phase_ != SenderControlPhase::idle || media_epoch == 0U ||
      media_epoch > kMaximumSafeInteger) {
    return fail("CONTROL_BEGIN_INVALID");
  }
  media_epoch_ = media_epoch;
  phase_ = SenderControlPhase::connected;
  reason_ = "CONTROL_CONNECTED";
  auto output = message("HELLO");
  if (output.valid) {
    Json decoded = Json::parse(output.outbound_messages.front());
    decoded["mediaEpoch"] = media_epoch_;
    output.outbound_messages.front() = serialize(decoded);
  }
  return output;
}

SenderControlOutput SenderControlProtocol::request_clock(double sender_send_time_ms) {
  if (phase_ != SenderControlPhase::connected || !std::isfinite(sender_send_time_ms) ||
      sender_send_time_ms < 0.0 || sender_send_time_ms > static_cast<double>(kMaximumSafeInteger) ||
      (last_clock_request_ms_ && sender_send_time_ms < *last_clock_request_ms_)) {
    return fail("CONTROL_CLOCK_REQUEST_INVALID");
  }
  if (clock_requests_sent_ >= kInitialClockRequestCount && last_clock_request_ms_ &&
      sender_send_time_ms - *last_clock_request_ms_ < kClockRequestIntervalMs) {
    return {};
  }
  if (outstanding_clock_requests_.size() >= kMaximumOutstandingClockRequests) {
    return fail("CONTROL_CLOCK_REQUESTS_EXHAUSTED");
  }
  auto output = message("CLOCK_REQUEST");
  if (!output.valid) {
    return output;
  }
  Json decoded = Json::parse(output.outbound_messages.front());
  decoded["senderSendTimeMs"] = sender_send_time_ms;
  output.outbound_messages.front() = serialize(decoded);
  outstanding_clock_requests_[send_sequence_] = sender_send_time_ms;
  last_clock_request_ms_ = sender_send_time_ms;
  ++clock_requests_sent_;
  return output;
}

SenderControlOutput SenderControlProtocol::pause(std::uint64_t closed_media_epoch) {
  if (phase_ != SenderControlPhase::connected || closed_media_epoch != media_epoch_) {
    return fail("CONTROL_PAUSE_INVALID");
  }
  auto output = message("SESSION_PAUSED");
  if (output.valid) {
    Json decoded = Json::parse(output.outbound_messages.front());
    decoded["mediaEpoch"] = closed_media_epoch;
    output.outbound_messages.front() = serialize(decoded);
    pending_transition_sequence_ = send_sequence_;
    phase_ = SenderControlPhase::pause_pending;
    reason_ = "CONTROL_PAUSE_PENDING";
  }
  return output;
}

SenderControlOutput SenderControlProtocol::resume(std::uint64_t media_epoch,
                                                  std::uint64_t dependency_epoch) {
  if (phase_ != SenderControlPhase::paused || media_epoch <= media_epoch_ || media_epoch == 0U ||
      dependency_epoch == 0U || media_epoch > kMaximumSafeInteger ||
      dependency_epoch > kMaximumSafeInteger) {
    return fail("CONTROL_RESUME_INVALID");
  }
  auto output = message("SESSION_RESUMED");
  if (output.valid) {
    Json decoded = Json::parse(output.outbound_messages.front());
    decoded["dependencyEpoch"] = dependency_epoch;
    decoded["mediaEpoch"] = media_epoch;
    output.outbound_messages.front() = serialize(decoded);
    media_epoch_ = media_epoch;
    dependency_epoch_ = dependency_epoch;
    pending_transition_sequence_ = send_sequence_;
    phase_ = SenderControlPhase::resume_pending;
    reason_ = "CONTROL_RESUME_PENDING";
  }
  return output;
}

SenderControlOutput SenderControlProtocol::end(std::uint64_t media_epoch, std::string_view reason) {
  if (!active_phase(phase_) || media_epoch != media_epoch_ ||
      !std::regex_match(std::string(reason), kReasonPattern)) {
    return fail("CONTROL_END_INVALID");
  }
  auto output = message("SESSION_ENDED");
  if (output.valid) {
    Json decoded = Json::parse(output.outbound_messages.front());
    decoded["mediaEpoch"] = media_epoch;
    decoded["reason"] = reason;
    output.outbound_messages.front() = serialize(decoded);
    pending_transition_sequence_ = send_sequence_;
    phase_ = SenderControlPhase::end_pending;
    reason_ = "CONTROL_END_PENDING";
  }
  return output;
}

SenderControlOutput SenderControlProtocol::protocol_error(std::string_view code) {
  if (!active_phase(phase_) || !std::regex_match(std::string(code), kReasonPattern)) {
    return fail("CONTROL_PROTOCOL_ERROR_INVALID");
  }
  auto output = message("PROTOCOL_ERROR");
  if (output.valid) {
    Json decoded = Json::parse(output.outbound_messages.front());
    decoded["code"] = code;
    output.outbound_messages.front() = serialize(decoded);
    output.close_channel = true;
    phase_ = SenderControlPhase::failed;
    reason_ = std::string(code);
    clear_pending();
    output.events.push_back({.kind = ReceiverControlEventKind::terminal,
                             .request_sequence = 0U,
                             .sender_send_time_ms = 0.0,
                             .receiver_receive_time_ms = 0.0,
                             .receiver_send_time_ms = 0.0,
                             .stats = {},
                             .reason = reason_});
  }
  return output;
}

SenderControlOutput SenderControlProtocol::receive(std::string_view encoded,
                                                   double received_at_ms) {
  if (!active_phase(phase_) || encoded.empty() || encoded.size() > kMaximumControlBytes) {
    return fail("CONTROL_RECEIVE_INVALID");
  }
  if (!admit_receiver_message(received_at_ms)) {
    return fail("CONTROL_RECEIVER_RATE_OR_CLOCK_INVALID");
  }
  bool duplicate_key = false;
  const auto decoded = strict_parse(encoded, duplicate_key);
  if (duplicate_key || decoded.is_discarded() || !decoded.is_object() ||
      !decoded.contains("protocolVersion") || !decoded.at("protocolVersion").is_string() ||
      decoded.at("protocolVersion") != kControlProtocol || !decoded.contains("sessionId") ||
      !decoded.at("sessionId").is_string() || decoded.at("sessionId") != session_id_ ||
      !decoded.contains("sequence") || !valid_safe_integer(decoded.at("sequence")) ||
      safe_integer(decoded.at("sequence")) != receive_sequence_ + 1U || !decoded.contains("type") ||
      !decoded.at("type").is_string()) {
    return fail("CONTROL_MESSAGE_INVALID");
  }
  const auto type = decoded.at("type").get<std::string>();
  SenderControlOutput output;

  if (type == "CLOCK_RESPONSE") {
    if (!exact_keys(decoded,
                    {"protocolVersion", "receiverReceiveTimeMs", "receiverSendTimeMs",
                     "requestSequence", "senderSendTimeMs", "sequence", "sessionId", "type"}) ||
        !decoded.contains("requestSequence") ||
        !valid_safe_integer(decoded.at("requestSequence")) ||
        !decoded.contains("senderSendTimeMs") || !valid_time(decoded.at("senderSendTimeMs")) ||
        !decoded.contains("receiverReceiveTimeMs") ||
        !valid_time(decoded.at("receiverReceiveTimeMs")) ||
        !decoded.contains("receiverSendTimeMs") || !valid_time(decoded.at("receiverSendTimeMs"))) {
      return fail("CONTROL_CLOCK_RESPONSE_INVALID");
    }
    const auto request_sequence = safe_integer(decoded.at("requestSequence"));
    const auto request = outstanding_clock_requests_.find(request_sequence);
    if (request == outstanding_clock_requests_.end() ||
        request->second != decoded.at("senderSendTimeMs").get<double>() ||
        decoded.at("receiverSendTimeMs").get<double>() <
            decoded.at("receiverReceiveTimeMs").get<double>()) {
      return fail("CONTROL_CLOCK_RESPONSE_MISMATCH");
    }
    output.events.push_back(
        {.kind = ReceiverControlEventKind::clock_response,
         .request_sequence = request_sequence,
         .sender_send_time_ms = request->second,
         .receiver_receive_time_ms = decoded.at("receiverReceiveTimeMs").get<double>(),
         .receiver_send_time_ms = decoded.at("receiverSendTimeMs").get<double>(),
         .stats = {},
         .reason = {}});
    outstanding_clock_requests_.erase(request);
  } else if (type == "RECEIVER_STATS") {
    if (phase_ != SenderControlPhase::connected ||
        !exact_keys(decoded, {"compositorFrames", "decodedFrames", "droppedFrames",
                              "latestPresentedRtpTimestamp", "protocolVersion", "sequence",
                              "sessionId", "type"}) ||
        !decoded.contains("compositorFrames") ||
        !valid_safe_integer(decoded.at("compositorFrames"), true) ||
        !decoded.contains("decodedFrames") ||
        !valid_safe_integer(decoded.at("decodedFrames"), true) ||
        !decoded.contains("droppedFrames") ||
        !valid_safe_integer(decoded.at("droppedFrames"), true) ||
        !decoded.contains("latestPresentedRtpTimestamp") ||
        (!decoded.at("latestPresentedRtpTimestamp").is_null() &&
         (!valid_safe_integer(decoded.at("latestPresentedRtpTimestamp"), true) ||
          safe_integer(decoded.at("latestPresentedRtpTimestamp")) > kMaximumRtpTimestamp))) {
      return fail("CONTROL_RECEIVER_STATS_INVALID");
    }
    ReceiverControlStats stats{
        .compositor_frames = safe_integer(decoded.at("compositorFrames")),
        .decoded_frames = safe_integer(decoded.at("decodedFrames")),
        .dropped_frames = safe_integer(decoded.at("droppedFrames")),
        .latest_presented_rtp_timestamp =
            decoded.at("latestPresentedRtpTimestamp").is_null()
                ? std::nullopt
                : std::optional<std::uint32_t>(static_cast<std::uint32_t>(
                      safe_integer(decoded.at("latestPresentedRtpTimestamp")))),
    };
    if (last_stats_ && (stats.compositor_frames < last_stats_->compositor_frames ||
                        stats.decoded_frames < last_stats_->decoded_frames ||
                        stats.dropped_frames < last_stats_->dropped_frames)) {
      return fail("CONTROL_RECEIVER_STATS_REGRESSED");
    }
    last_stats_ = stats;
    output.events.push_back({.kind = ReceiverControlEventKind::receiver_stats,
                             .request_sequence = 0U,
                             .sender_send_time_ms = 0.0,
                             .receiver_receive_time_ms = 0.0,
                             .receiver_send_time_ms = 0.0,
                             .stats = stats,
                             .reason = {}});
  } else if (type == "SESSION_PAUSED_ACK" || type == "SESSION_RESUMED_ACK" ||
             type == "SESSION_ENDED_ACK") {
    if (!exact_keys(decoded,
                    {"protocolVersion", "requestSequence", "sequence", "sessionId", "type"}) ||
        !decoded.contains("requestSequence") ||
        !valid_safe_integer(decoded.at("requestSequence")) || !pending_transition_sequence_ ||
        safe_integer(decoded.at("requestSequence")) != *pending_transition_sequence_) {
      return fail("CONTROL_ACK_INVALID");
    }
    ReceiverControlEventKind kind;
    if (type == "SESSION_PAUSED_ACK" && phase_ == SenderControlPhase::pause_pending) {
      phase_ = SenderControlPhase::paused;
      reason_ = "CONTROL_PAUSED";
      kind = ReceiverControlEventKind::pause_acknowledged;
    } else if (type == "SESSION_RESUMED_ACK" && phase_ == SenderControlPhase::resume_pending) {
      phase_ = SenderControlPhase::connected;
      reason_ = "CONTROL_CONNECTED";
      kind = ReceiverControlEventKind::resume_acknowledged;
    } else if (type == "SESSION_ENDED_ACK" && phase_ == SenderControlPhase::end_pending) {
      phase_ = SenderControlPhase::ended;
      reason_ = "CONTROL_ENDED";
      kind = ReceiverControlEventKind::end_acknowledged;
      output.close_channel = true;
      outstanding_clock_requests_.clear();
    } else {
      return fail("CONTROL_ACK_STATE_INVALID");
    }
    output.events.push_back({.kind = kind,
                             .request_sequence = *pending_transition_sequence_,
                             .sender_send_time_ms = 0.0,
                             .receiver_receive_time_ms = 0.0,
                             .receiver_send_time_ms = 0.0,
                             .stats = {},
                             .reason = {}});
    pending_transition_sequence_.reset();
  } else if (type == "PROTOCOL_ERROR") {
    if (!exact_keys(decoded, {"code", "protocolVersion", "sequence", "sessionId", "type"}) ||
        !valid_reason(decoded, "code")) {
      return fail("CONTROL_REMOTE_PROTOCOL_ERROR_INVALID");
    }
    reason_ = decoded.at("code").get<std::string>();
    phase_ = SenderControlPhase::failed;
    output.close_channel = true;
    clear_pending();
    output.events.push_back({.kind = ReceiverControlEventKind::protocol_error,
                             .request_sequence = 0U,
                             .sender_send_time_ms = 0.0,
                             .receiver_receive_time_ms = 0.0,
                             .receiver_send_time_ms = 0.0,
                             .stats = {},
                             .reason = reason_});
  } else {
    return fail("CONTROL_TYPE_INVALID");
  }

  receive_sequence_ += 1U;
  received_control_messages_ += 1U;
  return output;
}

SenderControlOutput SenderControlProtocol::channel_closed() {
  if (phase_ == SenderControlPhase::ended || phase_ == SenderControlPhase::failed) {
    return {};
  }
  return fail("CONTROL_CHANNEL_CLOSED");
}

SenderControlDiagnostics SenderControlProtocol::diagnostics() const {
  return {
      .phase = phase_,
      .send_sequence = send_sequence_,
      .receive_sequence = receive_sequence_,
      .media_epoch = media_epoch_,
      .dependency_epoch = dependency_epoch_,
      .outstanding_clock_requests = outstanding_clock_requests_.size(),
      .received_control_messages = received_control_messages_,
      .reason = reason_,
  };
}

SenderControlOutput SenderControlProtocol::fail(std::string reason) {
  SenderControlOutput output;
  output.valid = false;
  if (phase_ == SenderControlPhase::failed || phase_ == SenderControlPhase::ended) {
    return output;
  }
  phase_ = SenderControlPhase::failed;
  reason_ = std::move(reason);
  clear_pending();
  output.close_channel = true;
  output.events.push_back({.kind = ReceiverControlEventKind::terminal,
                           .request_sequence = 0U,
                           .sender_send_time_ms = 0.0,
                           .receiver_receive_time_ms = 0.0,
                           .receiver_send_time_ms = 0.0,
                           .stats = {},
                           .reason = reason_});
  return output;
}

SenderControlOutput SenderControlProtocol::message(std::string_view type) {
  if (send_sequence_ >= kMaximumSafeInteger) {
    return fail("CONTROL_SEND_SEQUENCE_EXHAUSTED");
  }
  ++send_sequence_;
  Json value = {
      {"protocolVersion", kControlProtocol},
      {"sequence", send_sequence_},
      {"sessionId", session_id_},
      {"type", type},
  };
  SenderControlOutput output;
  try {
    output.outbound_messages.push_back(serialize(value));
  } catch (const std::exception &) {
    return fail("CONTROL_MESSAGE_SIZE_INVALID");
  }
  return output;
}

bool SenderControlProtocol::admit_receiver_message(double received_at_ms) {
  if (!std::isfinite(received_at_ms) || received_at_ms < 0.0 ||
      received_at_ms > static_cast<double>(kMaximumSafeInteger) ||
      (last_receive_time_ms_ && received_at_ms < *last_receive_time_ms_)) {
    return false;
  }
  last_receive_time_ms_ = received_at_ms;
  while (!receiver_message_times_.empty() &&
         received_at_ms - receiver_message_times_.front() >= kReceiverRateWindowMs) {
    receiver_message_times_.pop_front();
  }
  if (receiver_message_times_.size() >= kMaximumReceiverControlMessagesPerSecond) {
    return false;
  }
  receiver_message_times_.push_back(received_at_ms);
  return true;
}

void SenderControlProtocol::clear_pending() {
  outstanding_clock_requests_.clear();
  pending_transition_sequence_.reset();
}

std::string_view sender_control_phase_name(SenderControlPhase phase) {
  switch (phase) {
  case SenderControlPhase::idle:
    return "IDLE";
  case SenderControlPhase::connected:
    return "CONNECTED";
  case SenderControlPhase::pause_pending:
    return "PAUSE_PENDING";
  case SenderControlPhase::paused:
    return "PAUSED";
  case SenderControlPhase::resume_pending:
    return "RESUME_PENDING";
  case SenderControlPhase::end_pending:
    return "END_PENDING";
  case SenderControlPhase::ended:
    return "ENDED";
  case SenderControlPhase::failed:
    return "FAILED";
  }
  return "UNKNOWN";
}

} // namespace glyphrelay
