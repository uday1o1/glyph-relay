#pragma once

#include "glyphrelay/recording.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace glyphrelay {

struct EncodedTransportQueueConfig {
  std::size_t maximum_access_units = 3U;
  std::size_t maximum_bytes = 8U * 1024U * 1024U;
  std::uint64_t maximum_age_ns = 100'000'000ULL;
};

struct EncodedTransportQueueDiagnostics {
  bool admission_open = true;
  bool awaiting_recovery = true;
  bool unusable = false;
  std::size_t access_units = 0;
  std::size_t bytes = 0;
  std::size_t maximum_access_units = 0;
  std::size_t maximum_bytes = 0;
  std::uint64_t maximum_age_ns = 0;
  std::uint64_t active_media_epoch = 0;
  std::uint64_t active_dependency_epoch = 0;
  std::uint64_t accepted_access_units = 0;
  std::uint64_t dequeued_access_units = 0;
  std::uint64_t rejected_access_units = 0;
  std::uint64_t purged_access_units = 0;
  std::uint64_t purged_bytes = 0;
  std::uint64_t capacity_purges = 0;
  std::uint64_t age_purges = 0;
  std::uint64_t explicit_recovery_purges = 0;
  std::uint64_t recovery_requests = 0;
};

struct EncodedTransportAdmission {
  bool accepted = false;
  bool recovery_required = false;
  bool unusable = false;
  std::string reason;
};

struct EncodedTransportDequeue {
  std::optional<RecordedAccessUnit> access_unit;
  bool recovery_required = false;
  std::string reason;
};

class EncodedTransportQueue {
public:
  explicit EncodedTransportQueue(EncodedTransportQueueConfig config = {});

  EncodedTransportAdmission enqueue(const RecordedAccessUnit &access_unit, std::uint64_t now_ns);
  EncodedTransportDequeue dequeue(std::uint64_t now_ns);
  bool require_recovery();
  void stop();
  EncodedTransportQueueDiagnostics diagnostics() const;

private:
  struct Entry {
    RecordedAccessUnit access_unit;
    std::uint64_t enqueued_ns = 0;
  };

  bool validate_access_unit(const RecordedAccessUnit &access_unit) const;
  bool is_recovery(const RecordedAccessUnit &access_unit) const;
  bool validate_time(std::uint64_t now_ns);
  bool expire_if_needed(std::uint64_t now_ns);
  void purge(bool recovery_requested);
  EncodedTransportAdmission reject(std::string reason, bool recovery_required,
                                   bool unusable = false);

  const EncodedTransportQueueConfig config_;
  mutable std::mutex mutex_;
  std::deque<Entry> queue_;
  EncodedTransportQueueDiagnostics diagnostics_;
  std::optional<std::uint64_t> last_now_ns_;
  std::optional<std::uint64_t> epoch_floor_media_;
  std::optional<std::uint64_t> epoch_floor_dependency_;
};

struct EncodedFanoutResult {
  bool accepted_any = false;
  bool transport_accepted = false;
  bool recorder_accepted = false;
  bool transport_recovery_required = false;
  bool transport_unusable = false;
  bool recorder_failed = false;
  std::string transport_reason;
  std::string recorder_reason;
};

struct EncodedFanoutDiagnostics {
  bool transport_active = false;
  bool recorder_active = false;
  std::uint64_t published_access_units = 0;
  std::uint64_t transport_accepted_access_units = 0;
  std::uint64_t transport_rejected_access_units = 0;
  std::uint64_t recorder_accepted_access_units = 0;
  std::uint64_t recorder_rejected_access_units = 0;
  std::uint64_t recorder_failures = 0;
};

class EncodedAccessUnitFanout {
public:
  using RecorderAdmission = std::function<RecorderEnqueueResult(RecordedAccessUnit)>;

  EncodedAccessUnitFanout(EncodedTransportQueue *transport, RecorderAdmission recorder = {});

  EncodedFanoutResult publish(const RecordedAccessUnit &access_unit, std::uint64_t now_ns);
  void disable_transport();
  void disable_recorder();
  EncodedFanoutDiagnostics diagnostics() const;

private:
  mutable std::mutex mutex_;
  EncodedTransportQueue *transport_ = nullptr;
  RecorderAdmission recorder_;
  EncodedFanoutDiagnostics diagnostics_;
};

} // namespace glyphrelay
