#include "glyphrelay/encoded_fanout.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace glyphrelay {
namespace {

void saturating_increment(std::uint64_t &value) {
  if (value != std::numeric_limits<std::uint64_t>::max()) {
    ++value;
  }
}

void saturating_add(std::uint64_t &value, std::size_t amount) {
  const auto converted = static_cast<std::uint64_t>(amount);
  if (converted > std::numeric_limits<std::uint64_t>::max() - value) {
    value = std::numeric_limits<std::uint64_t>::max();
  } else {
    value += converted;
  }
}

bool valid_hex64(const std::string &value) {
  if (value.size() != 64U) {
    return false;
  }
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

} // namespace

EncodedTransportQueue::EncodedTransportQueue(EncodedTransportQueueConfig config) : config_(config) {
  if (config_.maximum_access_units < 1U || config_.maximum_access_units > 64U ||
      config_.maximum_bytes < 1U || config_.maximum_bytes > 64U * 1024U * 1024U ||
      config_.maximum_age_ns < 1U || config_.maximum_age_ns > 2'000'000'000ULL) {
    throw std::invalid_argument("encoded transport queue bounds invalid");
  }
  diagnostics_.maximum_access_units = config_.maximum_access_units;
  diagnostics_.maximum_bytes = config_.maximum_bytes;
  diagnostics_.maximum_age_ns = config_.maximum_age_ns;
}

bool EncodedTransportQueue::validate_access_unit(const RecordedAccessUnit &access_unit) const {
  return access_unit.bytes && !access_unit.bytes->empty() && access_unit.media_epoch != 0U &&
         access_unit.dependency_epoch != 0U && access_unit.geometry_epoch != 0U &&
         access_unit.encoder_configuration_epoch != 0U && access_unit.source_frame_id != 0U &&
         access_unit.extended_rtp_timestamp != 0U && access_unit.presentation_timestamp_ns != 0U &&
         valid_hex64(access_unit.configuration_sha256) &&
         access_unit.parameter_sets_present == access_unit.keyframe &&
         (access_unit.picture_type == RecordingPictureType::idr ||
          access_unit.picture_type == RecordingPictureType::predicted) &&
         (access_unit.picture_type == RecordingPictureType::idr) == access_unit.keyframe;
}

bool EncodedTransportQueue::is_recovery(const RecordedAccessUnit &access_unit) const {
  return access_unit.keyframe && access_unit.parameter_sets_present &&
         access_unit.picture_type == RecordingPictureType::idr;
}

bool EncodedTransportQueue::validate_time(std::uint64_t now_ns) {
  if (last_now_ns_ && now_ns < *last_now_ns_) {
    diagnostics_.admission_open = false;
    diagnostics_.unusable = true;
    diagnostics_.awaiting_recovery = true;
    purge(false);
    return false;
  }
  last_now_ns_ = now_ns;
  return true;
}

void EncodedTransportQueue::purge(bool recovery_requested) {
  saturating_add(diagnostics_.purged_access_units, queue_.size());
  saturating_add(diagnostics_.purged_bytes, diagnostics_.bytes);
  queue_.clear();
  diagnostics_.access_units = 0U;
  diagnostics_.bytes = 0U;
  diagnostics_.active_media_epoch = 0U;
  diagnostics_.active_dependency_epoch = 0U;
  diagnostics_.awaiting_recovery = true;
  if (recovery_requested) {
    saturating_increment(diagnostics_.recovery_requests);
  }
}

bool EncodedTransportQueue::expire_if_needed(std::uint64_t now_ns) {
  if (queue_.empty() || now_ns - queue_.front().enqueued_ns < config_.maximum_age_ns) {
    return false;
  }
  saturating_increment(diagnostics_.age_purges);
  purge(true);
  return true;
}

EncodedTransportAdmission EncodedTransportQueue::reject(std::string reason, bool recovery_required,
                                                        bool unusable) {
  saturating_increment(diagnostics_.rejected_access_units);
  return {
      .accepted = false,
      .recovery_required = recovery_required,
      .unusable = unusable,
      .reason = std::move(reason),
  };
}

EncodedTransportAdmission EncodedTransportQueue::enqueue(const RecordedAccessUnit &access_unit,
                                                         std::uint64_t now_ns) {
  std::scoped_lock lock(mutex_);
  if (!diagnostics_.admission_open) {
    return reject(diagnostics_.unusable ? "TRANSPORT_QUEUE_UNUSABLE" : "TRANSPORT_QUEUE_STOPPED",
                  diagnostics_.awaiting_recovery, diagnostics_.unusable);
  }
  if (!validate_time(now_ns)) {
    return reject("TRANSPORT_TIME_REGRESSED", true, true);
  }
  const bool expired = expire_if_needed(now_ns);
  if (!validate_access_unit(access_unit)) {
    return reject("TRANSPORT_ACCESS_UNIT_INVALID", diagnostics_.awaiting_recovery,
                  diagnostics_.unusable);
  }
  const bool recovery = is_recovery(access_unit);
  if (epoch_floor_media_ &&
      (access_unit.media_epoch < *epoch_floor_media_ ||
       (access_unit.media_epoch == *epoch_floor_media_ && epoch_floor_dependency_ &&
        access_unit.dependency_epoch < *epoch_floor_dependency_))) {
    return reject("TRANSPORT_STALE_EPOCH", diagnostics_.awaiting_recovery);
  }
  const bool epoch_changed =
      epoch_floor_media_ &&
      (access_unit.media_epoch > *epoch_floor_media_ ||
       (access_unit.media_epoch == *epoch_floor_media_ && epoch_floor_dependency_ &&
        access_unit.dependency_epoch > *epoch_floor_dependency_));
  if (epoch_changed) {
    purge(!recovery);
    epoch_floor_media_ = access_unit.media_epoch;
    epoch_floor_dependency_ = access_unit.dependency_epoch;
  }
  if (diagnostics_.awaiting_recovery && !recovery) {
    return reject(expired ? "TRANSPORT_QUEUE_AGE_PURGED" : "TRANSPORT_AWAITING_RECOVERY_IDR", true);
  }
  if (access_unit.bytes->size() > config_.maximum_bytes) {
    if (recovery) {
      diagnostics_.admission_open = false;
      diagnostics_.unusable = true;
      purge(false);
      return reject("TRANSPORT_RECOVERY_ACCESS_UNIT_TOO_LARGE", true, true);
    }
    saturating_increment(diagnostics_.capacity_purges);
    purge(true);
    return reject("TRANSPORT_QUEUE_CAPACITY_PURGED", true);
  }
  const bool count_full = queue_.size() >= config_.maximum_access_units;
  const bool bytes_full = access_unit.bytes->size() > config_.maximum_bytes - diagnostics_.bytes;
  if (count_full || bytes_full) {
    saturating_increment(diagnostics_.capacity_purges);
    purge(true);
    return reject("TRANSPORT_QUEUE_CAPACITY_PURGED", true);
  }
  if (diagnostics_.awaiting_recovery) {
    diagnostics_.awaiting_recovery = false;
    diagnostics_.active_media_epoch = access_unit.media_epoch;
    diagnostics_.active_dependency_epoch = access_unit.dependency_epoch;
    epoch_floor_media_ = access_unit.media_epoch;
    epoch_floor_dependency_ = access_unit.dependency_epoch;
  }
  queue_.push_back({access_unit, now_ns});
  diagnostics_.access_units = queue_.size();
  diagnostics_.bytes += access_unit.bytes->size();
  saturating_increment(diagnostics_.accepted_access_units);
  return {
      .accepted = true,
      .recovery_required = false,
      .unusable = false,
      .reason = "TRANSPORT_ACCESS_UNIT_ACCEPTED",
  };
}

EncodedTransportDequeue EncodedTransportQueue::dequeue(std::uint64_t now_ns) {
  std::scoped_lock lock(mutex_);
  if (!diagnostics_.admission_open) {
    return {
        .access_unit = std::nullopt,
        .recovery_required = diagnostics_.awaiting_recovery,
        .reason = diagnostics_.unusable ? "TRANSPORT_QUEUE_UNUSABLE" : "TRANSPORT_QUEUE_STOPPED",
    };
  }
  if (!validate_time(now_ns)) {
    return {.access_unit = std::nullopt,
            .recovery_required = true,
            .reason = "TRANSPORT_TIME_REGRESSED"};
  }
  if (expire_if_needed(now_ns)) {
    return {.access_unit = std::nullopt,
            .recovery_required = true,
            .reason = "TRANSPORT_QUEUE_AGE_PURGED"};
  }
  if (queue_.empty()) {
    return {.access_unit = std::nullopt,
            .recovery_required = diagnostics_.awaiting_recovery,
            .reason = "TRANSPORT_QUEUE_EMPTY"};
  }
  auto access_unit = std::move(queue_.front().access_unit);
  queue_.pop_front();
  diagnostics_.access_units = queue_.size();
  diagnostics_.bytes -= access_unit.bytes->size();
  saturating_increment(diagnostics_.dequeued_access_units);
  return {.access_unit = std::move(access_unit),
          .recovery_required = false,
          .reason = "TRANSPORT_ACCESS_UNIT_DEQUEUED"};
}

bool EncodedTransportQueue::require_recovery() {
  std::scoped_lock lock(mutex_);
  if (!diagnostics_.admission_open) {
    return false;
  }
  if (diagnostics_.awaiting_recovery && queue_.empty()) {
    return true;
  }
  saturating_increment(diagnostics_.explicit_recovery_purges);
  purge(true);
  return true;
}

void EncodedTransportQueue::stop() {
  std::scoped_lock lock(mutex_);
  if (!diagnostics_.admission_open) {
    return;
  }
  diagnostics_.admission_open = false;
  purge(false);
}

EncodedTransportQueueDiagnostics EncodedTransportQueue::diagnostics() const {
  std::scoped_lock lock(mutex_);
  return diagnostics_;
}

EncodedAccessUnitFanout::EncodedAccessUnitFanout(EncodedTransportQueue *transport,
                                                 RecorderAdmission recorder)
    : transport_(transport), recorder_(std::move(recorder)) {
  if (transport_ == nullptr && !recorder_) {
    throw std::invalid_argument("encoded fanout requires at least one branch");
  }
  diagnostics_.transport_active = transport_ != nullptr;
  diagnostics_.recorder_active = static_cast<bool>(recorder_);
}

EncodedFanoutResult EncodedAccessUnitFanout::publish(const RecordedAccessUnit &access_unit,
                                                     std::uint64_t now_ns) {
  std::scoped_lock lock(mutex_);
  EncodedFanoutResult result;
  saturating_increment(diagnostics_.published_access_units);
  if (diagnostics_.transport_active && transport_ != nullptr) {
    const auto admission = transport_->enqueue(access_unit, now_ns);
    result.transport_accepted = admission.accepted;
    result.transport_recovery_required = admission.recovery_required;
    result.transport_unusable = admission.unusable;
    result.transport_reason = admission.reason;
    if (admission.accepted) {
      saturating_increment(diagnostics_.transport_accepted_access_units);
    } else {
      saturating_increment(diagnostics_.transport_rejected_access_units);
    }
    if (admission.unusable) {
      diagnostics_.transport_active = false;
    }
  }
  if (diagnostics_.recorder_active && recorder_) {
    try {
      const auto admission = recorder_(access_unit);
      result.recorder_accepted = admission.accepted;
      result.recorder_failed = admission.failed;
      result.recorder_reason = admission.reason;
      if (admission.accepted) {
        saturating_increment(diagnostics_.recorder_accepted_access_units);
      } else {
        saturating_increment(diagnostics_.recorder_rejected_access_units);
      }
      if (admission.failed) {
        diagnostics_.recorder_active = false;
        saturating_increment(diagnostics_.recorder_failures);
      }
    } catch (...) {
      result.recorder_failed = true;
      result.recorder_reason = "RECORDER_BRANCH_EXCEPTION";
      saturating_increment(diagnostics_.recorder_rejected_access_units);
      diagnostics_.recorder_active = false;
      saturating_increment(diagnostics_.recorder_failures);
    }
  }
  result.accepted_any = result.transport_accepted || result.recorder_accepted;
  return result;
}

void EncodedAccessUnitFanout::disable_transport() {
  std::scoped_lock lock(mutex_);
  if (diagnostics_.transport_active && transport_ != nullptr) {
    transport_->stop();
  }
  diagnostics_.transport_active = false;
}

void EncodedAccessUnitFanout::disable_recorder() {
  std::scoped_lock lock(mutex_);
  diagnostics_.recorder_active = false;
  recorder_ = {};
}

EncodedFanoutDiagnostics EncodedAccessUnitFanout::diagnostics() const {
  std::scoped_lock lock(mutex_);
  return diagnostics_;
}

} // namespace glyphrelay
