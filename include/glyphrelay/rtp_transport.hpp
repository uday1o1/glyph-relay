#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace glyphrelay {

constexpr std::size_t kMaximumRtpPayloadBytes = 1'200U;
constexpr std::size_t kRtpHeaderBytes = 12U;
constexpr std::uint64_t kRtpClockRate = 90'000U;

enum class H264PacketRole {
  sequence_parameter_set,
  picture_parameter_set,
  idr,
  inter,
  supplemental,
};

struct RtpPacketIdentity {
  std::uint64_t frame_id = 0;
  std::uint64_t media_epoch = 0;
  std::uint64_t access_unit_id = 0;
  std::uint64_t dependency_epoch = 0;
  std::uint32_t ssrc = 0;
  std::uint64_t extended_sequence = 0;
  std::uint16_t wire_sequence = 0;
  std::uint64_t extended_timestamp = 0;
  std::uint32_t wire_timestamp = 0;
  H264PacketRole role = H264PacketRole::supplemental;
  bool marker = false;
};

struct PlaintextRtpPacket {
  std::vector<std::uint8_t> bytes;
  std::size_t rtp_header_bytes = kRtpHeaderBytes;
  RtpPacketIdentity identity;
  bool retransmission = false;

  std::span<const std::uint8_t> payload() const;
};

struct PacketizeAccessUnit {
  std::span<const std::uint8_t> annex_b;
  std::uint64_t frame_id = 0;
  std::uint64_t media_epoch = 0;
  std::uint64_t access_unit_id = 0;
  std::uint64_t dependency_epoch = 0;
  std::uint64_t dequeue_monotonic_nanoseconds = 0;
};

struct PacketizationResult {
  bool passed = false;
  std::string reason;
  std::uint64_t extended_timestamp = 0;
  std::vector<PlaintextRtpPacket> packets;
};

class RtpClock90k {
public:
  RtpClock90k(std::uint64_t base_monotonic_nanoseconds, std::uint64_t base_extended_timestamp);

  std::optional<std::uint64_t> map(std::uint64_t monotonic_nanoseconds);
  std::uint64_t last_extended_timestamp() const;

private:
  std::uint64_t base_monotonic_nanoseconds_ = 0;
  std::uint64_t base_extended_timestamp_ = 0;
  std::optional<std::uint64_t> last_extended_timestamp_;
};

class H264RtpPacketizer {
public:
  H264RtpPacketizer(std::uint8_t payload_type, std::uint32_t ssrc,
                    std::uint64_t initial_extended_sequence,
                    std::uint64_t base_monotonic_nanoseconds,
                    std::uint64_t base_extended_timestamp);

  PacketizationResult packetize(const PacketizeAccessUnit &input);
  std::uint64_t next_extended_sequence() const;
  std::uint64_t last_extended_timestamp() const;

private:
  std::uint8_t payload_type_ = 0;
  std::uint32_t ssrc_ = 0;
  std::uint64_t next_extended_sequence_ = 0;
  RtpClock90k clock_;
};

enum class NackResolution {
  retransmit,
  missing,
  ambiguous,
  retransmission_limit,
  stale_epoch,
};

struct NackLookupResult {
  NackResolution resolution = NackResolution::missing;
  std::optional<PlaintextRtpPacket> packet;
};

struct RetransmissionCacheSnapshot {
  std::size_t packets = 0;
  std::size_t bytes = 0;
  std::uint64_t evicted_age = 0;
  std::uint64_t evicted_packet_limit = 0;
  std::uint64_t evicted_byte_limit = 0;
  std::uint64_t retransmissions = 0;
};

class RetransmissionCache {
public:
  static constexpr std::uint64_t kMaximumAgeMilliseconds = 500U;
  static constexpr std::size_t kMaximumPackets = 2'048U;
  static constexpr std::size_t kMaximumBytes = 4U * 1024U * 1024U;
  static constexpr unsigned int kMaximumRetransmissions = 2U;

  void reset_epoch(std::uint64_t media_epoch, std::uint64_t dependency_epoch);
  bool store(const PlaintextRtpPacket &packet, std::uint64_t now_milliseconds);
  NackLookupResult resolve(std::uint16_t wire_sequence, std::uint64_t media_epoch,
                           std::uint64_t dependency_epoch, std::uint64_t now_milliseconds);
  void clear();

  RetransmissionCacheSnapshot snapshot() const;
  std::uint64_t media_epoch() const;
  std::uint64_t dependency_epoch() const;

private:
  struct Entry {
    PlaintextRtpPacket packet;
    std::uint64_t stored_milliseconds = 0;
    unsigned int retransmissions = 0;
  };

  void validate_time(std::uint64_t now_milliseconds);
  void evict_expired(std::uint64_t now_milliseconds);
  void evict_oldest(bool packet_limit);

  std::deque<Entry> entries_;
  std::size_t bytes_ = 0;
  std::uint64_t media_epoch_ = 0;
  std::uint64_t dependency_epoch_ = 0;
  std::optional<std::uint64_t> last_now_milliseconds_;
  RetransmissionCacheSnapshot cumulative_;
};

struct GenericNackField {
  std::uint16_t packet_id = 0;
  std::uint16_t lost_packet_bitmask = 0;
};

std::vector<std::uint16_t> expand_generic_nack(std::span<const GenericNackField> fields);

enum class RecoveryTrigger {
  startup,
  resume,
  dependency_epoch_transition,
  receiver_admission,
  recorder_armed,
  picture_loss_indication,
  nack_miss,
};

struct RecoveryDiagnostics {
  std::uint64_t feedback_messages = 0;
  std::uint64_t distinct_nack_identifiers = 0;
  std::uint64_t ignored_feedback_messages = 0;
  std::uint64_t ignored_nack_identifiers = 0;
  std::uint64_t stale_feedback = 0;
  std::uint64_t idr_requests = 0;
  std::uint64_t coalesced_idr_requests = 0;
  bool feedback_flood_terminated = false;
};

struct RecoveryDecision {
  std::vector<PlaintextRtpPacket> retransmissions;
  bool request_idr_with_parameter_sets = false;
  bool feedback_rate_limited = false;
  bool terminate_session = false;
};

class RtpRecoveryController {
public:
  static constexpr std::size_t kMaximumDistinctNackIdentifiersPerSecond = 100U;
  static constexpr std::size_t kMaximumFeedbackMessagesPerSecond = 10U;
  static constexpr std::uint64_t kFeedbackWindowMilliseconds = 1'000U;
  static constexpr std::uint64_t kFeedbackFloodTerminationMilliseconds = 10'000U;

  explicit RtpRecoveryController(RetransmissionCache &cache);

  bool begin_epoch(std::uint64_t media_epoch, std::uint64_t dependency_epoch,
                   std::uint64_t now_milliseconds, RecoveryTrigger trigger);
  bool request_forced_idr(std::uint64_t now_milliseconds, RecoveryTrigger trigger);
  RecoveryDecision handle_pli(std::uint64_t media_epoch, std::uint64_t dependency_epoch,
                              std::uint64_t now_milliseconds);
  RecoveryDecision handle_nack(std::uint64_t media_epoch, std::uint64_t dependency_epoch,
                               std::span<const GenericNackField> fields,
                               std::uint64_t now_milliseconds);
  void stop();

  RecoveryDiagnostics diagnostics() const;

private:
  bool admit_feedback_message(std::uint64_t now_milliseconds);
  bool admit_nack_identifier(std::uint16_t identifier, std::uint64_t now_milliseconds);
  bool request_feedback_idr(std::uint64_t now_milliseconds, RecoveryTrigger trigger);
  void update_flood_state(bool exceeded, std::uint64_t now_milliseconds);
  bool terminated() const;
  void validate_time(std::uint64_t now_milliseconds);

  RetransmissionCache &cache_;
  std::uint64_t media_epoch_ = 0;
  std::uint64_t dependency_epoch_ = 0;
  std::optional<std::uint64_t> last_idr_request_milliseconds_;
  std::optional<std::uint64_t> last_now_milliseconds_;
  std::optional<std::uint64_t> overload_started_milliseconds_;
  std::deque<std::uint64_t> feedback_message_times_;
  std::unordered_map<std::uint16_t, std::uint64_t> nack_identifier_times_;
  RecoveryDiagnostics diagnostics_;
  bool stopped_ = false;
};

std::string nack_resolution_name(NackResolution resolution);
std::string recovery_trigger_name(RecoveryTrigger trigger);

} // namespace glyphrelay
