#include "glyphrelay/rtp_transport.hpp"

#include "glyphrelay/annex_b.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace glyphrelay {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000U;

struct RtpPayload {
  std::vector<std::uint8_t> bytes;
  H264PacketRole role = H264PacketRole::supplemental;
};

void write_u16(std::span<std::uint8_t> output, std::size_t offset, std::uint16_t value) {
  output[offset] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 1U] = static_cast<std::uint8_t>(value & 0xFFU);
}

void write_u32(std::span<std::uint8_t> output, std::size_t offset, std::uint32_t value) {
  output[offset] = static_cast<std::uint8_t>(value >> 24U);
  output[offset + 1U] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  output[offset + 2U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  output[offset + 3U] = static_cast<std::uint8_t>(value & 0xFFU);
}

H264PacketRole packet_role(std::uint8_t nal_type) {
  switch (nal_type) {
  case 7U:
    return H264PacketRole::sequence_parameter_set;
  case 8U:
    return H264PacketRole::picture_parameter_set;
  case 5U:
    return H264PacketRole::idr;
  case 1U:
    return H264PacketRole::inter;
  default:
    return H264PacketRole::supplemental;
  }
}

bool idr_has_parameter_sets(const AnnexBAccessUnit &access_unit) {
  bool saw_sps = false;
  bool saw_pps_after_sps = false;
  bool saw_non_idr_vcl = false;
  for (const auto &unit : access_unit.nal_units) {
    if (unit.unit_type == 7U) {
      saw_sps = true;
    } else if (unit.unit_type == 8U) {
      saw_pps_after_sps = saw_sps;
    } else if (unit.unit_type == 5U) {
      return !saw_non_idr_vcl && saw_sps && saw_pps_after_sps;
    } else if (unit.unit_type >= 1U && unit.unit_type <= 5U) {
      saw_non_idr_vcl = true;
    }
  }
  return true;
}

std::vector<RtpPayload> fragment_access_unit(const AnnexBAccessUnit &access_unit) {
  std::vector<RtpPayload> payloads;
  for (const auto &unit : access_unit.nal_units) {
    const auto nal = access_unit.payload(unit);
    const auto role = packet_role(unit.unit_type);
    if (nal.size() <= kMaximumRtpPayloadBytes) {
      payloads.push_back({std::vector<std::uint8_t>(nal.begin(), nal.end()), role});
      continue;
    }

    const auto original_header = nal.front();
    const auto fu_indicator = static_cast<std::uint8_t>((original_header & 0xE0U) | 28U);
    const auto original_type = static_cast<std::uint8_t>(original_header & 0x1FU);
    constexpr std::size_t kFuHeaders = 2U;
    constexpr std::size_t kMaximumFuData = kMaximumRtpPayloadBytes - kFuHeaders;
    std::size_t offset = 1U;
    while (offset < nal.size()) {
      const auto remaining = nal.size() - offset;
      const auto chunk_size = std::min(remaining, kMaximumFuData);
      const bool start = offset == 1U;
      const bool end = chunk_size == remaining;
      std::vector<std::uint8_t> payload;
      payload.reserve(kFuHeaders + chunk_size);
      payload.push_back(fu_indicator);
      payload.push_back(
          static_cast<std::uint8_t>((start ? 0x80U : 0U) | (end ? 0x40U : 0U) | original_type));
      payload.insert(payload.end(), nal.begin() + static_cast<std::ptrdiff_t>(offset),
                     nal.begin() + static_cast<std::ptrdiff_t>(offset + chunk_size));
      payloads.push_back({std::move(payload), role});
      offset += chunk_size;
    }
  }
  return payloads;
}

PlaintextRtpPacket make_packet(const RtpPayload &payload, bool marker, std::uint8_t payload_type,
                               std::uint32_t ssrc, std::uint64_t extended_sequence,
                               std::uint64_t extended_timestamp, const PacketizeAccessUnit &input) {
  PlaintextRtpPacket packet;
  packet.bytes.assign(kRtpHeaderBytes + payload.bytes.size(), 0U);
  packet.bytes[0] = 0x80U;
  packet.bytes[1] = static_cast<std::uint8_t>((marker ? 0x80U : 0U) | payload_type);
  const auto wire_sequence = static_cast<std::uint16_t>(extended_sequence & 0xFFFFU);
  const auto wire_timestamp = static_cast<std::uint32_t>(extended_timestamp & 0xFFFFFFFFU);
  write_u16(packet.bytes, 2U, wire_sequence);
  write_u32(packet.bytes, 4U, wire_timestamp);
  write_u32(packet.bytes, 8U, ssrc);
  std::copy(payload.bytes.begin(), payload.bytes.end(),
            packet.bytes.begin() + static_cast<std::ptrdiff_t>(kRtpHeaderBytes));
  packet.identity = {
      .frame_id = input.frame_id,
      .media_epoch = input.media_epoch,
      .access_unit_id = input.access_unit_id,
      .dependency_epoch = input.dependency_epoch,
      .ssrc = ssrc,
      .extended_sequence = extended_sequence,
      .wire_sequence = wire_sequence,
      .extended_timestamp = extended_timestamp,
      .wire_timestamp = wire_timestamp,
      .role = payload.role,
      .marker = marker,
  };
  return packet;
}

void saturating_increment(std::uint64_t &value) {
  if (value != std::numeric_limits<std::uint64_t>::max()) {
    ++value;
  }
}

} // namespace

std::span<const std::uint8_t> PlaintextRtpPacket::payload() const {
  if (rtp_header_bytes < kRtpHeaderBytes || bytes.size() < rtp_header_bytes) {
    return {};
  }
  return std::span(bytes).subspan(rtp_header_bytes);
}

RtpClock90k::RtpClock90k(std::uint64_t base_monotonic_nanoseconds,
                         std::uint64_t base_extended_timestamp)
    : base_monotonic_nanoseconds_(base_monotonic_nanoseconds),
      base_extended_timestamp_(base_extended_timestamp) {}

std::optional<std::uint64_t> RtpClock90k::map(std::uint64_t monotonic_nanoseconds) {
  if (monotonic_nanoseconds < base_monotonic_nanoseconds_) {
    return std::nullopt;
  }
  const auto delta = monotonic_nanoseconds - base_monotonic_nanoseconds_;
  const auto whole_seconds = delta / kNanosecondsPerSecond;
  const auto remaining_nanoseconds = delta % kNanosecondsPerSecond;
  if (whole_seconds > std::numeric_limits<std::uint64_t>::max() / kRtpClockRate) {
    return std::nullopt;
  }
  const auto whole_ticks = whole_seconds * kRtpClockRate;
  const auto fractional_ticks = (remaining_nanoseconds * kRtpClockRate) / kNanosecondsPerSecond;
  if (fractional_ticks > std::numeric_limits<std::uint64_t>::max() - whole_ticks) {
    return std::nullopt;
  }
  const auto elapsed_ticks = whole_ticks + fractional_ticks;
  if (elapsed_ticks > std::numeric_limits<std::uint64_t>::max() - base_extended_timestamp_) {
    return std::nullopt;
  }
  const auto candidate = base_extended_timestamp_ + elapsed_ticks;
  if (last_extended_timestamp_ && candidate <= *last_extended_timestamp_) {
    return std::nullopt;
  }
  last_extended_timestamp_ = candidate;
  return candidate;
}

std::uint64_t RtpClock90k::last_extended_timestamp() const {
  return last_extended_timestamp_.value_or(base_extended_timestamp_);
}

H264RtpPacketizer::H264RtpPacketizer(std::uint8_t payload_type, std::uint32_t ssrc,
                                     std::uint64_t initial_extended_sequence,
                                     std::uint64_t base_monotonic_nanoseconds,
                                     std::uint64_t base_extended_timestamp)
    : payload_type_(payload_type), ssrc_(ssrc), next_extended_sequence_(initial_extended_sequence),
      clock_(base_monotonic_nanoseconds, base_extended_timestamp) {
  if (payload_type < 96U || payload_type > 127U) {
    throw std::invalid_argument("H.264 RTP payload type must be dynamic");
  }
  if (ssrc == 0U) {
    throw std::invalid_argument("RTP SSRC zero is reserved by GlyphRelay");
  }
}

PacketizationResult H264RtpPacketizer::packetize(const PacketizeAccessUnit &input) {
  PacketizationResult result;
  if (input.media_epoch == 0U || input.dependency_epoch == 0U || input.access_unit_id == 0U) {
    result.reason = "rtp_access_unit_identity_invalid";
    return result;
  }
  const auto parsed = parse_annex_b_access_unit(input.annex_b);
  if (!parsed.passed) {
    result.reason = "rtp_" + parsed.reason;
    return result;
  }
  if (!idr_has_parameter_sets(parsed.access_unit)) {
    result.reason = "rtp_idr_missing_sps_pps";
    return result;
  }
  auto payloads = fragment_access_unit(parsed.access_unit);
  if (payloads.empty()) {
    result.reason = "rtp_access_unit_has_no_payloads";
    return result;
  }
  const auto packet_count = static_cast<std::uint64_t>(payloads.size());
  if (packet_count > std::numeric_limits<std::uint64_t>::max() - next_extended_sequence_) {
    result.reason = "rtp_extended_sequence_exhausted";
    return result;
  }
  const auto extended_timestamp = clock_.map(input.dequeue_monotonic_nanoseconds);
  if (!extended_timestamp) {
    result.reason = "rtp_timestamp_not_strictly_increasing";
    return result;
  }

  result.packets.reserve(payloads.size());
  for (std::size_t index = 0; index < payloads.size(); ++index) {
    const bool marker = index + 1U == payloads.size();
    result.packets.push_back(make_packet(payloads[index], marker, payload_type_, ssrc_,
                                         next_extended_sequence_ + index, *extended_timestamp,
                                         input));
  }
  next_extended_sequence_ += packet_count;
  result.passed = true;
  result.reason = "rtp_access_unit_packetized";
  result.extended_timestamp = *extended_timestamp;
  return result;
}

std::uint64_t H264RtpPacketizer::next_extended_sequence() const { return next_extended_sequence_; }

std::uint64_t H264RtpPacketizer::last_extended_timestamp() const {
  return clock_.last_extended_timestamp();
}

void RetransmissionCache::reset_epoch(std::uint64_t media_epoch, std::uint64_t dependency_epoch) {
  if (media_epoch == 0U || dependency_epoch == 0U) {
    throw std::invalid_argument("retransmission cache epochs must be nonzero");
  }
  clear();
  media_epoch_ = media_epoch;
  dependency_epoch_ = dependency_epoch;
}

void RetransmissionCache::validate_time(std::uint64_t now_milliseconds) {
  if (last_now_milliseconds_ && now_milliseconds < *last_now_milliseconds_) {
    throw std::invalid_argument("retransmission cache time moved backward");
  }
  last_now_milliseconds_ = now_milliseconds;
}

void RetransmissionCache::evict_oldest(bool packet_limit) {
  if (entries_.empty()) {
    return;
  }
  bytes_ -= entries_.front().packet.bytes.size();
  entries_.pop_front();
  if (packet_limit) {
    saturating_increment(cumulative_.evicted_packet_limit);
  } else {
    saturating_increment(cumulative_.evicted_byte_limit);
  }
}

void RetransmissionCache::evict_expired(std::uint64_t now_milliseconds) {
  while (!entries_.empty() &&
         now_milliseconds - entries_.front().stored_milliseconds >= kMaximumAgeMilliseconds) {
    bytes_ -= entries_.front().packet.bytes.size();
    entries_.pop_front();
    saturating_increment(cumulative_.evicted_age);
  }
}

bool RetransmissionCache::store(const PlaintextRtpPacket &packet, std::uint64_t now_milliseconds) {
  validate_time(now_milliseconds);
  evict_expired(now_milliseconds);
  if (media_epoch_ == 0U || dependency_epoch_ == 0U ||
      packet.identity.media_epoch != media_epoch_ ||
      packet.identity.dependency_epoch != dependency_epoch_ ||
      packet.rtp_header_bytes < kRtpHeaderBytes || packet.bytes.size() < packet.rtp_header_bytes ||
      packet.payload().size() > kMaximumRtpPayloadBytes || packet.bytes.size() > kMaximumBytes) {
    return false;
  }
  const auto duplicate = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &entry) {
    return entry.packet.identity.ssrc == packet.identity.ssrc &&
           entry.packet.identity.extended_sequence == packet.identity.extended_sequence;
  });
  if (duplicate != entries_.end()) {
    return false;
  }
  while (entries_.size() >= kMaximumPackets) {
    evict_oldest(true);
  }
  while (!entries_.empty() && packet.bytes.size() > kMaximumBytes - bytes_) {
    evict_oldest(false);
  }
  entries_.push_back({packet, now_milliseconds, 0U});
  bytes_ += packet.bytes.size();
  return true;
}

NackLookupResult RetransmissionCache::resolve(std::uint16_t wire_sequence,
                                              std::uint64_t media_epoch,
                                              std::uint64_t dependency_epoch,
                                              std::uint64_t now_milliseconds) {
  validate_time(now_milliseconds);
  evict_expired(now_milliseconds);
  if (media_epoch != media_epoch_ || dependency_epoch != dependency_epoch_) {
    return {NackResolution::stale_epoch, std::nullopt};
  }
  Entry *match = nullptr;
  for (auto &entry : entries_) {
    if (entry.packet.identity.wire_sequence != wire_sequence) {
      continue;
    }
    if (match != nullptr) {
      return {NackResolution::ambiguous, std::nullopt};
    }
    match = &entry;
  }
  if (match == nullptr) {
    return {NackResolution::missing, std::nullopt};
  }
  if (match->retransmissions >= kMaximumRetransmissions) {
    return {NackResolution::retransmission_limit, std::nullopt};
  }
  ++match->retransmissions;
  saturating_increment(cumulative_.retransmissions);
  return {NackResolution::retransmit, match->packet};
}

void RetransmissionCache::clear() {
  entries_.clear();
  bytes_ = 0U;
  media_epoch_ = 0U;
  dependency_epoch_ = 0U;
  last_now_milliseconds_.reset();
}

RetransmissionCacheSnapshot RetransmissionCache::snapshot() const {
  auto snapshot = cumulative_;
  snapshot.packets = entries_.size();
  snapshot.bytes = bytes_;
  return snapshot;
}

std::uint64_t RetransmissionCache::media_epoch() const { return media_epoch_; }

std::uint64_t RetransmissionCache::dependency_epoch() const { return dependency_epoch_; }

std::vector<std::uint16_t> expand_generic_nack(std::span<const GenericNackField> fields) {
  std::vector<std::uint16_t> identifiers;
  std::unordered_set<std::uint16_t> seen;
  for (const auto &field : fields) {
    if (seen.insert(field.packet_id).second) {
      identifiers.push_back(field.packet_id);
    }
    for (unsigned int bit = 0U; bit < 16U; ++bit) {
      if ((field.lost_packet_bitmask & (1U << bit)) == 0U) {
        continue;
      }
      const auto identifier =
          static_cast<std::uint16_t>(field.packet_id + static_cast<std::uint16_t>(bit + 1U));
      if (seen.insert(identifier).second) {
        identifiers.push_back(identifier);
      }
    }
  }
  return identifiers;
}

RtpRecoveryController::RtpRecoveryController(RetransmissionCache &cache) : cache_(cache) {}

void RtpRecoveryController::validate_time(std::uint64_t now_milliseconds) {
  if (last_now_milliseconds_ && now_milliseconds < *last_now_milliseconds_) {
    throw std::invalid_argument("recovery feedback time moved backward");
  }
  last_now_milliseconds_ = now_milliseconds;
}

bool RtpRecoveryController::begin_epoch(std::uint64_t media_epoch, std::uint64_t dependency_epoch,
                                        std::uint64_t now_milliseconds, RecoveryTrigger trigger) {
  validate_time(now_milliseconds);
  if (terminated() || media_epoch == 0U || dependency_epoch == 0U) {
    return false;
  }
  const bool changed = media_epoch != media_epoch_ || dependency_epoch != dependency_epoch_;
  if (!changed) {
    return request_forced_idr(now_milliseconds, trigger);
  }
  media_epoch_ = media_epoch;
  dependency_epoch_ = dependency_epoch;
  cache_.reset_epoch(media_epoch, dependency_epoch);
  feedback_message_times_.clear();
  nack_identifier_times_.clear();
  overload_started_milliseconds_.reset();
  last_idr_request_milliseconds_ = now_milliseconds;
  saturating_increment(diagnostics_.idr_requests);
  return true;
}

bool RtpRecoveryController::request_forced_idr(std::uint64_t now_milliseconds,
                                               RecoveryTrigger trigger) {
  static_cast<void>(trigger);
  validate_time(now_milliseconds);
  if (terminated() || media_epoch_ == 0U || dependency_epoch_ == 0U) {
    return false;
  }
  if (last_idr_request_milliseconds_ &&
      now_milliseconds - *last_idr_request_milliseconds_ < kFeedbackWindowMilliseconds) {
    saturating_increment(diagnostics_.coalesced_idr_requests);
    return false;
  }
  last_idr_request_milliseconds_ = now_milliseconds;
  saturating_increment(diagnostics_.idr_requests);
  return true;
}

bool RtpRecoveryController::admit_feedback_message(std::uint64_t now_milliseconds) {
  while (!feedback_message_times_.empty() &&
         now_milliseconds - feedback_message_times_.front() >= kFeedbackWindowMilliseconds) {
    feedback_message_times_.pop_front();
  }
  feedback_message_times_.push_back(now_milliseconds);
  if (feedback_message_times_.size() > kMaximumFeedbackMessagesPerSecond + 1U) {
    feedback_message_times_.pop_front();
  }
  saturating_increment(diagnostics_.feedback_messages);
  if (feedback_message_times_.size() > kMaximumFeedbackMessagesPerSecond) {
    saturating_increment(diagnostics_.ignored_feedback_messages);
    update_flood_state(true, now_milliseconds);
    return false;
  }
  update_flood_state(false, now_milliseconds);
  return true;
}

bool RtpRecoveryController::admit_nack_identifier(std::uint16_t identifier,
                                                  std::uint64_t now_milliseconds) {
  for (auto iterator = nack_identifier_times_.begin(); iterator != nack_identifier_times_.end();) {
    if (now_milliseconds - iterator->second >= kFeedbackWindowMilliseconds) {
      iterator = nack_identifier_times_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  const auto existing = nack_identifier_times_.find(identifier);
  if (existing != nack_identifier_times_.end()) {
    existing->second = now_milliseconds;
    return true;
  }
  if (nack_identifier_times_.size() >= kMaximumDistinctNackIdentifiersPerSecond) {
    saturating_increment(diagnostics_.ignored_nack_identifiers);
    update_flood_state(true, now_milliseconds);
    return false;
  }
  nack_identifier_times_.emplace(identifier, now_milliseconds);
  saturating_increment(diagnostics_.distinct_nack_identifiers);
  return true;
}

void RtpRecoveryController::update_flood_state(bool exceeded, std::uint64_t now_milliseconds) {
  if (exceeded) {
    if (!overload_started_milliseconds_) {
      overload_started_milliseconds_ = now_milliseconds;
    }
    if (now_milliseconds - *overload_started_milliseconds_ >=
        kFeedbackFloodTerminationMilliseconds) {
      diagnostics_.feedback_flood_terminated = true;
      stopped_ = true;
      cache_.clear();
    }
  } else if (feedback_message_times_.size() < kMaximumFeedbackMessagesPerSecond &&
             nack_identifier_times_.size() < kMaximumDistinctNackIdentifiersPerSecond) {
    overload_started_milliseconds_.reset();
  }
}

bool RtpRecoveryController::request_feedback_idr(std::uint64_t now_milliseconds,
                                                 RecoveryTrigger trigger) {
  return request_forced_idr(now_milliseconds, trigger);
}

bool RtpRecoveryController::terminated() const {
  return stopped_ || diagnostics_.feedback_flood_terminated;
}

RecoveryDecision RtpRecoveryController::handle_pli(std::uint64_t media_epoch,
                                                   std::uint64_t dependency_epoch,
                                                   std::uint64_t now_milliseconds) {
  validate_time(now_milliseconds);
  RecoveryDecision decision;
  if (terminated()) {
    decision.terminate_session = diagnostics_.feedback_flood_terminated;
    return decision;
  }
  if (!admit_feedback_message(now_milliseconds)) {
    decision.feedback_rate_limited = true;
    decision.terminate_session = diagnostics_.feedback_flood_terminated;
    return decision;
  }
  if (media_epoch != media_epoch_ || dependency_epoch != dependency_epoch_) {
    saturating_increment(diagnostics_.stale_feedback);
    return decision;
  }
  decision.request_idr_with_parameter_sets =
      request_feedback_idr(now_milliseconds, RecoveryTrigger::picture_loss_indication);
  return decision;
}

RecoveryDecision RtpRecoveryController::handle_nack(std::uint64_t media_epoch,
                                                    std::uint64_t dependency_epoch,
                                                    std::span<const GenericNackField> fields,
                                                    std::uint64_t now_milliseconds) {
  validate_time(now_milliseconds);
  RecoveryDecision decision;
  if (terminated()) {
    decision.terminate_session = diagnostics_.feedback_flood_terminated;
    return decision;
  }
  if (!admit_feedback_message(now_milliseconds)) {
    decision.feedback_rate_limited = true;
    decision.terminate_session = diagnostics_.feedback_flood_terminated;
    return decision;
  }
  if (media_epoch != media_epoch_ || dependency_epoch != dependency_epoch_) {
    saturating_increment(diagnostics_.stale_feedback);
    return decision;
  }

  bool recovery_needed = false;
  for (const auto identifier : expand_generic_nack(fields)) {
    if (!admit_nack_identifier(identifier, now_milliseconds)) {
      decision.feedback_rate_limited = true;
      continue;
    }
    const auto lookup = cache_.resolve(identifier, media_epoch, dependency_epoch, now_milliseconds);
    if (lookup.resolution == NackResolution::retransmit && lookup.packet) {
      decision.retransmissions.push_back(*lookup.packet);
    } else if (lookup.resolution != NackResolution::stale_epoch) {
      recovery_needed = true;
    }
  }
  if (recovery_needed) {
    decision.request_idr_with_parameter_sets =
        request_feedback_idr(now_milliseconds, RecoveryTrigger::nack_miss);
  }
  decision.terminate_session = diagnostics_.feedback_flood_terminated;
  return decision;
}

void RtpRecoveryController::stop() {
  stopped_ = true;
  cache_.clear();
  feedback_message_times_.clear();
  nack_identifier_times_.clear();
}

RecoveryDiagnostics RtpRecoveryController::diagnostics() const { return diagnostics_; }

std::string nack_resolution_name(NackResolution resolution) {
  switch (resolution) {
  case NackResolution::retransmit:
    return "retransmit";
  case NackResolution::missing:
    return "missing";
  case NackResolution::ambiguous:
    return "ambiguous";
  case NackResolution::retransmission_limit:
    return "retransmission_limit";
  case NackResolution::stale_epoch:
    return "stale_epoch";
  }
  return "unknown";
}

std::string recovery_trigger_name(RecoveryTrigger trigger) {
  switch (trigger) {
  case RecoveryTrigger::startup:
    return "startup";
  case RecoveryTrigger::resume:
    return "resume";
  case RecoveryTrigger::dependency_epoch_transition:
    return "dependency_epoch_transition";
  case RecoveryTrigger::receiver_admission:
    return "receiver_admission";
  case RecoveryTrigger::recorder_armed:
    return "recorder_armed";
  case RecoveryTrigger::picture_loss_indication:
    return "picture_loss_indication";
  case RecoveryTrigger::nack_miss:
    return "nack_miss";
  }
  return "unknown";
}

} // namespace glyphrelay
