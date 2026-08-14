#include "glyphrelay/media_pacer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

glyphrelay::PlaintextRtpPacket packet(std::uint64_t access_unit, std::uint64_t sequence,
                                      glyphrelay::H264PacketRole role, bool marker,
                                      std::size_t bytes = 100U,
                                      std::uint64_t dependency_epoch = 1U) {
  glyphrelay::PlaintextRtpPacket result;
  result.bytes.assign(bytes, std::uint8_t{0x42U});
  result.rtp_header_bytes = glyphrelay::kRtpHeaderBytes;
  result.identity = {
      .frame_id = access_unit,
      .media_epoch = 1U,
      .access_unit_id = access_unit,
      .dependency_epoch = dependency_epoch,
      .ssrc = 7U,
      .extended_sequence = sequence,
      .wire_sequence = static_cast<std::uint16_t>(sequence),
      .extended_timestamp = 90'000U + access_unit * 3'000U,
      .wire_timestamp = static_cast<std::uint32_t>(90'000U + access_unit * 3'000U),
      .role = role,
      .marker = marker,
  };
  return result;
}

std::vector<glyphrelay::PlaintextRtpPacket> recovery(std::uint64_t access_unit,
                                                     std::uint64_t sequence,
                                                     std::size_t bytes = 100U,
                                                     std::uint64_t dependency_epoch = 1U) {
  return {
      packet(access_unit, sequence, glyphrelay::H264PacketRole::sequence_parameter_set, false,
             bytes, dependency_epoch),
      packet(access_unit, sequence + 1U, glyphrelay::H264PacketRole::picture_parameter_set, false,
             bytes, dependency_epoch),
      packet(access_unit, sequence + 2U, glyphrelay::H264PacketRole::idr, true, bytes,
             dependency_epoch),
  };
}

void test_configuration_and_atomic_recovery_admission() {
  glyphrelay::RetransmissionCache cache;
  bool rejected = false;
  try {
    glyphrelay::MediaPacerQueue invalid(cache, {.maximum_bytes = 4U * 1024U * 1024U + 1U});
    static_cast<void>(invalid);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "pacer configuration may not exceed the frozen hard bound");

  glyphrelay::MediaPacerQueue pacer(cache, {.maximum_bytes = 1'000U});
  pacer.set_target_bits_per_second(80'000.0, 0U);
  const auto initial = pacer.snapshot();
  require(initial.awaiting_recovery && initial.maximum_age_milliseconds == 100U &&
              initial.burst_capacity_bytes == 1'000.0 && initial.available_tokens_bytes == 1'000.0,
          "the pacer must begin behind a recovery barrier with a 100ms token burst");
  const auto predicted = packet(1U, 1U, glyphrelay::H264PacketRole::inter, true);
  require(!pacer.admit_access_unit(std::span(&predicted, 1U), 0U).accepted,
          "predicted media must not open initial pacer admission");
  const auto idr = recovery(2U, 2U);
  const auto admitted = pacer.admit_access_unit(idr, 0U);
  require(admitted.accepted && pacer.snapshot().packets == 3U && pacer.snapshot().bytes == 300U &&
              cache.media_epoch() == 1U && cache.dependency_epoch() == 1U,
          "a complete SPS PPS IDR batch must open admission atomically");
}

void test_token_bucket_and_exact_age_boundary() {
  glyphrelay::RetransmissionCache cache;
  glyphrelay::MediaPacerQueue pacer(cache, {.maximum_bytes = 2'000U});
  pacer.set_target_bits_per_second(8'000.0, 0U);
  const auto idr = recovery(1U, 1U, 50U);
  require(pacer.admit_access_unit(idr, 0U).accepted, "token fixture recovery must be admitted");
  require(pacer.dequeue(0U).packet.has_value() && pacer.dequeue(0U).packet.has_value(),
          "the initial 100ms bucket must release packets up to its capacity");
  require(!pacer.dequeue(0U).packet.has_value(),
          "a packet beyond the current token balance must wait");
  require(pacer.dequeue(50U).packet.has_value(),
          "monotonic refill must release the waiting packet");

  const auto predicted = packet(2U, 4U, glyphrelay::H264PacketRole::inter, true, 100U);
  require(pacer.admit_access_unit(std::span(&predicted, 1U), 50U).accepted,
          "same-epoch predicted media must be accepted");
  require(!pacer.dequeue(149U).recovery_required,
          "a packet must remain queued one millisecond before the age limit");
  const auto expired = pacer.dequeue(150U);
  require(expired.recovery_required && pacer.snapshot().packets == 0U &&
              pacer.snapshot().bytes == 0U && pacer.snapshot().age_purges == 1U &&
              cache.media_epoch() == 0U,
          "the exact 100ms age boundary must purge the dependency epoch and cache");
}

void test_capacity_purges_whole_dependency_epoch() {
  glyphrelay::RetransmissionCache cache;
  glyphrelay::MediaPacerQueue pacer(cache, {.maximum_bytes = 500U});
  pacer.set_target_bits_per_second(1'000.0, 0U);
  const auto idr = recovery(1U, 1U, 100U);
  require(pacer.admit_access_unit(idr, 0U).accepted, "capacity fixture recovery must be admitted");
  const auto predicted = packet(2U, 4U, glyphrelay::H264PacketRole::inter, true, 250U);
  const auto overflow = pacer.admit_access_unit(std::span(&predicted, 1U), 1U);
  const auto purged = pacer.snapshot();
  require(!overflow.accepted && overflow.recovery_required && purged.packets == 0U &&
              purged.bytes == 0U && purged.purged_packets == 3U && purged.capacity_purges == 1U &&
              purged.awaiting_recovery,
          "capacity overflow must purge every queued packet in the dependency epoch");
  require(!pacer.admit_access_unit(std::span(&predicted, 1U), 2U).accepted,
          "later media from an abandoned dependency epoch must remain fenced");
  const auto next = recovery(3U, 5U, 100U, 2U);
  require(pacer.admit_access_unit(next, 3U).accepted,
          "a newer complete recovery epoch must reopen the pacer");
}

void test_oversized_recovery_and_stop_cleanup() {
  glyphrelay::RetransmissionCache cache;
  glyphrelay::MediaPacerQueue pacer(cache, {.maximum_bytes = 200U});
  pacer.set_target_bits_per_second(8'000.0, 0U);
  const auto oversized = recovery(1U, 1U, 100U);
  const auto rejected = pacer.admit_access_unit(oversized, 0U);
  require(!rejected.accepted && rejected.recovery_required && rejected.unusable_candidate &&
              pacer.snapshot().packets == 0U,
          "a recovery access unit larger than the hard cap must be an unusable candidate");
  pacer.stop();
  pacer.stop();
  const auto stopped = pacer.snapshot();
  require(stopped.stopped && !stopped.admission_open && !stopped.awaiting_recovery &&
              stopped.packets == 0U && stopped.bytes == 0U,
          "idempotent stop must release the whole pacer and close admission");
}

} // namespace

int main() {
  test_configuration_and_atomic_recovery_admission();
  test_token_bucket_and_exact_age_boundary();
  test_capacity_purges_whole_dependency_epoch();
  test_oversized_recovery_and_stop_cleanup();
  std::cout << "media pacer tests passed\n";
  return 0;
}
