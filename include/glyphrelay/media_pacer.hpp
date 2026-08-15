#pragma once

#include "glyphrelay/rtp_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace glyphrelay {

struct MediaPacerConfig {
  std::size_t maximum_bytes = 4U * 1024U * 1024U;
  std::uint64_t maximum_age_milliseconds = 100U;
};

struct MediaPacerAdmission {
  bool accepted = false;
  bool recovery_required = false;
  bool unusable_candidate = false;
  std::string reason;
};

struct MediaPacerDequeue {
  std::optional<PlaintextRtpPacket> packet;
  bool recovery_required = false;
  std::string reason;
};

struct MediaPacerSnapshot {
  bool admission_open = true;
  bool awaiting_recovery = true;
  bool stopped = false;
  std::size_t packets = 0;
  std::size_t bytes = 0;
  std::size_t maximum_bytes = 0;
  std::uint64_t maximum_age_milliseconds = 0;
  std::uint64_t active_media_epoch = 0;
  std::uint64_t active_dependency_epoch = 0;
  std::uint64_t abandoned_dependency_epoch = 0;
  std::uint64_t accepted_access_units = 0;
  std::uint64_t accepted_packets = 0;
  std::uint64_t dequeued_packets = 0;
  std::uint64_t purged_packets = 0;
  std::uint64_t purged_bytes = 0;
  std::uint64_t capacity_purges = 0;
  std::uint64_t age_purges = 0;
  std::uint64_t recovery_requests = 0;
  std::uint64_t rejected_access_units = 0;
  double target_bits_per_second = 0.0;
  double available_tokens_bytes = 0.0;
  double burst_capacity_bytes = 0.0;
  double oldest_packet_age_milliseconds = 0.0;
};

class MediaPacerQueue {
public:
  explicit MediaPacerQueue(RetransmissionCache &retransmission_cache, MediaPacerConfig config = {});

  void set_target_bits_per_second(double target_bits_per_second, std::uint64_t now_milliseconds);
  MediaPacerAdmission admit_access_unit(std::span<const PlaintextRtpPacket> packets,
                                        std::uint64_t now_milliseconds);
  MediaPacerAdmission admit_retransmission(const PlaintextRtpPacket &packet,
                                           std::uint64_t now_milliseconds);
  MediaPacerDequeue dequeue(std::uint64_t now_milliseconds);
  bool require_recovery();
  void stop();
  MediaPacerSnapshot snapshot(std::optional<std::uint64_t> now_milliseconds = std::nullopt) const;

private:
  struct Entry {
    PlaintextRtpPacket packet;
    std::uint64_t admitted_milliseconds = 0;
  };

  void validate_time(std::uint64_t now_milliseconds);
  void refill(std::uint64_t now_milliseconds);
  bool expire_if_needed(std::uint64_t now_milliseconds);
  void purge(bool capacity, bool age, bool recovery_required);
  bool is_recovery_access_unit(std::span<const PlaintextRtpPacket> packets) const;
  bool validate_batch_identity(std::span<const PlaintextRtpPacket> packets) const;
  MediaPacerAdmission reject(std::string reason, bool recovery_required,
                             bool unusable_candidate = false);

  RetransmissionCache &retransmission_cache_;
  const MediaPacerConfig config_;
  std::deque<Entry> queue_;
  MediaPacerSnapshot snapshot_;
  std::optional<std::uint64_t> last_now_milliseconds_;
  std::optional<std::uint64_t> token_timestamp_milliseconds_;
  double target_bits_per_second_ = 0.0;
  double tokens_bytes_ = 0.0;
  double burst_capacity_bytes_ = 0.0;
};

} // namespace glyphrelay
