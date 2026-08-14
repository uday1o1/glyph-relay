#include "glyphrelay_media_handlers.hpp"

#include <rtc/message.hpp>
#include <rtc/rtp.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace {

constexpr std::uint32_t kSsrc = 0x10203040U;

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

void append_nal(rtc::binary &access_unit, bool long_start_code, std::span<const std::uint8_t> nal) {
  if (long_start_code) {
    access_unit.push_back(rtc::byte{0U});
  }
  access_unit.insert(access_unit.end(), {rtc::byte{0U}, rtc::byte{0U}, rtc::byte{1U}});
  std::transform(nal.begin(), nal.end(), std::back_inserter(access_unit),
                 [](std::uint8_t value) { return static_cast<rtc::byte>(value); });
}

rtc::message_ptr idr_access_unit(std::uint64_t access_unit_id = 1U) {
  rtc::binary bytes;
  const std::vector<std::uint8_t> sps = {0x67U, 0x42U, 0xC0U, 0x28U, 0x55U};
  const std::vector<std::uint8_t> pps = {0x68U, 0xCEU, 0x06U, 0xE2U};
  std::vector<std::uint8_t> idr(2'401U, 0x55U);
  idr.front() = 0x65U;
  append_nal(bytes, true, sps);
  append_nal(bytes, false, pps);
  append_nal(bytes, true, idr);
  auto message = rtc::make_message(std::move(bytes), rtc::Message::Binary);
  message->frameId = access_unit_id - 1U;
  message->mediaEpoch = 4U;
  message->accessUnitId = access_unit_id;
  message->dependencyEpoch = 8U;
  message->extendedTimestamp = 0x1FFFFFFF0ULL + access_unit_id - 1U;
  return message;
}

rtc::message_ptr malformed_access_unit() {
  rtc::binary bytes = {rtc::byte{0x65U}, rtc::byte{0x55U}};
  auto message = rtc::make_message(std::move(bytes), rtc::Message::Binary);
  message->mediaEpoch = 4U;
  message->accessUnitId = 2U;
  message->dependencyEpoch = 8U;
  message->extendedTimestamp = 0x200000100ULL;
  return message;
}

rtc::message_ptr feedback(std::uint8_t payload_type, std::uint8_t format,
                          std::uint16_t packet_id = 0U, std::uint16_t lost_packet_bitmask = 0U) {
  const bool nack = payload_type == 205U;
  auto message = rtc::make_message(nack ? 16U : 12U, rtc::Message::Control);
  auto write_u16 = [&](std::size_t offset, std::uint16_t value) {
    (*message)[offset] = static_cast<rtc::byte>(value >> 8U);
    (*message)[offset + 1U] = static_cast<rtc::byte>(value & 0xFFU);
  };
  auto write_u32 = [&](std::size_t offset, std::uint32_t value) {
    (*message)[offset] = static_cast<rtc::byte>(value >> 24U);
    (*message)[offset + 1U] = static_cast<rtc::byte>((value >> 16U) & 0xFFU);
    (*message)[offset + 2U] = static_cast<rtc::byte>((value >> 8U) & 0xFFU);
    (*message)[offset + 3U] = static_cast<rtc::byte>(value & 0xFFU);
  };
  (*message)[0] = static_cast<rtc::byte>(0x80U | format);
  (*message)[1] = static_cast<rtc::byte>(payload_type);
  write_u16(2U, static_cast<std::uint16_t>(message->size() / 4U - 1U));
  write_u32(4U, 0x55667788U);
  write_u32(8U, kSsrc);
  if (nack) {
    write_u16(12U, packet_id);
    write_u16(14U, lost_packet_bitmask);
  }
  return message;
}

void test_pinned_packetizer_is_the_sole_sequence_owner() {
  auto config = std::make_shared<rtc::RtpPacketizationConfig>(kSsrc, "glyphrelay", 102U, 90'000U);
  config->setExtendedSequenceNumber(65'534U);
  glyphrelay::rtc_adapter::StrictH264Packetizer packetizer(config);
  rtc::message_vector messages = {idr_access_unit()};
  packetizer.outgoing(messages, [](rtc::message_ptr) {});
  require(messages.size() >= 5U,
          "the pinned packetizer must produce SPS, PPS, and multiple FU-A packets");
  std::vector<std::uint8_t> reconstructed_idr = {0x65U};
  for (std::size_t index = 0U; index < messages.size(); ++index) {
    const auto &message = messages[index];
    const auto *header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
    const auto header_bytes = header->getSize();
    const auto extended_sequence = 65'534U + index;
    const auto expected_role = index == 0U   ? rtc::Message::RtpRole::Sps
                               : index == 1U ? rtc::Message::RtpRole::Pps
                                             : rtc::Message::RtpRole::Idr;
    require(message->extendedSequenceNumber == extended_sequence &&
                header->seqNumber() == static_cast<std::uint16_t>(extended_sequence & 0xFFFFU) &&
                message->rtpRole == expected_role,
            "the pinned allocator must expose one 64-bit identity through 16-bit wrap");
    require(header->timestamp() == static_cast<std::uint32_t>(message->extendedTimestamp) &&
                message->extendedTimestamp == 0x1FFFFFFF0ULL,
            "the pinned packetizer must mirror the low extended timestamp bits on wire");
    require(message->size() - header_bytes <= glyphrelay::kMaximumRtpPayloadBytes,
            "the pinned packetizer must enforce the 1,200-byte RTP payload bound");
    require((header->marker() != 0U) == (index + 1U == messages.size()),
            "only the final pinned-library packet may carry the marker bit");
    if (index >= 2U) {
      const auto payload = std::span(*message).subspan(header_bytes);
      require(payload.size() >= 3U && (std::to_integer<std::uint8_t>(payload[0]) & 0x1FU) == 28U &&
                  (std::to_integer<std::uint8_t>(payload[0]) & 0xE0U) == 0x60U &&
                  (std::to_integer<std::uint8_t>(payload[1]) & 0x1FU) == 5U &&
                  ((std::to_integer<std::uint8_t>(payload[1]) & 0x80U) != 0U) == (index == 2U) &&
                  ((std::to_integer<std::uint8_t>(payload[1]) & 0x40U) != 0U) ==
                      (index + 1U == messages.size()),
              "pinned FU-A fragments must preserve NRI and ordered start and end identity");
      std::transform(payload.begin() + 2, payload.end(), std::back_inserter(reconstructed_idr),
                     [](rtc::byte value) { return std::to_integer<std::uint8_t>(value); });
    }
  }
  std::vector<std::uint8_t> expected_idr(2'401U, 0x55U);
  expected_idr.front() = 0x65U;
  require(reconstructed_idr == expected_idr,
          "pinned FU-A fragments must reconstruct the original IDR exactly without STAP-A");
  const auto expected_next_sequence = 65'534U + messages.size();
  require(config->extendedSequenceNumber == expected_next_sequence &&
              config->sequenceNumber ==
                  static_cast<std::uint16_t>(expected_next_sequence & 0xFFFFU),
          "the pinned config must retain the sole next extended and wire sequence");

  const auto next_sequence = config->extendedSequenceNumber;
  rtc::message_vector malformed = {malformed_access_unit()};
  packetizer.outgoing(malformed, [](rtc::message_ptr) {});
  require(malformed.empty() &&
              packetizer.take_last_rejection() ==
                  std::optional<std::string>("rtp_annex_b_missing_initial_start_code") &&
              config->extendedSequenceNumber == next_sequence,
          "strict validation must reject malformed Annex B before library allocation");

  rtc::message_vector repeated_timestamp = {idr_access_unit(2U)};
  repeated_timestamp.front()->extendedTimestamp = 0x1FFFFFFF0ULL;
  packetizer.outgoing(repeated_timestamp, [](rtc::message_ptr) {});
  require(repeated_timestamp.empty() &&
              packetizer.take_last_rejection() ==
                  std::optional<std::string>("rtp_timestamp_not_strictly_increasing") &&
              config->extendedSequenceNumber == next_sequence,
          "a repeated extended timestamp must fail before the sole allocator advances");
}

void test_bounded_responder_replays_original_plaintext_rtp() {
  auto config = std::make_shared<rtc::RtpPacketizationConfig>(kSsrc, "glyphrelay", 102U, 90'000U);
  config->setExtendedSequenceNumber(65'534U);
  glyphrelay::rtc_adapter::StrictH264Packetizer packetizer(config);
  rtc::message_vector packets = {idr_access_unit()};
  packetizer.outgoing(packets, [](rtc::message_ptr) {});

  std::uint64_t now = 0U;
  unsigned int idr_requests = 0U;
  unsigned int terminations = 0U;
  glyphrelay::rtc_adapter::BoundedNackResponder responder(
      kSsrc, [&] { return now; }, [&] { ++idr_requests; }, [&] { ++terminations; });
  require(responder.begin_epoch(4U, 8U, glyphrelay::RecoveryTrigger::startup) && idr_requests == 1U,
          "starting a responder epoch must request one recovery IDR");
  responder.outgoing(packets, [](rtc::message_ptr) {});
  require(responder.cache_snapshot().packets == packets.size(),
          "every pinned-library RTP packet must enter the bounded cache");

  std::vector<rtc::message_ptr> replayed;
  const auto send = [&](rtc::message_ptr message) { replayed.push_back(std::move(message)); };
  now = 100U;
  rtc::message_vector nack = {feedback(205U, 1U, 65'535U, 0x0001U)};
  responder.incoming(nack, send);
  require(replayed.size() == 2U && *replayed[0] == *packets[1] && *replayed[1] == *packets[2] &&
              replayed[0]->extendedSequenceNumber == packets[1]->extendedSequenceNumber &&
              replayed[1]->extendedSequenceNumber == packets[2]->extendedSequenceNumber,
          "Generic NACK PID and BLP must replay exact original RTP bytes and identities at wrap");

  replayed.clear();
  now = 200U;
  responder.incoming(nack, send);
  require(replayed.size() == 2U,
          "a second Generic NACK may use the final per-packet retransmission allowance");
  replayed.clear();
  now = 300U;
  responder.incoming(nack, send);
  require(replayed.empty() && responder.cache_snapshot().retransmissions == 4U,
          "a third Generic NACK must not exceed the two-retransmission packet limit");

  now = 1'000U;
  rtc::message_vector pli = {feedback(206U, 1U)};
  responder.incoming(pli, send);
  require(idr_requests == 2U,
          "an authenticated PLI must enter the shared IDR-with-parameter-sets limiter");

  auto malformed = feedback(205U, 1U, 0U, 0U);
  malformed->resize(15U);
  rtc::message_vector malformed_messages = {malformed};
  responder.incoming(malformed_messages, send);
  require(responder.malformed_feedback_messages() == 1U,
          "truncated RTCP feedback must be rejected without cache access");

  responder.stop();
  require(responder.cache_snapshot().packets == 0U && terminations == 0U,
          "responder stop must immediately erase cached RTP without a false termination");
}

} // namespace

int main() {
  test_pinned_packetizer_is_the_sole_sequence_owner();
  test_bounded_responder_replays_original_plaintext_rtp();
  return 0;
}
