#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>

namespace glyphrelay {

enum class DatagramClass {
  media,
  control,
};

enum class DatagramPath {
  direct_udp,
  turn_udp,
};

enum class DatagramIpFamily {
  ipv4,
  ipv6,
};

enum class DatagramProvenance {
  untrusted,
  libdatachannel_media,
  libdatachannel_control,
  libjuice_generated_control,
};

enum class DatagramProtocol {
  srtp,
  srtcp,
  dtls,
  stun,
  turn_channel_data,
  turn_send_indication,
  turn_control,
};

enum class MediaBoundaryReason {
  stop,
  pause,
  screen_lock,
  capture_revoked,
  permission_lost,
};

struct FinalDatagramMetadata {
  DatagramClass classification = DatagramClass::control;
  DatagramPath path = DatagramPath::direct_udp;
  DatagramIpFamily ip_family = DatagramIpFamily::ipv4;
  DatagramProvenance provenance = DatagramProvenance::untrusted;
  DatagramProtocol protocol = DatagramProtocol::stun;
  std::uint64_t media_epoch = 0;
  bool ipv4_options = false;
  bool ipv6_extension_headers = false;
  bool fragmented = false;
  bool udp_transport = true;
};

struct DatagramEgressSnapshot {
  std::uint64_t datagrams = 0;
  std::uint64_t udp_payload_bytes = 0;
  std::uint64_t ip_total_bytes = 0;
  std::uint64_t media_ip_total_bytes = 0;
  std::uint64_t control_ip_total_bytes = 0;
  std::uint64_t direct_ip_total_bytes = 0;
  std::uint64_t turn_ip_total_bytes = 0;
  std::uint64_t rejected_datagrams = 0;
  std::uint64_t failed_datagrams = 0;
  std::uint64_t short_datagrams = 0;
  bool overflowed = false;
};

enum class FinalSendDisposition {
  sent,
  rejected,
  failed,
  short_send,
  accounting_overflow,
};

struct FinalSendResult {
  FinalSendDisposition disposition = FinalSendDisposition::rejected;
  std::string reason;
  std::ptrdiff_t native_result = -1;
  std::uint64_t ip_total_bytes = 0;

  bool sent() const;
};

struct MediaBoundary {
  MediaBoundaryReason reason = MediaBoundaryReason::stop;
  std::uint64_t closed_epoch = 0;
  std::uint64_t next_epoch = 0;
};

class DatagramEgressCounter {
public:
  DatagramEgressSnapshot snapshot() const;

private:
  friend class MediaEgressGate;
  bool record_success(const FinalDatagramMetadata &metadata, std::size_t udp_payload_bytes,
                      std::uint64_t ip_total_bytes);
  void record_rejected();
  void record_failed(bool short_send);

  mutable std::mutex mutex_;
  DatagramEgressSnapshot snapshot_;
};

class MediaEgressGate {
public:
  using NativeSend = std::function<std::ptrdiff_t()>;
  using TestBarrier = std::function<void()>;

  explicit MediaEgressGate(std::uint64_t initial_media_epoch = 1U);

  FinalSendResult send_final(std::span<const std::byte> datagram,
                             const FinalDatagramMetadata &metadata, const NativeSend &native_send,
                             const TestBarrier &after_validation_test_barrier = {});
  MediaBoundary close_media(MediaBoundaryReason reason,
                            const TestBarrier &before_exclusive_lock_test_barrier = {});
  std::uint64_t resume_media();

  bool media_admission_open() const;
  std::uint64_t active_media_epoch() const;
  DatagramEgressSnapshot snapshot() const;

private:
  FinalSendResult send_after_validation(std::span<const std::byte> datagram,
                                        const FinalDatagramMetadata &metadata,
                                        const NativeSend &native_send);

  mutable std::shared_mutex gate_;
  bool media_open_ = true;
  bool terminally_closed_ = false;
  std::uint64_t active_epoch_ = 1U;
  std::uint64_t last_closed_epoch_ = 0U;
  DatagramEgressCounter counter_;
};

std::string final_send_disposition_name(FinalSendDisposition disposition);
std::string media_boundary_reason_name(MediaBoundaryReason reason);

} // namespace glyphrelay
