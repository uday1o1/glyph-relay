/**
 * Copyright (c) 2026 GlyphRelay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "glyphrelay_media_handlers.hpp"

#include "glyphrelay/annex_b.hpp"

#include <rtc/message.hpp>
#include <rtc/rtp.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace glyphrelay::rtc_adapter {
namespace {

constexpr std::size_t kRtcpFeedbackHeaderBytes = 12U;
constexpr std::size_t kRtcpNackFieldBytes = 4U;
constexpr std::size_t kMaximumParsedNackFields = 101U;

std::span<const std::uint8_t> as_uint8_span(const rtc::binary &bytes) {
  return {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()};
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                    static_cast<std::uint16_t>(bytes[offset + 1U]));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 3U]);
}

bool has_required_parameter_sets(const AnnexBAccessUnit &access_unit) {
  return !access_unit.contains(5U) || access_unit.starts_with_parameter_sets_and_idr();
}

std::optional<rtc::Message::RtpRole> rtp_role(std::span<const std::uint8_t> payload) {
  if (payload.empty()) {
    return std::nullopt;
  }
  auto type = static_cast<std::uint8_t>(payload.front() & 0x1FU);
  if (type == 28U) {
    if (payload.size() < 2U) {
      return std::nullopt;
    }
    type = static_cast<std::uint8_t>(payload[1] & 0x1FU);
  }
  switch (type) {
  case 7U:
    return rtc::Message::RtpRole::Sps;
  case 8U:
    return rtc::Message::RtpRole::Pps;
  case 5U:
    return rtc::Message::RtpRole::Idr;
  case 1U:
    return rtc::Message::RtpRole::Inter;
  default:
    return rtc::Message::RtpRole::Supplemental;
  }
}

H264PacketRole to_packet_role(rtc::Message::RtpRole role) {
  switch (role) {
  case rtc::Message::RtpRole::Sps:
    return H264PacketRole::sequence_parameter_set;
  case rtc::Message::RtpRole::Pps:
    return H264PacketRole::picture_parameter_set;
  case rtc::Message::RtpRole::Idr:
    return H264PacketRole::idr;
  case rtc::Message::RtpRole::Inter:
    return H264PacketRole::inter;
  case rtc::Message::RtpRole::Supplemental:
  case rtc::Message::RtpRole::Unspecified:
    return H264PacketRole::supplemental;
  }
  return H264PacketRole::supplemental;
}

rtc::Message::RtpRole to_rtc_role(H264PacketRole role) {
  switch (role) {
  case H264PacketRole::sequence_parameter_set:
    return rtc::Message::RtpRole::Sps;
  case H264PacketRole::picture_parameter_set:
    return rtc::Message::RtpRole::Pps;
  case H264PacketRole::idr:
    return rtc::Message::RtpRole::Idr;
  case H264PacketRole::inter:
    return rtc::Message::RtpRole::Inter;
  case H264PacketRole::supplemental:
    return rtc::Message::RtpRole::Supplemental;
  }
  return rtc::Message::RtpRole::Unspecified;
}

std::optional<PlaintextRtpPacket> to_cached_packet(const rtc::message_ptr &message,
                                                   std::uint32_t media_ssrc) {
  if (!message || message->type == rtc::Message::Control || message->size() < kRtpHeaderBytes) {
    return std::nullopt;
  }
  const auto *header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
  const auto header_bytes = header->getSize();
  if (header->version() != 2U || header->ssrc() != media_ssrc || header_bytes < kRtpHeaderBytes ||
      header_bytes > message->size()) {
    return std::nullopt;
  }

  PlaintextRtpPacket packet;
  packet.bytes.reserve(message->size());
  std::transform(message->begin(), message->end(), std::back_inserter(packet.bytes),
                 [](rtc::byte value) { return std::to_integer<std::uint8_t>(value); });
  packet.rtp_header_bytes = header_bytes;
  packet.identity = {
      .frame_id = message->frameId,
      .media_epoch = message->mediaEpoch,
      .access_unit_id = message->accessUnitId,
      .dependency_epoch = message->dependencyEpoch,
      .ssrc = header->ssrc(),
      .extended_sequence = message->extendedSequenceNumber,
      .wire_sequence = header->seqNumber(),
      .extended_timestamp = message->extendedTimestamp,
      .wire_timestamp = header->timestamp(),
      .role = to_packet_role(message->rtpRole),
      .marker = header->marker() != 0U,
  };
  return packet;
}

rtc::message_ptr to_rtc_message(const PlaintextRtpPacket &packet) {
  auto message = rtc::make_message(packet.bytes.size(), rtc::Message::Binary);
  std::transform(packet.bytes.begin(), packet.bytes.end(), message->begin(),
                 [](std::uint8_t value) { return static_cast<rtc::byte>(value); });
  message->frameId = packet.identity.frame_id;
  message->mediaEpoch = packet.identity.media_epoch;
  message->accessUnitId = packet.identity.access_unit_id;
  message->dependencyEpoch = packet.identity.dependency_epoch;
  message->extendedSequenceNumber = packet.identity.extended_sequence;
  message->extendedTimestamp = packet.identity.extended_timestamp;
  message->rtpRole = to_rtc_role(packet.identity.role);
  return message;
}

} // namespace

StrictH264Packetizer::StrictH264Packetizer(std::shared_ptr<rtc::RtpPacketizationConfig> config)
    : config_(std::move(config)),
      packetizer_(rtc::NalUnit::Separator::StartSequence, config_, kMaximumRtpPayloadBytes) {
  if (!config_) {
    throw std::invalid_argument("strict H.264 packetizer requires an RTP configuration");
  }
}

void StrictH264Packetizer::outgoing(rtc::message_vector &messages,
                                    const rtc::message_callback &send) {
  std::lock_guard lock(mutex_);
  rtc::message_vector result;
  for (const auto &message : messages) {
    std::string rejection;
    if (!message || message->type == rtc::Message::Control || message->mediaEpoch == 0U ||
        message->dependencyEpoch == 0U || message->accessUnitId == 0U ||
        message->extendedTimestamp == 0U) {
      rejection = "rtp_access_unit_identity_invalid";
    } else {
      const auto parsed = parse_annex_b_access_unit(as_uint8_span(*message));
      if (!parsed.passed) {
        rejection = "rtp_" + parsed.reason;
      } else if (!has_required_parameter_sets(parsed.access_unit)) {
        rejection = "rtp_idr_missing_sps_pps";
      }
    }

    if (!rejection.empty()) {
      last_rejection_ = rejection;
      continue;
    }
    if (last_extended_timestamp_ && message->extendedTimestamp <= *last_extended_timestamp_) {
      last_rejection_ = "rtp_timestamp_not_strictly_increasing";
      continue;
    }
    last_extended_timestamp_ = message->extendedTimestamp;

    config_->setExtendedTimestamp(message->extendedTimestamp);
    rtc::message_vector one = {message};
    packetizer_.outgoing(one, send);
    const bool valid_output = std::all_of(one.begin(), one.end(), [](const auto &packet) {
      if (!packet || packet->size() < kRtpHeaderBytes) {
        return false;
      }
      const auto *header = reinterpret_cast<const rtc::RtpHeader *>(packet->data());
      const auto header_bytes = header->getSize();
      if (header_bytes > packet->size() ||
          packet->size() - header_bytes > kMaximumRtpPayloadBytes) {
        return false;
      }
      const auto payload = as_uint8_span(*packet).subspan(header_bytes);
      const auto role = rtp_role(payload);
      if (!role || (payload.front() & 0x1FU) == 24U) {
        return false;
      }
      packet->rtpRole = *role;
      return true;
    });
    if (!valid_output) {
      last_rejection_ = "rtp_library_packetizer_contract_violation";
      continue;
    }
    result.insert(result.end(), std::make_move_iterator(one.begin()),
                  std::make_move_iterator(one.end()));
  }
  messages.swap(result);
}

std::optional<std::string> StrictH264Packetizer::take_last_rejection() {
  std::lock_guard lock(mutex_);
  auto result = std::move(last_rejection_);
  last_rejection_.reset();
  return result;
}

BoundedNackResponder::BoundedNackResponder(std::uint32_t media_ssrc, Clock now_milliseconds,
                                           Callback request_idr_with_parameter_sets,
                                           Callback terminate_session)
    : media_ssrc_(media_ssrc), now_milliseconds_(std::move(now_milliseconds)),
      request_idr_with_parameter_sets_(std::move(request_idr_with_parameter_sets)),
      terminate_session_(std::move(terminate_session)), recovery_(cache_) {
  if (media_ssrc_ == 0U || !now_milliseconds_ || !request_idr_with_parameter_sets_ ||
      !terminate_session_) {
    throw std::invalid_argument("bounded NACK responder requires an SSRC, clock, and callbacks");
  }
}

bool BoundedNackResponder::begin_epoch(std::uint64_t media_epoch, std::uint64_t dependency_epoch,
                                       RecoveryTrigger trigger) {
  bool requested = false;
  {
    std::lock_guard lock(mutex_);
    const auto now = now_milliseconds_();
    requested = recovery_.begin_epoch(media_epoch, dependency_epoch, now, trigger);
    if (cache_.media_epoch() == media_epoch && cache_.dependency_epoch() == dependency_epoch) {
      media_epoch_ = media_epoch;
      dependency_epoch_ = dependency_epoch;
      termination_notified_ = false;
    }
  }
  if (requested) {
    request_idr_with_parameter_sets_();
  }
  return requested;
}

bool BoundedNackResponder::request_forced_idr(RecoveryTrigger trigger) {
  bool requested = false;
  {
    std::lock_guard lock(mutex_);
    requested = recovery_.request_forced_idr(now_milliseconds_(), trigger);
  }
  if (requested) {
    request_idr_with_parameter_sets_();
  }
  return requested;
}

void BoundedNackResponder::stop() {
  std::lock_guard lock(mutex_);
  recovery_.stop();
  media_epoch_ = 0U;
  dependency_epoch_ = 0U;
}

void BoundedNackResponder::outgoing(rtc::message_vector &messages,
                                    const rtc::message_callback &send) {
  static_cast<void>(send);
  std::lock_guard lock(mutex_);
  for (const auto &message : messages) {
    const auto packet = to_cached_packet(message, media_ssrc_);
    if (packet) {
      cache_.store(*packet, now_milliseconds_());
    }
  }
}

void BoundedNackResponder::incoming(rtc::message_vector &messages,
                                    const rtc::message_callback &send) {
  std::vector<rtc::message_ptr> retransmissions;
  bool request_idr = false;
  bool terminate = false;
  {
    std::lock_guard lock(mutex_);
    for (const auto &message : messages) {
      if (!message || message->type != rtc::Message::Control) {
        continue;
      }
      const auto bytes = as_uint8_span(*message);
      std::size_t offset = 0U;
      while (offset < bytes.size()) {
        if (bytes.size() - offset < 4U) {
          ++malformed_feedback_messages_;
          break;
        }
        const auto first = bytes[offset];
        const auto payload_type = bytes[offset + 1U];
        const auto packet_bytes =
            (static_cast<std::size_t>(read_u16(bytes, offset + 2U)) + 1U) * 4U;
        if ((first >> 6U) != 2U || (first & 0x20U) != 0U || packet_bytes < 4U ||
            packet_bytes > bytes.size() - offset) {
          ++malformed_feedback_messages_;
          break;
        }
        const auto format = static_cast<std::uint8_t>(first & 0x1FU);
        if (payload_type == 205U && format == 1U) {
          if (packet_bytes < kRtcpFeedbackHeaderBytes + kRtcpNackFieldBytes ||
              (packet_bytes - kRtcpFeedbackHeaderBytes) % kRtcpNackFieldBytes != 0U) {
            ++malformed_feedback_messages_;
          } else if (read_u32(bytes, offset + 8U) == media_ssrc_) {
            const auto field_count =
                (packet_bytes - kRtcpFeedbackHeaderBytes) / kRtcpNackFieldBytes;
            std::vector<GenericNackField> fields;
            fields.reserve(std::min(field_count, kMaximumParsedNackFields));
            for (std::size_t index = 0U; index < std::min(field_count, kMaximumParsedNackFields);
                 ++index) {
              const auto field_offset =
                  offset + kRtcpFeedbackHeaderBytes + index * kRtcpNackFieldBytes;
              fields.push_back({.packet_id = read_u16(bytes, field_offset),
                                .lost_packet_bitmask = read_u16(bytes, field_offset + 2U)});
            }
            const auto decision =
                recovery_.handle_nack(media_epoch_, dependency_epoch_, fields, now_milliseconds_());
            for (const auto &packet : decision.retransmissions) {
              retransmissions.push_back(to_rtc_message(packet));
            }
            request_idr = request_idr || decision.request_idr_with_parameter_sets;
            terminate = terminate || decision.terminate_session;
          }
        } else if (payload_type == 206U && format == 1U) {
          if (packet_bytes != kRtcpFeedbackHeaderBytes) {
            ++malformed_feedback_messages_;
          } else if (read_u32(bytes, offset + 8U) == media_ssrc_) {
            const auto decision =
                recovery_.handle_pli(media_epoch_, dependency_epoch_, now_milliseconds_());
            request_idr = request_idr || decision.request_idr_with_parameter_sets;
            terminate = terminate || decision.terminate_session;
          }
        }
        offset += packet_bytes;
      }
    }
    if (terminate && termination_notified_) {
      terminate = false;
    } else if (terminate) {
      termination_notified_ = true;
    }
  }

  for (auto &message : retransmissions) {
    send(std::move(message));
  }
  if (request_idr) {
    request_idr_with_parameter_sets_();
  }
  if (terminate) {
    terminate_session_();
  }
}

RecoveryDiagnostics BoundedNackResponder::diagnostics() const {
  std::lock_guard lock(mutex_);
  return recovery_.diagnostics();
}

RetransmissionCacheSnapshot BoundedNackResponder::cache_snapshot() const {
  std::lock_guard lock(mutex_);
  return cache_.snapshot();
}

std::uint64_t BoundedNackResponder::malformed_feedback_messages() const {
  std::lock_guard lock(mutex_);
  return malformed_feedback_messages_;
}

} // namespace glyphrelay::rtc_adapter
