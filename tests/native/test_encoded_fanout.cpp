#include "glyphrelay/encoded_fanout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

glyphrelay::RecordedAccessUnit access_unit(std::uint64_t source_frame_id, bool recovery,
                                           std::uint64_t media_epoch = 1U,
                                           std::uint64_t dependency_epoch = 1U,
                                           std::size_t bytes = 16U) {
  auto payload = std::make_shared<const std::vector<std::uint8_t>>(bytes, recovery ? 0x65U : 0x41U);
  return {
      .bytes = std::move(payload),
      .media_epoch = media_epoch,
      .dependency_epoch = dependency_epoch,
      .geometry_epoch = 1U,
      .encoder_configuration_epoch = 1U,
      .configuration_sha256 = std::string(64U, 'a'),
      .source_frame_id = source_frame_id,
      .extended_rtp_timestamp = 90'000U + source_frame_id * 3'000U,
      .picture_type = recovery ? glyphrelay::RecordingPictureType::idr
                               : glyphrelay::RecordingPictureType::predicted,
      .keyframe = recovery,
      .parameter_sets_present = recovery,
      .presentation_timestamp_ns = source_frame_id * 33'333'333U,
  };
}

void test_configuration_and_startup_recovery() {
  bool rejected_zero_bound = false;
  try {
    glyphrelay::EncodedTransportQueue invalid({.maximum_access_units = 0U});
    static_cast<void>(invalid);
  } catch (const std::invalid_argument &) {
    rejected_zero_bound = true;
  }
  require(rejected_zero_bound, "zero queue capacity must fail at construction");

  glyphrelay::EncodedTransportQueue queue;
  const auto initial = queue.diagnostics();
  require(initial.admission_open && initial.awaiting_recovery &&
              initial.maximum_access_units == 3U && initial.maximum_bytes == 8U * 1024U * 1024U &&
              initial.maximum_age_ns == 100'000'000ULL,
          "transport diagnostics must publish every hard queue bound");
  const auto predicted = queue.enqueue(access_unit(1U, false), 10U);
  require(!predicted.accepted && predicted.recovery_required &&
              predicted.reason == "TRANSPORT_AWAITING_RECOVERY_IDR",
          "transport admission must begin behind a recovery IDR barrier");
  const auto recovery = queue.enqueue(access_unit(2U, true), 20U);
  require(recovery.accepted && !recovery.recovery_required,
          "an IDR carrying parameter sets must open initial admission");

  auto invalid_hash = access_unit(3U, false);
  invalid_hash.configuration_sha256.front() = 'G';
  require(queue.enqueue(invalid_hash, 21U).reason == "TRANSPORT_ACCESS_UNIT_INVALID",
          "transport admission must reject a non-canonical configuration digest");
  auto missing_parameter_sets = access_unit(4U, true);
  missing_parameter_sets.parameter_sets_present = false;
  require(queue.enqueue(missing_parameter_sets, 22U).reason == "TRANSPORT_ACCESS_UNIT_INVALID",
          "transport admission must reject an IDR without its required parameter sets");
}

void test_fifo_identity_and_capacity_epoch_purge() {
  glyphrelay::EncodedTransportQueue queue;
  const auto recovery = access_unit(1U, true);
  const auto predicted_a = access_unit(2U, false);
  const auto predicted_b = access_unit(3U, false);
  require(queue.enqueue(recovery, 0U).accepted && queue.enqueue(predicted_a, 1U).accepted &&
              queue.enqueue(predicted_b, 2U).accepted,
          "three access units must fill the bounded transport queue");

  const auto overflow = queue.enqueue(access_unit(4U, false), 3U);
  const auto purged = queue.diagnostics();
  require(!overflow.accepted && overflow.recovery_required &&
              overflow.reason == "TRANSPORT_QUEUE_CAPACITY_PURGED" && purged.access_units == 0U &&
              purged.bytes == 0U && purged.purged_access_units == 3U &&
              purged.capacity_purges == 1U && purged.recovery_requests == 1U,
          "overflow must purge the whole dependency epoch instead of one predicted frame");
  require(!queue.enqueue(access_unit(5U, false), 4U).accepted,
          "predicted frames must remain fenced after an overload purge");

  const auto next_recovery = access_unit(6U, true, 1U, 2U);
  require(queue.enqueue(next_recovery, 5U).accepted,
          "a newer recovery epoch must reopen admission");
  const auto dequeued = queue.dequeue(6U);
  require(dequeued.access_unit && dequeued.access_unit->bytes.get() == next_recovery.bytes.get() &&
              dequeued.access_unit->source_frame_id == 6U,
          "dequeue must preserve FIFO metadata and the immutable shared allocation");
}

void test_age_boundary_and_explicit_recovery_coalescing() {
  glyphrelay::EncodedTransportQueue queue;
  require(queue.enqueue(access_unit(1U, true), 1U).accepted,
          "age fixture recovery must be admitted");
  require(queue.dequeue(100'000'000U).access_unit.has_value(),
          "a queued access unit must remain valid one nanosecond before its age limit");

  require(queue.enqueue(access_unit(2U, false), 100'000'001U).accepted,
          "same-epoch predicted access must remain valid after dequeue");
  const auto expired = queue.dequeue(200'000'001U);
  require(!expired.access_unit && expired.recovery_required &&
              expired.reason == "TRANSPORT_QUEUE_AGE_PURGED",
          "the exact 100 millisecond boundary must purge the entire epoch");
  const auto after_expiry = queue.diagnostics();
  require(after_expiry.age_purges == 1U && after_expiry.recovery_requests == 1U,
          "age purge diagnostics must request one recovery");

  require(queue.require_recovery() && queue.require_recovery(),
          "duplicate recovery requests must be safe and idempotent");
  const auto coalesced = queue.diagnostics();
  require(coalesced.explicit_recovery_purges == 0U && coalesced.recovery_requests == 1U,
          "an already-pending empty recovery state must coalesce duplicate requests");

  require(queue.enqueue(access_unit(3U, true, 1U, 2U), 200'000'002U).accepted,
          "a recovery frame must reopen the age-purged queue");
  require(queue.require_recovery(), "an active explicit recovery must purge the queue");
  const auto explicit_purge = queue.diagnostics();
  require(explicit_purge.explicit_recovery_purges == 1U && explicit_purge.recovery_requests == 2U &&
              explicit_purge.access_units == 0U,
          "one active explicit recovery must purge once and increment its counters once");
}

void test_epoch_fencing_and_fail_closed_states() {
  glyphrelay::EncodedTransportQueue queue;
  require(queue.enqueue(access_unit(1U, true, 3U, 4U), 10U).accepted &&
              queue.enqueue(access_unit(2U, false, 3U, 4U), 11U).accepted,
          "epoch fixture must start with an active dependency chain");
  const auto future_predicted = queue.enqueue(access_unit(3U, false, 3U, 5U), 12U);
  require(!future_predicted.accepted && future_predicted.recovery_required &&
              queue.diagnostics().access_units == 0U,
          "a new dependency epoch must purge its predecessor and require recovery");
  const auto stale_recovery = queue.enqueue(access_unit(4U, true, 3U, 4U), 13U);
  require(!stale_recovery.accepted && stale_recovery.reason == "TRANSPORT_STALE_EPOCH",
          "a rejected future epoch must still fence stale recovery frames");
  require(queue.enqueue(access_unit(5U, true, 3U, 5U), 14U).accepted,
          "the exact pending epoch recovery must reopen admission");

  const auto regressed = queue.dequeue(13U);
  const auto unusable = queue.diagnostics();
  require(!regressed.access_unit && regressed.reason == "TRANSPORT_TIME_REGRESSED" &&
              unusable.unusable && !unusable.admission_open && unusable.access_units == 0U,
          "monotonic time regression must purge and permanently fail the transport branch");

  glyphrelay::EncodedTransportQueue too_small(
      {.maximum_access_units = 3U, .maximum_bytes = 8U, .maximum_age_ns = 100U});
  const auto oversized = too_small.enqueue(access_unit(1U, true, 1U, 1U, 9U), 1U);
  require(!oversized.accepted && oversized.unusable &&
              oversized.reason == "TRANSPORT_RECOVERY_ACCESS_UNIT_TOO_LARGE" &&
              too_small.diagnostics().unusable,
          "a recovery frame that can never fit must make only the transport branch unusable");
}

void test_stop_cleanup() {
  glyphrelay::EncodedTransportQueue queue;
  require(queue.enqueue(access_unit(1U, true), 1U).accepted,
          "stop fixture must own a queued immutable frame");
  queue.stop();
  queue.stop();
  const auto stopped = queue.diagnostics();
  require(!stopped.admission_open && !stopped.unusable && stopped.access_units == 0U &&
              stopped.bytes == 0U && stopped.purged_access_units == 1U,
          "idempotent stop must release the entire queued dependency epoch exactly once");
  const auto rejected = queue.enqueue(access_unit(2U, true), 2U);
  require(!rejected.accepted && rejected.reason == "TRANSPORT_QUEUE_STOPPED",
          "stopped queues must never reopen admission");
}

void test_fanout_branch_identity_and_isolation() {
  glyphrelay::EncodedTransportQueue queue;
  std::vector<glyphrelay::RecordedAccessUnit> recorded;
  glyphrelay::EncodedAccessUnitFanout fanout(
      &queue, [&recorded](glyphrelay::RecordedAccessUnit unit) {
        recorded.push_back(std::move(unit));
        return glyphrelay::RecorderEnqueueResult{
            .accepted = true, .failed = false, .reason = "RECORDER_ACCESS_UNIT_ACCEPTED"};
      });
  const auto recovery = access_unit(1U, true);
  const auto published = fanout.publish(recovery, 1U);
  require(published.transport_accepted && published.recorder_accepted && recorded.size() == 1U &&
              recorded.front().bytes.get() == recovery.bytes.get(),
          "fanout must share one immutable allocation with recorder and transport branches");
  const auto dequeued = queue.dequeue(2U);
  require(dequeued.access_unit && dequeued.access_unit->bytes.get() == recovery.bytes.get(),
          "the transport branch must retain the same immutable encoded allocation");

  std::size_t recorder_calls = 0U;
  glyphrelay::EncodedTransportQueue isolated_queue;
  glyphrelay::EncodedAccessUnitFanout recorder_failure(
      &isolated_queue, [&recorder_calls](glyphrelay::RecordedAccessUnit) {
        ++recorder_calls;
        return glyphrelay::RecorderEnqueueResult{
            .accepted = false, .failed = true, .reason = "RECORDER_QUEUE_OVERLOADED"};
      });
  const auto first = recorder_failure.publish(access_unit(1U, true), 1U);
  const auto second = recorder_failure.publish(access_unit(2U, false), 2U);
  const auto recorder_diagnostics = recorder_failure.diagnostics();
  require(first.transport_accepted && first.recorder_failed && second.transport_accepted &&
              recorder_calls == 1U && recorder_diagnostics.transport_active &&
              !recorder_diagnostics.recorder_active && recorder_diagnostics.recorder_failures == 1U,
          "recorder failure must disable only its branch while sharing continues");

  std::size_t exception_calls = 0U;
  glyphrelay::EncodedTransportQueue exception_queue;
  glyphrelay::EncodedAccessUnitFanout recorder_exception(
      &exception_queue,
      [&exception_calls](glyphrelay::RecordedAccessUnit) -> glyphrelay::RecorderEnqueueResult {
        ++exception_calls;
        throw std::runtime_error("fixture");
      });
  const auto exception_result = recorder_exception.publish(access_unit(1U, true), 1U);
  require(exception_result.transport_accepted && exception_result.recorder_failed &&
              exception_result.recorder_reason == "RECORDER_BRANCH_EXCEPTION" &&
              exception_calls == 1U,
          "a recorder callback exception must not escape or retract transport admission");
}

void test_recorder_acceptance_survives_transport_purge_and_failure() {
  glyphrelay::EncodedTransportQueue queue(
      {.maximum_access_units = 1U, .maximum_bytes = 32U, .maximum_age_ns = 100U});
  std::size_t recorder_accepts = 0U;
  glyphrelay::EncodedAccessUnitFanout fanout(
      &queue, [&recorder_accepts](glyphrelay::RecordedAccessUnit) {
        ++recorder_accepts;
        return glyphrelay::RecorderEnqueueResult{
            .accepted = true, .failed = false, .reason = "RECORDER_ACCESS_UNIT_ACCEPTED"};
      });
  require(fanout.publish(access_unit(1U, true), 1U).accepted_any,
          "first recovery must reach both fanout branches");
  const auto purged = fanout.publish(access_unit(2U, false), 2U);
  require(!purged.transport_accepted && purged.transport_recovery_required &&
              purged.recorder_accepted && recorder_accepts == 2U,
          "transport overload must never retract a recorder admission");
  const auto recovered = fanout.publish(access_unit(3U, true, 1U, 2U), 3U);
  require(recovered.transport_accepted && recovered.recorder_accepted,
          "both branches must continue after transport-only dependency recovery");

  glyphrelay::EncodedTransportQueue too_small(
      {.maximum_access_units = 1U, .maximum_bytes = 8U, .maximum_age_ns = 100U});
  std::size_t isolated_accepts = 0U;
  glyphrelay::EncodedAccessUnitFanout transport_failure(
      &too_small, [&isolated_accepts](glyphrelay::RecordedAccessUnit) {
        ++isolated_accepts;
        return glyphrelay::RecorderEnqueueResult{
            .accepted = true, .failed = false, .reason = "RECORDER_ACCESS_UNIT_ACCEPTED"};
      });
  const auto transport_unusable = transport_failure.publish(access_unit(1U, true, 1U, 1U, 9U), 1U);
  const auto recorder_only = transport_failure.publish(access_unit(2U, false, 1U, 1U, 4U), 2U);
  require(transport_unusable.transport_unusable && transport_unusable.recorder_accepted &&
              !recorder_only.transport_accepted && recorder_only.recorder_accepted &&
              isolated_accepts == 2U && !transport_failure.diagnostics().transport_active,
          "an unusable transport branch must not terminate a healthy recorder branch");
}

} // namespace

int main() {
  test_configuration_and_startup_recovery();
  test_fifo_identity_and_capacity_epoch_purge();
  test_age_boundary_and_explicit_recovery_coalescing();
  test_epoch_fencing_and_fail_closed_states();
  test_stop_cleanup();
  test_fanout_branch_identity_and_isolation();
  test_recorder_acceptance_survives_transport_purge_and_failure();
  std::cout << "encoded fanout tests passed\n";
  return 0;
}
