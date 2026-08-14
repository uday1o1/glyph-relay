#include "glyphrelay/media_pacer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace glyphrelay {
namespace {

void saturating_increment(std::uint64_t &value, std::uint64_t increment = 1U) {
  if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
    value = std::numeric_limits<std::uint64_t>::max();
  } else {
    value += increment;
  }
}

} // namespace

MediaPacerQueue::MediaPacerQueue(RetransmissionCache &retransmission_cache, MediaPacerConfig config)
    : retransmission_cache_(retransmission_cache), config_(config) {
  if (config_.maximum_bytes == 0U || config_.maximum_bytes > 4U * 1024U * 1024U ||
      config_.maximum_age_milliseconds == 0U || config_.maximum_age_milliseconds > 100U) {
    throw std::invalid_argument("media pacer bounds exceed the frozen controller limits");
  }
  snapshot_.maximum_bytes = config_.maximum_bytes;
  snapshot_.maximum_age_milliseconds = config_.maximum_age_milliseconds;
}

void MediaPacerQueue::validate_time(std::uint64_t now_milliseconds) {
  if (last_now_milliseconds_ && now_milliseconds < *last_now_milliseconds_) {
    throw std::invalid_argument("media pacer time moved backward");
  }
  last_now_milliseconds_ = now_milliseconds;
}

void MediaPacerQueue::refill(std::uint64_t now_milliseconds) {
  if (!token_timestamp_milliseconds_) {
    token_timestamp_milliseconds_ = now_milliseconds;
    tokens_bytes_ = burst_capacity_bytes_;
    return;
  }
  const auto elapsed = now_milliseconds - *token_timestamp_milliseconds_;
  const double refill_bytes = target_bits_per_second_ * static_cast<double>(elapsed) / 8'000.0;
  tokens_bytes_ = std::min(burst_capacity_bytes_, tokens_bytes_ + refill_bytes);
  token_timestamp_milliseconds_ = now_milliseconds;
}

void MediaPacerQueue::set_target_bits_per_second(double target_bits_per_second,
                                                 std::uint64_t now_milliseconds) {
  validate_time(now_milliseconds);
  if (!std::isfinite(target_bits_per_second) || target_bits_per_second < 0.0) {
    throw std::invalid_argument("media pacer target must be finite and nonnegative");
  }
  const bool first_configuration = !token_timestamp_milliseconds_;
  refill(now_milliseconds);
  target_bits_per_second_ = target_bits_per_second;
  burst_capacity_bytes_ = target_bits_per_second_ / 80.0;
  if (first_configuration) {
    tokens_bytes_ = burst_capacity_bytes_;
  } else {
    tokens_bytes_ = std::min(tokens_bytes_, burst_capacity_bytes_);
  }
  snapshot_.target_bits_per_second = target_bits_per_second_;
  snapshot_.burst_capacity_bytes = burst_capacity_bytes_;
  snapshot_.available_tokens_bytes = tokens_bytes_;
}

bool MediaPacerQueue::validate_batch_identity(std::span<const PlaintextRtpPacket> packets) const {
  if (packets.empty()) {
    return false;
  }
  const auto &first = packets.front().identity;
  if (first.media_epoch == 0U || first.dependency_epoch == 0U || first.access_unit_id == 0U) {
    return false;
  }
  for (std::size_t index = 0; index < packets.size(); ++index) {
    const auto &packet = packets[index];
    if (packet.bytes.size() < packet.rtp_header_bytes || packet.payload().empty() ||
        packet.identity.media_epoch != first.media_epoch ||
        packet.identity.dependency_epoch != first.dependency_epoch ||
        packet.identity.access_unit_id != first.access_unit_id ||
        packet.identity.frame_id != first.frame_id ||
        packet.identity.extended_sequence != first.extended_sequence + index ||
        packet.identity.marker != (index + 1U == packets.size())) {
      return false;
    }
  }
  return true;
}

bool MediaPacerQueue::is_recovery_access_unit(std::span<const PlaintextRtpPacket> packets) const {
  bool saw_sps = false;
  bool saw_pps = false;
  bool saw_idr = false;
  for (const auto &packet : packets) {
    if (packet.identity.role == H264PacketRole::sequence_parameter_set) {
      saw_sps = true;
    } else if (packet.identity.role == H264PacketRole::picture_parameter_set) {
      saw_pps = saw_sps;
    } else if (packet.identity.role == H264PacketRole::idr) {
      saw_idr = saw_sps && saw_pps;
    }
  }
  return saw_idr;
}

MediaPacerAdmission MediaPacerQueue::reject(std::string reason, bool recovery_required,
                                            bool unusable_candidate) {
  saturating_increment(snapshot_.rejected_access_units);
  return {.accepted = false,
          .recovery_required = recovery_required,
          .unusable_candidate = unusable_candidate,
          .reason = std::move(reason)};
}

void MediaPacerQueue::purge(bool capacity, bool age, bool recovery_required) {
  saturating_increment(snapshot_.purged_packets, static_cast<std::uint64_t>(queue_.size()));
  saturating_increment(snapshot_.purged_bytes, static_cast<std::uint64_t>(snapshot_.bytes));
  if (capacity) {
    saturating_increment(snapshot_.capacity_purges);
  }
  if (age) {
    saturating_increment(snapshot_.age_purges);
  }
  if (recovery_required && !snapshot_.awaiting_recovery) {
    saturating_increment(snapshot_.recovery_requests);
  }
  if (snapshot_.active_dependency_epoch != 0U) {
    snapshot_.abandoned_dependency_epoch = snapshot_.active_dependency_epoch;
  }
  queue_.clear();
  snapshot_.packets = 0U;
  snapshot_.bytes = 0U;
  snapshot_.awaiting_recovery = recovery_required;
  snapshot_.admission_open = !snapshot_.stopped;
  retransmission_cache_.clear();
}

bool MediaPacerQueue::expire_if_needed(std::uint64_t now_milliseconds) {
  if (queue_.empty() ||
      now_milliseconds - queue_.front().admitted_milliseconds < config_.maximum_age_milliseconds) {
    return false;
  }
  purge(false, true, true);
  return true;
}

MediaPacerAdmission MediaPacerQueue::admit_access_unit(std::span<const PlaintextRtpPacket> packets,
                                                       std::uint64_t now_milliseconds) {
  validate_time(now_milliseconds);
  expire_if_needed(now_milliseconds);
  if (snapshot_.stopped || !snapshot_.admission_open) {
    return reject("PACER_STOPPED", false);
  }
  if (!validate_batch_identity(packets)) {
    return reject("PACER_ACCESS_UNIT_INVALID", snapshot_.awaiting_recovery);
  }
  const auto &identity = packets.front().identity;
  const bool recovery = is_recovery_access_unit(packets);
  if (snapshot_.awaiting_recovery && !recovery) {
    return reject("PACER_AWAITING_RECOVERY_IDR", true);
  }
  if (snapshot_.abandoned_dependency_epoch != 0U &&
      identity.dependency_epoch <= snapshot_.abandoned_dependency_epoch) {
    return reject("PACER_ABANDONED_DEPENDENCY_EPOCH", true);
  }
  if (!snapshot_.awaiting_recovery &&
      (identity.media_epoch != snapshot_.active_media_epoch ||
       identity.dependency_epoch != snapshot_.active_dependency_epoch)) {
    purge(false, false, true);
    if (!recovery || identity.dependency_epoch <= snapshot_.abandoned_dependency_epoch) {
      return reject("PACER_NEW_EPOCH_REQUIRES_RECOVERY_IDR", true);
    }
  }

  std::size_t batch_bytes = 0U;
  for (const auto &packet : packets) {
    if (batch_bytes > config_.maximum_bytes ||
        packet.bytes.size() > config_.maximum_bytes - batch_bytes) {
      return reject("PACER_RECOVERY_ACCESS_UNIT_TOO_LARGE", true, recovery);
    }
    batch_bytes += packet.bytes.size();
  }
  if (batch_bytes > config_.maximum_bytes) {
    return reject("PACER_RECOVERY_ACCESS_UNIT_TOO_LARGE", true, recovery);
  }
  if (batch_bytes > config_.maximum_bytes - snapshot_.bytes) {
    purge(true, false, true);
    return reject("PACER_CAPACITY_PURGED_DEPENDENCY_EPOCH", true);
  }

  if (snapshot_.awaiting_recovery) {
    snapshot_.active_media_epoch = identity.media_epoch;
    snapshot_.active_dependency_epoch = identity.dependency_epoch;
    snapshot_.awaiting_recovery = false;
    snapshot_.abandoned_dependency_epoch = 0U;
    retransmission_cache_.reset_epoch(identity.media_epoch, identity.dependency_epoch);
  }
  for (const auto &packet : packets) {
    queue_.push_back({packet, now_milliseconds});
  }
  snapshot_.packets = queue_.size();
  snapshot_.bytes += batch_bytes;
  saturating_increment(snapshot_.accepted_access_units);
  saturating_increment(snapshot_.accepted_packets, static_cast<std::uint64_t>(packets.size()));
  return {.accepted = true,
          .recovery_required = false,
          .unusable_candidate = false,
          .reason = "PACER_ACCESS_UNIT_ACCEPTED"};
}

MediaPacerAdmission MediaPacerQueue::admit_retransmission(const PlaintextRtpPacket &packet,
                                                          std::uint64_t now_milliseconds) {
  validate_time(now_milliseconds);
  expire_if_needed(now_milliseconds);
  if (snapshot_.stopped || snapshot_.awaiting_recovery ||
      packet.identity.media_epoch != snapshot_.active_media_epoch ||
      packet.identity.dependency_epoch != snapshot_.active_dependency_epoch) {
    return reject("PACER_RETRANSMISSION_EPOCH_REJECTED", snapshot_.awaiting_recovery);
  }
  if (packet.bytes.size() > config_.maximum_bytes - snapshot_.bytes) {
    purge(true, false, true);
    return reject("PACER_RETRANSMISSION_CAPACITY_PURGED_DEPENDENCY_EPOCH", true);
  }
  queue_.push_back({packet, now_milliseconds});
  snapshot_.packets = queue_.size();
  snapshot_.bytes += packet.bytes.size();
  saturating_increment(snapshot_.accepted_packets);
  return {.accepted = true,
          .recovery_required = false,
          .unusable_candidate = false,
          .reason = "PACER_RETRANSMISSION_ACCEPTED"};
}

MediaPacerDequeue MediaPacerQueue::dequeue(std::uint64_t now_milliseconds) {
  validate_time(now_milliseconds);
  refill(now_milliseconds);
  snapshot_.available_tokens_bytes = tokens_bytes_;
  if (expire_if_needed(now_milliseconds)) {
    return {.packet = std::nullopt,
            .recovery_required = true,
            .reason = "PACER_AGE_PURGED_DEPENDENCY_EPOCH"};
  }
  if (queue_.empty()) {
    return {.packet = std::nullopt,
            .recovery_required = snapshot_.awaiting_recovery,
            .reason = snapshot_.awaiting_recovery ? "PACER_AWAITING_RECOVERY_IDR" : "PACER_EMPTY"};
  }
  const double packet_bytes = static_cast<double>(queue_.front().packet.bytes.size());
  if (tokens_bytes_ < packet_bytes) {
    return {
        .packet = std::nullopt, .recovery_required = false, .reason = "PACER_TOKENS_UNAVAILABLE"};
  }
  tokens_bytes_ -= packet_bytes;
  auto packet = std::move(queue_.front().packet);
  queue_.pop_front();
  snapshot_.packets = queue_.size();
  snapshot_.bytes -= packet.bytes.size();
  snapshot_.available_tokens_bytes = tokens_bytes_;
  saturating_increment(snapshot_.dequeued_packets);
  return {
      .packet = std::move(packet), .recovery_required = false, .reason = "PACER_PACKET_DEQUEUED"};
}

bool MediaPacerQueue::require_recovery() {
  if (snapshot_.stopped) {
    return false;
  }
  if (snapshot_.awaiting_recovery && queue_.empty()) {
    return true;
  }
  purge(false, false, true);
  return true;
}

void MediaPacerQueue::stop() {
  if (snapshot_.stopped) {
    return;
  }
  purge(false, false, false);
  snapshot_.stopped = true;
  snapshot_.admission_open = false;
  snapshot_.awaiting_recovery = false;
}

MediaPacerSnapshot MediaPacerQueue::snapshot() const {
  auto result = snapshot_;
  result.target_bits_per_second = target_bits_per_second_;
  result.available_tokens_bytes = tokens_bytes_;
  result.burst_capacity_bytes = burst_capacity_bytes_;
  return result;
}

} // namespace glyphrelay
