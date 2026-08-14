#include "glyphrelay/media_egress.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

glyphrelay::FinalDatagramMetadata direct_media(std::uint64_t epoch) {
  return {
      .classification = glyphrelay::DatagramClass::media,
      .path = glyphrelay::DatagramPath::direct_udp,
      .ip_family = glyphrelay::DatagramIpFamily::ipv4,
      .provenance = glyphrelay::DatagramProvenance::libdatachannel_media,
      .protocol = glyphrelay::DatagramProtocol::srtp,
      .media_epoch = epoch,
  };
}

glyphrelay::FinalDatagramMetadata direct_control() {
  return {
      .classification = glyphrelay::DatagramClass::control,
      .path = glyphrelay::DatagramPath::direct_udp,
      .ip_family = glyphrelay::DatagramIpFamily::ipv6,
      .provenance = glyphrelay::DatagramProvenance::libdatachannel_control,
      .protocol = glyphrelay::DatagramProtocol::dtls,
  };
}

std::vector<std::byte> bytes(std::size_t size) { return std::vector<std::byte>(size); }

void test_exact_accounting_and_failure_rules() {
  glyphrelay::MediaEgressGate gate(3U);
  const auto media = bytes(100U);
  auto media_metadata = direct_media(3U);
  const auto media_result = gate.send_final(
      media, media_metadata, [&media]() { return static_cast<std::ptrdiff_t>(media.size()); });
  require(media_result.sent() && media_result.ip_total_bytes == 128U,
          "IPv4 accounting must add one UDP and base IPv4 header");

  const auto control = bytes(50U);
  const auto control_result = gate.send_final(control, direct_control(), [&control]() {
    return static_cast<std::ptrdiff_t>(control.size());
  });
  require(control_result.sent() && control_result.ip_total_bytes == 98U,
          "IPv6 accounting must add one UDP and base IPv6 header");

  const auto turn = bytes(120U);
  auto turn_metadata = direct_media(3U);
  turn_metadata.path = glyphrelay::DatagramPath::turn_udp;
  turn_metadata.protocol = glyphrelay::DatagramProtocol::turn_channel_data;
  const auto turn_result = gate.send_final(
      turn, turn_metadata, [&turn]() { return static_cast<std::ptrdiff_t>(turn.size()); });
  require(turn_result.sent() && turn_result.ip_total_bytes == 148U,
          "TURN accounting must count the final wrapped UDP payload exactly once");

  const auto snapshot = gate.snapshot();
  require(snapshot.datagrams == 3U && snapshot.udp_payload_bytes == 270U &&
              snapshot.ip_total_bytes == 374U && snapshot.media_ip_total_bytes == 276U &&
              snapshot.control_ip_total_bytes == 98U && snapshot.direct_ip_total_bytes == 226U &&
              snapshot.turn_ip_total_bytes == 148U,
          "egress totals and class/path breakdowns must match exact IP lengths");

  const auto failed = gate.send_final(media, media_metadata, []() { return -1; });
  const auto short_send = gate.send_final(media, media_metadata, []() { return 99; });
  const auto threw = gate.send_final(media, media_metadata, []() -> std::ptrdiff_t {
    throw std::runtime_error("seeded native send exception");
  });
  const auto after_failures = gate.snapshot();
  require(failed.disposition == glyphrelay::FinalSendDisposition::failed &&
              short_send.disposition == glyphrelay::FinalSendDisposition::short_send &&
              threw.disposition == glyphrelay::FinalSendDisposition::failed &&
              after_failures.datagrams == 3U && after_failures.failed_datagrams == 2U &&
              after_failures.short_datagrams == 1U,
          "failed, thrown, and short native sends must add no wire egress");
}

void test_classifier_and_verified_path_rejection() {
  bool zero_epoch_rejected = false;
  try {
    const glyphrelay::MediaEgressGate invalid_gate(0U);
  } catch (const std::invalid_argument &) {
    zero_epoch_rejected = true;
  }
  require(zero_epoch_rejected, "media epoch zero must be rejected at construction");

  glyphrelay::MediaEgressGate gate(9U);
  const auto payload = bytes(64U);
  std::size_t native_calls = 0U;
  const auto attempt = [&gate, &payload, &native_calls](auto metadata) {
    return gate.send_final(payload, metadata, [&payload, &native_calls]() {
      ++native_calls;
      return static_cast<std::ptrdiff_t>(payload.size());
    });
  };

  auto metadata = direct_media(9U);
  metadata.provenance = glyphrelay::DatagramProvenance::untrusted;
  require(attempt(metadata).reason == "datagram_provenance_untrusted",
          "untrusted classification provenance must fail closed");

  metadata = direct_media(9U);
  metadata.classification = glyphrelay::DatagramClass::control;
  require(attempt(metadata).reason == "datagram_media_provenance_misclassified",
          "authenticated media provenance may not be relabeled control");

  metadata = direct_media(9U);
  metadata.fragmented = true;
  require(attempt(metadata).reason == "datagram_ip_header_or_fragmentation_unverified",
          "fragmented UDP cannot enter verified-cap mode");

  metadata = direct_media(9U);
  metadata.ipv4_options = true;
  require(attempt(metadata).reason == "datagram_ip_header_or_fragmentation_unverified",
          "IPv4 options cannot enter the base-header accounting contract");

  metadata = direct_media(9U);
  metadata.udp_transport = false;
  require(attempt(metadata).reason == "datagram_non_udp_transport_unverified",
          "TURN TCP or TLS cannot enter verified-cap mode");

  metadata = direct_media(9U);
  metadata.protocol = glyphrelay::DatagramProtocol::dtls;
  require(attempt(metadata).reason == "datagram_media_protocol_invalid",
          "DTLS may not be classified as media");

  metadata = direct_media(8U);
  require(attempt(metadata).reason == "media_epoch_not_admitted",
          "a stale media epoch must fail immediately before the native send");

  auto generated = direct_control();
  generated.provenance = glyphrelay::DatagramProvenance::libjuice_generated_control;
  generated.protocol = glyphrelay::DatagramProtocol::stun;
  require(attempt(generated).sent(), "libjuice-generated STUN must remain authenticated control");
  require(native_calls == 1U && gate.snapshot().rejected_datagrams == 7U,
          "only the correctly classified control fixture may cross the native boundary");
}

void test_linearizable_revocation_and_control_bypass() {
  glyphrelay::MediaEgressGate gate(41U);
  const auto payload = bytes(80U);
  const auto metadata = direct_media(41U);
  std::latch media_validated(1U);
  std::latch release_media_send(1U);
  std::latch revoke_about_to_lock(1U);
  std::atomic_uint64_t event_sequence = 0U;
  std::atomic_uint64_t media_send_event = 0U;
  std::atomic_uint64_t revoke_event = 0U;

  auto media_future = std::async(std::launch::async, [&]() {
    return gate.send_final(
        payload, metadata,
        [&]() {
          media_send_event = ++event_sequence;
          return static_cast<std::ptrdiff_t>(payload.size());
        },
        [&]() {
          media_validated.count_down();
          release_media_send.wait();
        });
  });
  media_validated.wait();

  auto revoke_future = std::async(std::launch::async, [&]() {
    const auto boundary = gate.close_media(glyphrelay::MediaBoundaryReason::pause,
                                           [&]() { revoke_about_to_lock.count_down(); });
    revoke_event = ++event_sequence;
    return boundary;
  });
  revoke_about_to_lock.wait();
  require(revoke_future.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
          "revocation must wait for the validated final send to return");

  const auto control = bytes(32U);
  const auto control_result = gate.send_final(control, direct_control(), [&control]() {
    return static_cast<std::ptrdiff_t>(control.size());
  });
  require(control_result.sent(), "control traffic must not wait behind the media revocation gate");

  release_media_send.count_down();
  const auto media_result = media_future.get();
  const auto boundary = revoke_future.get();
  require(media_result.sent() && media_send_event == 1U && revoke_event == 2U &&
              boundary.closed_epoch == 41U && boundary.next_epoch == 42U &&
              !gate.media_admission_open(),
          "the last admitted send must linearize before the closing epoch boundary");

  std::size_t stale_native_calls = 0U;
  const auto stale = gate.send_final(payload, metadata, [&]() {
    ++stale_native_calls;
    return static_cast<std::ptrdiff_t>(payload.size());
  });
  require(stale.disposition == glyphrelay::FinalSendDisposition::rejected &&
              stale_native_calls == 0U,
          "no old-epoch media native send may begin after the boundary");
  require(gate.resume_media() == 42U && gate.media_admission_open(),
          "pause may resume only in the advanced media epoch");
  auto resumed = metadata;
  resumed.media_epoch = 42U;
  require(gate.send_final(payload, resumed,
                          [&payload]() { return static_cast<std::ptrdiff_t>(payload.size()); })
              .sent(),
          "the resumed epoch must admit fresh media");

  const auto stopped = gate.close_media(glyphrelay::MediaBoundaryReason::screen_lock);
  require(stopped.closed_epoch == 42U && gate.resume_media() == 0U && !gate.media_admission_open(),
          "screen lock must create a terminal non-resumable media boundary");
  const auto repeated = gate.close_media(glyphrelay::MediaBoundaryReason::stop);
  require(repeated.closed_epoch == 42U && repeated.next_epoch == 43U,
          "repeated terminal closure must be idempotent for the media epoch");
}

void test_every_terminal_boundary_reason_linearizes() {
  constexpr std::array terminal_reasons{
      glyphrelay::MediaBoundaryReason::stop,
      glyphrelay::MediaBoundaryReason::screen_lock,
      glyphrelay::MediaBoundaryReason::capture_revoked,
      glyphrelay::MediaBoundaryReason::permission_lost,
  };
  std::uint64_t epoch = 70U;
  for (const auto reason : terminal_reasons) {
    glyphrelay::MediaEgressGate gate(epoch);
    const auto payload = bytes(48U);
    const auto metadata = direct_media(epoch);
    std::latch media_validated(1U);
    std::latch release_media_send(1U);
    std::latch boundary_about_to_lock(1U);
    std::atomic_bool native_send_called = false;

    auto media_future = std::async(std::launch::async, [&]() {
      return gate.send_final(
          payload, metadata,
          [&]() {
            native_send_called = true;
            return static_cast<std::ptrdiff_t>(payload.size());
          },
          [&]() {
            media_validated.count_down();
            release_media_send.wait();
          });
    });
    media_validated.wait();
    auto boundary_future = std::async(std::launch::async, [&]() {
      return gate.close_media(reason, [&]() { boundary_about_to_lock.count_down(); });
    });
    boundary_about_to_lock.wait();
    require(boundary_future.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
            "every terminal boundary must wait for a validated final send");

    release_media_send.count_down();
    const auto send_result = media_future.get();
    const auto boundary = boundary_future.get();
    require(send_result.sent() && native_send_called && boundary.reason == reason &&
                boundary.closed_epoch == epoch && boundary.next_epoch == epoch + 1U &&
                gate.resume_media() == 0U && !gate.media_admission_open(),
            "each terminal reason must close and advance the admitted media epoch");
    ++epoch;
  }
}

} // namespace

int main() {
  test_exact_accounting_and_failure_rules();
  test_classifier_and_verified_path_rejection();
  test_linearizable_revocation_and_control_bypass();
  test_every_terminal_boundary_reason_linearizes();
  return 0;
}
