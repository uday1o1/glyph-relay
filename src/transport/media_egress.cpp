#include "glyphrelay/media_egress.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace glyphrelay {
namespace {

constexpr std::size_t kMaximumUdpPayloadBytes = 65'507U;

bool checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t &result) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

std::uint64_t ip_total_bytes(DatagramIpFamily family, std::size_t payload_bytes) {
  const std::uint64_t header_bytes = family == DatagramIpFamily::ipv4 ? 28U : 48U;
  return static_cast<std::uint64_t>(payload_bytes) + header_bytes;
}

std::string validate_metadata(const FinalDatagramMetadata &metadata, std::size_t payload_bytes) {
  if (payload_bytes == 0U || payload_bytes > kMaximumUdpPayloadBytes) {
    return "datagram_udp_payload_size_invalid";
  }
  if (!metadata.udp_transport) {
    return "datagram_non_udp_transport_unverified";
  }
  if (metadata.ipv4_options || metadata.ipv6_extension_headers || metadata.fragmented) {
    return "datagram_ip_header_or_fragmentation_unverified";
  }
  if ((metadata.ip_family == DatagramIpFamily::ipv4 && metadata.ipv6_extension_headers) ||
      (metadata.ip_family == DatagramIpFamily::ipv6 && metadata.ipv4_options)) {
    return "datagram_ip_family_metadata_mismatch";
  }
  if (metadata.provenance == DatagramProvenance::untrusted) {
    return "datagram_provenance_untrusted";
  }
  if (metadata.provenance == DatagramProvenance::libjuice_generated_control &&
      metadata.classification != DatagramClass::control) {
    return "datagram_generated_control_misclassified";
  }
  if (metadata.provenance == DatagramProvenance::libdatachannel_media &&
      metadata.classification != DatagramClass::media) {
    return "datagram_media_provenance_misclassified";
  }
  if (metadata.provenance == DatagramProvenance::libdatachannel_control &&
      metadata.classification != DatagramClass::control) {
    return "datagram_control_provenance_misclassified";
  }
  if (metadata.classification == DatagramClass::media) {
    const bool valid_media_protocol =
        metadata.protocol == DatagramProtocol::srtp ||
        (metadata.path == DatagramPath::turn_udp &&
         (metadata.protocol == DatagramProtocol::turn_channel_data ||
          metadata.protocol == DatagramProtocol::turn_send_indication));
    if (!valid_media_protocol) {
      return "datagram_media_protocol_invalid";
    }
  } else if (metadata.protocol == DatagramProtocol::srtp) {
    return "datagram_srtp_must_be_media";
  }
  if (metadata.path == DatagramPath::direct_udp &&
      (metadata.protocol == DatagramProtocol::turn_channel_data ||
       metadata.protocol == DatagramProtocol::turn_send_indication ||
       metadata.protocol == DatagramProtocol::turn_control)) {
    return "datagram_turn_protocol_on_direct_path";
  }
  return {};
}

} // namespace

bool FinalSendResult::sent() const { return disposition == FinalSendDisposition::sent; }

DatagramEgressSnapshot DatagramEgressCounter::snapshot() const {
  const std::scoped_lock lock(mutex_);
  return snapshot_;
}

bool DatagramEgressCounter::record_success(const FinalDatagramMetadata &metadata,
                                           std::size_t udp_payload_bytes,
                                           std::uint64_t ip_total_bytes_value) {
  const std::scoped_lock lock(mutex_);
  auto candidate = snapshot_;
  const auto add = [&candidate](std::uint64_t &field, std::uint64_t value) {
    std::uint64_t next = 0U;
    if (!checked_add(field, value, next)) {
      candidate.overflowed = true;
      return false;
    }
    field = next;
    return true;
  };
  if (!add(candidate.datagrams, 1U) ||
      !add(candidate.udp_payload_bytes, static_cast<std::uint64_t>(udp_payload_bytes)) ||
      !add(candidate.ip_total_bytes, ip_total_bytes_value) ||
      !(metadata.classification == DatagramClass::media
            ? add(candidate.media_ip_total_bytes, ip_total_bytes_value)
            : add(candidate.control_ip_total_bytes, ip_total_bytes_value)) ||
      !(metadata.path == DatagramPath::direct_udp
            ? add(candidate.direct_ip_total_bytes, ip_total_bytes_value)
            : add(candidate.turn_ip_total_bytes, ip_total_bytes_value))) {
    snapshot_.overflowed = true;
    return false;
  }
  snapshot_ = candidate;
  return true;
}

void DatagramEgressCounter::record_rejected() {
  const std::scoped_lock lock(mutex_);
  if (snapshot_.rejected_datagrams == std::numeric_limits<std::uint64_t>::max()) {
    snapshot_.overflowed = true;
  } else {
    ++snapshot_.rejected_datagrams;
  }
}

void DatagramEgressCounter::record_failed(bool short_send) {
  const std::scoped_lock lock(mutex_);
  auto &field = short_send ? snapshot_.short_datagrams : snapshot_.failed_datagrams;
  if (field == std::numeric_limits<std::uint64_t>::max()) {
    snapshot_.overflowed = true;
  } else {
    ++field;
  }
}

MediaEgressGate::MediaEgressGate(std::uint64_t initial_media_epoch)
    : active_epoch_(initial_media_epoch) {
  if (initial_media_epoch == 0U) {
    throw std::invalid_argument("media epoch zero is reserved");
  }
}

FinalSendResult MediaEgressGate::send_final(std::span<const std::byte> datagram,
                                            const FinalDatagramMetadata &metadata,
                                            const NativeSend &native_send,
                                            const TestBarrier &after_validation_test_barrier) {
  const auto invalid_reason = validate_metadata(metadata, datagram.size());
  if (!invalid_reason.empty() || !native_send) {
    counter_.record_rejected();
    return {FinalSendDisposition::rejected,
            invalid_reason.empty() ? "datagram_native_send_missing" : invalid_reason, -1, 0U};
  }
  if (metadata.classification == DatagramClass::control) {
    return send_after_validation(datagram, metadata, native_send);
  }

  const std::shared_lock permit(gate_);
  if (!media_open_ || metadata.media_epoch != active_epoch_) {
    counter_.record_rejected();
    return {FinalSendDisposition::rejected, "media_epoch_not_admitted", -1, 0U};
  }
  if (after_validation_test_barrier) {
    after_validation_test_barrier();
  }
  return send_after_validation(datagram, metadata, native_send);
}

MediaBoundary MediaEgressGate::close_media(MediaBoundaryReason reason,
                                           const TestBarrier &before_exclusive_lock_test_barrier) {
  if (before_exclusive_lock_test_barrier) {
    before_exclusive_lock_test_barrier();
  }
  const std::unique_lock lock(gate_);
  if (media_open_) {
    last_closed_epoch_ = active_epoch_;
    media_open_ = false;
    if (active_epoch_ != std::numeric_limits<std::uint64_t>::max()) {
      ++active_epoch_;
    }
  }
  terminally_closed_ = terminally_closed_ || reason != MediaBoundaryReason::pause;
  return {reason, last_closed_epoch_, active_epoch_};
}

std::uint64_t MediaEgressGate::resume_media() {
  const std::unique_lock lock(gate_);
  if (terminally_closed_ || active_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    return 0U;
  }
  media_open_ = true;
  return active_epoch_;
}

bool MediaEgressGate::media_admission_open() const {
  const std::shared_lock lock(gate_);
  return media_open_;
}

std::uint64_t MediaEgressGate::active_media_epoch() const {
  const std::shared_lock lock(gate_);
  return active_epoch_;
}

DatagramEgressSnapshot MediaEgressGate::snapshot() const { return counter_.snapshot(); }

FinalSendResult MediaEgressGate::send_after_validation(std::span<const std::byte> datagram,
                                                       const FinalDatagramMetadata &metadata,
                                                       const NativeSend &native_send) {
  std::ptrdiff_t native_result = -1;
  try {
    native_result = native_send();
  } catch (...) {
    counter_.record_failed(false);
    return {FinalSendDisposition::failed, "datagram_native_send_threw", -1, 0U};
  }
  if (native_result < 0) {
    counter_.record_failed(false);
    return {FinalSendDisposition::failed, "datagram_native_send_failed", native_result, 0U};
  }
  if (static_cast<std::size_t>(native_result) != datagram.size()) {
    counter_.record_failed(true);
    return {FinalSendDisposition::short_send, "datagram_native_send_short", native_result, 0U};
  }
  const auto total_bytes = ip_total_bytes(metadata.ip_family, datagram.size());
  if (!counter_.record_success(metadata, datagram.size(), total_bytes)) {
    return {FinalSendDisposition::accounting_overflow, "datagram_accounting_overflow",
            native_result, 0U};
  }
  return {FinalSendDisposition::sent, "datagram_sent_and_counted", native_result, total_bytes};
}

std::string final_send_disposition_name(FinalSendDisposition disposition) {
  switch (disposition) {
  case FinalSendDisposition::sent:
    return "sent";
  case FinalSendDisposition::rejected:
    return "rejected";
  case FinalSendDisposition::failed:
    return "failed";
  case FinalSendDisposition::short_send:
    return "short_send";
  case FinalSendDisposition::accounting_overflow:
    return "accounting_overflow";
  }
  return "unknown";
}

std::string media_boundary_reason_name(MediaBoundaryReason reason) {
  switch (reason) {
  case MediaBoundaryReason::stop:
    return "stop";
  case MediaBoundaryReason::pause:
    return "pause";
  case MediaBoundaryReason::screen_lock:
    return "screen_lock";
  case MediaBoundaryReason::capture_revoked:
    return "capture_revoked";
  case MediaBoundaryReason::permission_lost:
    return "permission_lost";
  }
  return "unknown";
}

} // namespace glyphrelay
