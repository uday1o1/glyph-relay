#include "glyphrelay/rtp_transport.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

void append_nal(std::vector<std::uint8_t> &access_unit, bool long_start_code,
                std::span<const std::uint8_t> nal) {
  if (long_start_code) {
    access_unit.push_back(0U);
  }
  access_unit.insert(access_unit.end(), {0U, 0U, 1U});
  access_unit.insert(access_unit.end(), nal.begin(), nal.end());
}

std::vector<std::uint8_t> idr_access_unit(std::size_t idr_payload_bytes = 2'400U) {
  std::vector<std::uint8_t> access_unit;
  const std::vector<std::uint8_t> sps = {0x67U, 0x42U, 0xC0U, 0x28U, 0x55U};
  const std::vector<std::uint8_t> pps = {0x68U, 0xCEU, 0x06U, 0xE2U};
  std::vector<std::uint8_t> idr(1U + idr_payload_bytes, 0x55U);
  idr.front() = 0x65U;
  append_nal(access_unit, true, sps);
  append_nal(access_unit, false, pps);
  append_nal(access_unit, true, idr);
  return access_unit;
}

std::vector<std::uint8_t> inter_access_unit() {
  std::vector<std::uint8_t> access_unit;
  const std::vector<std::uint8_t> inter = {0x41U, 0x9AU, 0x55U, 0x55U};
  append_nal(access_unit, false, inter);
  return access_unit;
}

glyphrelay::PacketizeAccessUnit input(std::span<const std::uint8_t> bytes,
                                      std::uint64_t dequeue_nanoseconds,
                                      std::uint64_t access_unit_id = 1U) {
  return {
      .annex_b = bytes,
      .frame_id = access_unit_id - 1U,
      .media_epoch = 4U,
      .access_unit_id = access_unit_id,
      .dependency_epoch = 8U,
      .dequeue_monotonic_nanoseconds = dequeue_nanoseconds,
  };
}

glyphrelay::PlaintextRtpPacket packet_fixture(std::uint64_t extended_sequence,
                                              std::uint64_t media_epoch = 2U,
                                              std::uint64_t dependency_epoch = 3U) {
  glyphrelay::PlaintextRtpPacket packet;
  packet.bytes.assign(glyphrelay::kRtpHeaderBytes + 1U, 0U);
  packet.bytes[0] = 0x80U;
  packet.bytes[1] = 96U;
  packet.bytes[2] = static_cast<std::uint8_t>((extended_sequence >> 8U) & 0xFFU);
  packet.bytes[3] = static_cast<std::uint8_t>(extended_sequence & 0xFFU);
  packet.bytes.back() = 0x41U;
  packet.identity = {
      .frame_id = extended_sequence,
      .media_epoch = media_epoch,
      .access_unit_id = extended_sequence + 1U,
      .dependency_epoch = dependency_epoch,
      .ssrc = 0x10203040U,
      .extended_sequence = extended_sequence,
      .wire_sequence = static_cast<std::uint16_t>(extended_sequence & 0xFFFFU),
      .extended_timestamp = extended_sequence * 3U,
      .wire_timestamp = static_cast<std::uint32_t>(extended_sequence * 3U),
      .role = glyphrelay::H264PacketRole::inter,
      .marker = true,
  };
  return packet;
}

void test_strict_h264_packetization_and_wrap() {
  constexpr std::uint64_t base_time = 5'000'000'000U;
  constexpr std::uint64_t base_timestamp = 0x1FFFFFFF0ULL;
  glyphrelay::H264RtpPacketizer packetizer(102U, 0x10203040U, 65'534U, base_time, base_timestamp);
  const auto access_unit = idr_access_unit();
  const auto result = packetizer.packetize(input(access_unit, base_time));
  require(result.passed && result.packets.size() == 5U,
          "SPS, PPS, and a large IDR must produce two single NAL packets and three FU-A packets");
  require(result.extended_timestamp == base_timestamp,
          "the first access unit must use the recorded random extended RTP timestamp base");

  const std::vector<std::uint16_t> expected_wire_sequences = {65'534U, 65'535U, 0U, 1U, 2U};
  std::vector<std::uint8_t> reconstructed_idr = {0x65U};
  for (std::size_t index = 0; index < result.packets.size(); ++index) {
    const auto &packet = result.packets[index];
    require(packet.identity.extended_sequence == 65'534U + index &&
                packet.identity.wire_sequence == expected_wire_sequences[index],
            "one extended allocator must map monotonically through 16-bit wire wrap");
    require(packet.payload().size() <= glyphrelay::kMaximumRtpPayloadBytes,
            "every RTP payload must respect the 1,200-byte contract");
    require(packet.identity.marker == (index + 1U == result.packets.size()) &&
                ((packet.bytes[1] & 0x80U) != 0U) == packet.identity.marker,
            "only the final packet of the access unit may carry the marker bit");
    require(packet.bytes[0] == 0x80U && (packet.bytes[1] & 0x7FU) == 102U,
            "each packet must carry RTP version 2 and the negotiated payload type");
    if (index >= 2U) {
      const auto payload = packet.payload();
      require((payload[0] & 0x1FU) == 28U && (payload[0] & 0xE0U) == 0x60U &&
                  (payload[1] & 0x1FU) == 5U,
              "FU-A must preserve the original forbidden bit, NRI, and IDR type");
      require(((payload[1] & 0x80U) != 0U) == (index == 2U) &&
                  ((payload[1] & 0x40U) != 0U) == (index == 4U),
              "FU-A start and end bits must identify only the first and final fragments");
      reconstructed_idr.insert(reconstructed_idr.end(), payload.begin() + 2, payload.end());
    }
  }
  const auto expected_idr = std::vector<std::uint8_t>(access_unit.end() - 2'401, access_unit.end());
  require(reconstructed_idr == expected_idr,
          "ordered FU-A fragments must reconstruct the original IDR NAL exactly");
  require(packetizer.next_extended_sequence() == 65'539U,
          "the packetizer must advance only its one extended sequence owner");

  const auto inter = inter_access_unit();
  const auto second = packetizer.packetize(input(inter, base_time + 1'000'000'000U, 2U));
  require(second.passed && second.packets.size() == 1U &&
              second.extended_timestamp == base_timestamp + glyphrelay::kRtpClockRate &&
              second.packets.front().identity.wire_timestamp ==
                  static_cast<std::uint32_t>(base_timestamp + glyphrelay::kRtpClockRate),
          "the 90 kHz extended timestamp must stay monotonic through 32-bit wire wrap");
  const auto repeated_time = packetizer.packetize(input(inter, base_time + 1'000'000'000U, 3U));
  require(!repeated_time.passed && repeated_time.reason == "rtp_timestamp_not_strictly_increasing",
          "equal dequeue times must not create duplicate extended RTP timestamps");
}

void test_packetizer_rejects_invalid_inputs() {
  bool invalid_payload_type = false;
  try {
    static_cast<void>(glyphrelay::H264RtpPacketizer(95U, 1U, 0U, 0U, 0U));
  } catch (const std::invalid_argument &) {
    invalid_payload_type = true;
  }
  require(invalid_payload_type, "a non-dynamic H.264 payload type must fail at construction");

  glyphrelay::H264RtpPacketizer packetizer(96U, 1U, 4U, 100U, 200U);
  const std::vector<std::uint8_t> malformed = {0x65U, 0x55U};
  require(!packetizer.packetize(input(malformed, 100U)).passed,
          "malformed Annex B must fail before sequence or timestamp allocation");

  std::vector<std::uint8_t> idr_without_parameter_sets;
  const std::vector<std::uint8_t> idr = {0x65U, 0x55U, 0x55U};
  append_nal(idr_without_parameter_sets, false, idr);
  const auto missing = packetizer.packetize(input(idr_without_parameter_sets, 100U));
  require(!missing.passed && missing.reason == "rtp_idr_missing_sps_pps",
          "an IDR without SPS and PPS must fail before RTP emission");

  auto invalid_identity = input(inter_access_unit(), 100U);
  invalid_identity.media_epoch = 0U;
  require(!packetizer.packetize(invalid_identity).passed,
          "zero media or dependency epochs must fail before packetization");
  require(packetizer.next_extended_sequence() == 4U,
          "rejected access units must not consume the sole sequence allocator");

  glyphrelay::H264RtpPacketizer exhausted(96U, 1U, std::numeric_limits<std::uint64_t>::max(), 100U,
                                          200U);
  const auto exhaustion = exhausted.packetize(input(inter_access_unit(), 100U));
  require(!exhaustion.passed && exhaustion.reason == "rtp_extended_sequence_exhausted" &&
              exhausted.next_extended_sequence() == std::numeric_limits<std::uint64_t>::max(),
          "extended sequence exhaustion must fail without wrapping the sole allocator");
}

void test_retransmission_cache_bounds_and_identity() {
  glyphrelay::RetransmissionCache cache;
  cache.reset_epoch(2U, 3U);
  const auto original = packet_fixture(65'535U);
  require(cache.store(original, 0U), "an active-epoch RTP packet must enter the cache");
  const auto first = cache.resolve(65'535U, 2U, 3U, 499U);
  const auto second = cache.resolve(65'535U, 2U, 3U, 499U);
  const auto exhausted = cache.resolve(65'535U, 2U, 3U, 499U);
  require(first.resolution == glyphrelay::NackResolution::retransmit && first.packet &&
              first.packet->bytes == original.bytes &&
              second.resolution == glyphrelay::NackResolution::retransmit && second.packet &&
              second.packet->identity.extended_sequence == original.identity.extended_sequence &&
              exhausted.resolution == glyphrelay::NackResolution::retransmission_limit,
          "two retransmissions must preserve plaintext RTP identity and a third must fail closed");
  require(cache.resolve(65'535U, 1U, 3U, 499U).resolution ==
              glyphrelay::NackResolution::stale_epoch,
          "a NACK from a stale media epoch must not resolve");
  require(cache.resolve(65'535U, 2U, 3U, 500U).resolution == glyphrelay::NackResolution::missing,
          "a packet must expire at the exact 500-millisecond age boundary");

  cache.reset_epoch(2U, 4U);
  auto first_wrap = packet_fixture(7U, 2U, 4U);
  auto second_wrap = packet_fixture(65'543U, 2U, 4U);
  require(cache.store(first_wrap, 600U) && cache.store(second_wrap, 600U),
          "two extended identities with the same low sequence may be represented defensively");
  require(cache.resolve(7U, 2U, 4U, 600U).resolution == glyphrelay::NackResolution::ambiguous,
          "a 16-bit NACK with two active-epoch matches must never guess");

  cache.reset_epoch(5U, 6U);
  for (std::size_t index = 0; index < glyphrelay::RetransmissionCache::kMaximumPackets + 1U;
       ++index) {
    require(cache.store(packet_fixture(index, 5U, 6U), 700U),
            "bounded packet fixtures must enter the active cache");
  }
  const auto bounded = cache.snapshot();
  require(bounded.packets == glyphrelay::RetransmissionCache::kMaximumPackets &&
              bounded.bytes <= glyphrelay::RetransmissionCache::kMaximumBytes &&
              bounded.evicted_packet_limit == 1U,
          "the cache must evict its oldest packet at the 2,048-packet hard limit");

  cache.reset_epoch(7U, 8U);
  for (std::size_t index = 0U; index < 1'500U; ++index) {
    auto large_header = packet_fixture(index, 7U, 8U);
    large_header.rtp_header_bytes = 1'800U;
    large_header.bytes.resize(3'000U, 0U);
    require(cache.store(large_header, 800U),
            "a bounded packet with a large valid RTP extension header must enter the cache");
  }
  const auto byte_bounded = cache.snapshot();
  require(byte_bounded.packets < 1'500U &&
              byte_bounded.bytes <= glyphrelay::RetransmissionCache::kMaximumBytes &&
              byte_bounded.evicted_byte_limit > 0U,
          "the cache must evict oldest packets before crossing its independent 4 MiB cap");
  cache.clear();
  require(cache.snapshot().packets == 0U && cache.snapshot().bytes == 0U &&
              cache.media_epoch() == 0U && cache.dependency_epoch() == 0U,
          "explicit clear must erase entries and invalidate the active epoch");
}

void test_generic_nack_expansion_and_recovery_limits() {
  const std::vector<glyphrelay::GenericNackField> wrapping_fields = {
      {.packet_id = 65'535U, .lost_packet_bitmask = 0x0003U},
      {.packet_id = 0U, .lost_packet_bitmask = 0x0001U},
  };
  const auto identifiers = glyphrelay::expand_generic_nack(wrapping_fields);
  const std::vector<std::uint16_t> expected = {65'535U, 0U, 1U};
  require(identifiers == expected,
          "PID and BLP expansion must wrap modulo 16 bits and remove duplicates in order");

  glyphrelay::RetransmissionCache cache;
  glyphrelay::RtpRecoveryController recovery(cache);
  require(recovery.begin_epoch(2U, 3U, 0U, glyphrelay::RecoveryTrigger::startup),
          "a new dependency epoch must request one startup IDR");
  require(!recovery.handle_pli(2U, 3U, 0U).request_idr_with_parameter_sets,
          "a simultaneous PLI must coalesce with the startup IDR");
  require(recovery.handle_pli(2U, 3U, 1'000U).request_idr_with_parameter_sets,
          "a later authenticated PLI must request an IDR with SPS and PPS");
  require(cache.store(packet_fixture(65'535U), 1'600U),
          "the active rollover packet must enter the recovery cache");

  const std::vector<glyphrelay::GenericNackField> one = {
      {.packet_id = 65'535U, .lost_packet_bitmask = 0U}};
  const auto retransmit = recovery.handle_nack(2U, 3U, one, 2'000U);
  require(retransmit.retransmissions.size() == 1U &&
              retransmit.retransmissions.front().bytes == packet_fixture(65'535U).bytes,
          "Generic NACK must return the exact cached original RTP bytes and identity");
  const auto stale = recovery.handle_nack(1U, 3U, one, 2'001U);
  require(stale.retransmissions.empty() && recovery.diagnostics().stale_feedback == 1U,
          "stale-epoch Generic NACK must be ignored and diagnosed");
  const std::vector<glyphrelay::GenericNackField> absent = {
      {.packet_id = 1'234U, .lost_packet_bitmask = 0U}};
  const auto absent_recovery = recovery.handle_nack(2U, 3U, absent, 3'000U);
  require(absent_recovery.retransmissions.empty() &&
              absent_recovery.request_idr_with_parameter_sets,
          "an absent active-epoch NACK must request one coalesced recovery IDR");
  recovery.stop();
  require(cache.snapshot().packets == 0U,
          "session stop must immediately erase the retransmission cache");

  glyphrelay::RetransmissionCache ambiguous_cache;
  glyphrelay::RtpRecoveryController ambiguous_recovery(ambiguous_cache);
  require(ambiguous_recovery.begin_epoch(5U, 6U, 0U, glyphrelay::RecoveryTrigger::startup),
          "the ambiguous rollover fixture must begin an active epoch");
  require(ambiguous_cache.store(packet_fixture(7U, 5U, 6U), 600U) &&
              ambiguous_cache.store(packet_fixture(65'543U, 5U, 6U), 600U),
          "the ambiguous rollover fixture must retain both extended identities");
  const std::vector<glyphrelay::GenericNackField> ambiguous = {
      {.packet_id = 7U, .lost_packet_bitmask = 0U}};
  const auto ambiguous_decision = ambiguous_recovery.handle_nack(5U, 6U, ambiguous, 1'000U);
  require(ambiguous_decision.retransmissions.empty() &&
              ambiguous_decision.request_idr_with_parameter_sets,
          "an ambiguous low-16-bit NACK must request recovery instead of guessing");
}

void test_feedback_rate_and_sustained_flood_termination() {
  glyphrelay::RetransmissionCache cache;
  glyphrelay::RtpRecoveryController recovery(cache);
  require(recovery.begin_epoch(9U, 10U, 0U, glyphrelay::RecoveryTrigger::startup),
          "the flood fixture must begin with one active dependency epoch");

  bool saw_rate_limit = false;
  bool terminated = false;
  for (std::uint64_t now = 50U; now <= 11'000U; now += 50U) {
    const auto decision = recovery.handle_pli(9U, 10U, now);
    saw_rate_limit = saw_rate_limit || decision.feedback_rate_limited;
    if (decision.terminate_session) {
      terminated = true;
      break;
    }
  }
  require(saw_rate_limit && terminated && recovery.diagnostics().feedback_flood_terminated,
          "feedback above ten messages per second for ten seconds must terminate visibly");
  require(!recovery.begin_epoch(9U, 11U, 12'000U,
                                glyphrelay::RecoveryTrigger::dependency_epoch_transition) &&
              !recovery.request_forced_idr(12'001U, glyphrelay::RecoveryTrigger::resume),
          "a feedback-flood termination must be irreversible for that controller instance");

  glyphrelay::RetransmissionCache identifier_cache;
  glyphrelay::RtpRecoveryController identifier_recovery(identifier_cache);
  require(identifier_recovery.begin_epoch(12U, 13U, 0U, glyphrelay::RecoveryTrigger::startup),
          "the identifier-rate fixture must begin its epoch");
  std::vector<glyphrelay::GenericNackField> too_many;
  for (std::uint16_t identifier = 0U; identifier < 101U; ++identifier) {
    too_many.push_back({.packet_id = identifier, .lost_packet_bitmask = 0U});
  }
  const auto limited = identifier_recovery.handle_nack(12U, 13U, too_many, 100U);
  require(limited.feedback_rate_limited &&
              identifier_recovery.diagnostics().distinct_nack_identifiers == 100U &&
              identifier_recovery.diagnostics().ignored_nack_identifiers == 1U,
          "the 101st distinct NACK identifier in one second must be ignored with a bounded count");
}

} // namespace

int main() {
  test_strict_h264_packetization_and_wrap();
  test_packetizer_rejects_invalid_inputs();
  test_retransmission_cache_bounds_and_identity();
  test_generic_nack_expansion_and_recovery_limits();
  test_feedback_rate_and_sustained_flood_termination();
  return 0;
}
