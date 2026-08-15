#include "glyphrelay/peer_sender.hpp"

#include <nlohmann/json.hpp>

#include <rtc/candidate.hpp>
#include <rtc/configuration.hpp>
#include <rtc/datachannel.hpp>
#include <rtc/description.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/track.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

constexpr std::string_view kSession = "abcdefghijklmnopqrstuv";

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

template <typename Predicate>
bool wait_for(std::mutex &mutex, std::condition_variable &changed,
              std::chrono::milliseconds timeout, Predicate predicate) {
  std::unique_lock lock(mutex);
  return changed.wait_for(lock, timeout, predicate);
}

std::string candidate_json(const rtc::Candidate &candidate) {
  return Json({{"candidate", candidate.candidate()}, {"sdpMid", candidate.mid()}}).dump();
}

glyphrelay::RecordedAccessUnit recovery_access_unit() {
  const auto bytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{
      0x00U, 0x00U, 0x00U, 0x01U, 0x67U, 0x42U, 0xC0U, 0x1FU, 0x55U, 0x00U, 0x00U, 0x01U, 0x68U,
      0xCEU, 0x06U, 0xE2U, 0x00U, 0x00U, 0x00U, 0x01U, 0x65U, 0x88U, 0x84U, 0x21U, 0xA0U});
  return {
      .bytes = bytes,
      .media_epoch = 1U,
      .dependency_epoch = 1U,
      .geometry_epoch = 1U,
      .encoder_configuration_epoch = 1U,
      .configuration_sha256 = std::string(64U, 'a'),
      .source_frame_id = 1U,
      .extended_rtp_timestamp = 90'000U,
      .picture_type = glyphrelay::RecordingPictureType::idr,
      .keyframe = true,
      .parameter_sets_present = true,
      .presentation_timestamp_ns = 1U,
  };
}

void test_loopback_peer_control_media_and_cleanup() {
  std::mutex mutex;
  std::condition_variable changed;
  std::string failure;
  std::string offer_sdp;
  std::size_t received_rtp_packets = 0U;
  std::size_t received_stats_events = 0U;
  std::size_t recovery_requests = 0U;
  std::size_t ended_events = 0U;
  bool receiver_connected = false;
  bool receiver_control_open = false;
  bool sender_offer_accepted = false;
  std::atomic<std::uint64_t> receiver_sequence = 0U;
  std::vector<std::string> pending_receiver_candidates;

  rtc::Configuration receiver_configuration;
  receiver_configuration.bindAddress = "127.0.0.1";
  receiver_configuration.enableIceTcp = false;
  receiver_configuration.enableIceUdpMux = false;
  receiver_configuration.disableAutoNegotiation = true;
  receiver_configuration.maxMessageSize = glyphrelay::kMaximumControlBytes;
  rtc::PeerConnection receiver(receiver_configuration);

  std::unique_ptr<glyphrelay::PeerSender> sender;
  receiver.onStateChange([&](rtc::PeerConnection::State state) {
    {
      std::scoped_lock lock(mutex);
      if (state == rtc::PeerConnection::State::Connected) {
        receiver_connected = true;
      } else if (state == rtc::PeerConnection::State::Failed && failure.empty()) {
        failure = "receiver_peer_failed";
      }
    }
    changed.notify_all();
  });
  receiver.onLocalDescription([&](rtc::Description description) {
    {
      std::scoped_lock lock(mutex);
      offer_sdp = static_cast<std::string>(description);
    }
    changed.notify_all();
  });
  receiver.onLocalCandidate([&](rtc::Candidate candidate) {
    const auto encoded = candidate_json(candidate);
    {
      std::scoped_lock lock(mutex);
      if (!sender_offer_accepted) {
        pending_receiver_candidates.push_back(encoded);
        return;
      }
    }
    if (sender && !sender->add_remote_candidate(encoded)) {
      std::scoped_lock lock(mutex);
      if (failure.empty()) {
        failure = "sender_rejected_receiver_candidate";
      }
      changed.notify_all();
    }
  });

  rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
  media.addH264Codec(102, "profile-level-id=42e01f;packetization-mode=1;"
                          "level-asymmetry-allowed=1");
  auto receiver_track = receiver.addTrack(media);
  receiver_track->onMessage(
      [&](rtc::binary bytes) {
        {
          std::scoped_lock lock(mutex);
          if (!bytes.empty()) {
            ++received_rtp_packets;
          }
        }
        changed.notify_all();
      },
      [&](std::string) {
        std::scoped_lock lock(mutex);
        if (failure.empty()) {
          failure = "receiver_track_text_unexpected";
        }
        changed.notify_all();
      });

  auto receiver_control = receiver.createDataChannel(std::string(glyphrelay::kControlProtocol));
  receiver_control->onOpen([&] {
    {
      std::scoped_lock lock(mutex);
      receiver_control_open = true;
    }
    changed.notify_all();
  });
  receiver_control->onMessage(
      [&](rtc::binary) {
        std::scoped_lock lock(mutex);
        if (failure.empty()) {
          failure = "receiver_control_binary_unexpected";
        }
        changed.notify_all();
      },
      [&](std::string encoded) {
        try {
          const auto message = Json::parse(encoded);
          const auto type = message.at("type").get<std::string>();
          Json response;
          if (type == "CLOCK_REQUEST") {
            response = {
                {"protocolVersion", glyphrelay::kControlProtocol},
                {"receiverReceiveTimeMs", 10.0},
                {"receiverSendTimeMs", 11.0},
                {"requestSequence", message.at("sequence")},
                {"senderSendTimeMs", message.at("senderSendTimeMs")},
                {"sequence", receiver_sequence.fetch_add(1U) + 1U},
                {"sessionId", kSession},
                {"type", "CLOCK_RESPONSE"},
            };
          } else if (type == "SESSION_ENDED") {
            response = {
                {"protocolVersion", glyphrelay::kControlProtocol},
                {"requestSequence", message.at("sequence")},
                {"sequence", receiver_sequence.fetch_add(1U) + 1U},
                {"sessionId", kSession},
                {"type", "SESSION_ENDED_ACK"},
            };
          } else if (type != "HELLO") {
            throw std::runtime_error("unexpected sender control type: " + type);
          }
          if (!response.is_null() && !receiver_control->send(response.dump())) {
            throw std::runtime_error("receiver control response send failed");
          }
        } catch (const std::exception &error) {
          std::scoped_lock lock(mutex);
          if (failure.empty()) {
            failure = error.what();
          }
          changed.notify_all();
        }
      });

  sender = std::make_unique<glyphrelay::PeerSender>(glyphrelay::PeerSenderConfig{
      .session_id = std::string(kSession),
      .bind_address = "127.0.0.1",
      .ice_server_urls = {},
      .event_callback =
          [&](const glyphrelay::PeerSenderEvent &event) {
            try {
              if (event.kind == glyphrelay::PeerSenderEventKind::local_description) {
                receiver.setRemoteDescription(rtc::Description(event.value, "answer"));
              } else if (event.kind == glyphrelay::PeerSenderEventKind::local_candidate) {
                const auto decoded = Json::parse(event.value);
                receiver.addRemoteCandidate(
                    rtc::Candidate(decoded.at("candidate").get<std::string>(),
                                   decoded.at("sdpMid").get<std::string>()));
              }
              {
                std::scoped_lock lock(mutex);
                if (event.kind == glyphrelay::PeerSenderEventKind::receiver_stats) {
                  ++received_stats_events;
                } else if (event.kind == glyphrelay::PeerSenderEventKind::recovery_requested) {
                  ++recovery_requests;
                } else if (event.kind == glyphrelay::PeerSenderEventKind::ended) {
                  ++ended_events;
                } else if (event.kind == glyphrelay::PeerSenderEventKind::failed &&
                           failure.empty()) {
                  failure = event.value;
                }
              }
              changed.notify_all();
            } catch (const std::exception &error) {
              std::scoped_lock lock(mutex);
              if (failure.empty()) {
                failure = error.what();
              }
              changed.notify_all();
            }
          },
      .request_idr_with_parameter_sets =
          [&] {
            {
              std::scoped_lock lock(mutex);
              ++recovery_requests;
            }
            changed.notify_all();
          },
  });

  receiver.setLocalDescription(rtc::Description::Type::Offer);
  require(wait_for(mutex, changed, std::chrono::seconds(5),
                   [&] { return !offer_sdp.empty() || !failure.empty(); }) &&
              failure.empty(),
          "receiver must produce one loopback H.264 offer");
  require(sender->accept_offer(offer_sdp),
          "native sender must accept the compatible loopback receiver offer");
  std::vector<std::string> buffered_candidates;
  {
    std::scoped_lock lock(mutex);
    sender_offer_accepted = true;
    buffered_candidates.swap(pending_receiver_candidates);
  }
  for (const auto &candidate : buffered_candidates) {
    require(sender->add_remote_candidate(candidate),
            "sender must accept receiver candidates after the ordered offer");
  }
  const bool peer_ready = wait_for(mutex, changed, std::chrono::seconds(10), [&] {
    const auto diagnostics = sender->diagnostics();
    return !failure.empty() ||
           (receiver_connected && receiver_control_open && diagnostics.connected &&
            diagnostics.track_open && diagnostics.control_open &&
            diagnostics.control.receive_sequence >= 5U);
  });
  if (!failure.empty()) {
    std::cerr << "peer integration failure: " << failure << '\n';
  }
  require(peer_ready && failure.empty(),
          "both loopback peers, the video track, and reliable control channel must open");

  sender->set_pacing_target_bits_per_second(1'000'000.0);
  require(sender->diagnostics().pacer.target_bits_per_second == 1'000'000.0 &&
              sender->diagnostics().pacer.maximum_age_milliseconds == 100U &&
              sender->diagnostics().pacer.maximum_bytes == 4U * 1024U * 1024U,
          "the real peer path must expose the configured pacing target and frozen hard bounds");

  Json stats = {
      {"compositorFrames", 1U},
      {"decodedFrames", 1U},
      {"droppedFrames", 0U},
      {"latestPresentedRtpTimestamp", nullptr},
      {"protocolVersion", glyphrelay::kControlProtocol},
      {"sequence", receiver_sequence.fetch_add(1U) + 1U},
      {"sessionId", kSession},
      {"type", "RECEIVER_STATS"},
  };
  require(receiver_control->send(stats.dump()),
          "receiver must send one bounded statistics message through SCTP");
  require(wait_for(mutex, changed, std::chrono::seconds(2),
                   [&] { return received_stats_events == 1U || !failure.empty(); }) &&
              failure.empty(),
          "sender must surface one validated receiver-statistics event");

  const bool access_unit_sent = sender->send_access_unit(recovery_access_unit());
  if (!access_unit_sent) {
    std::cerr << "peer access-unit rejection: " << sender->diagnostics().reason << '\n';
  }
  require(access_unit_sent, "sender must packetize and send one complete recovery access unit");
  require(wait_for(mutex, changed, std::chrono::seconds(2),
                   [&] { return received_rtp_packets > 0U || !failure.empty(); }) &&
              failure.empty(),
          "receiver must observe protected loopback RTP for the recovery access unit");
  const auto before_stop = sender->diagnostics();
  require(
      before_stop.access_units_sent == 1U && before_stop.egress.media_ip_total_bytes > 0U &&
          before_stop.retransmission_cache.packets > 0U && recovery_requests > 0U,
      "peer diagnostics must prove send, final media egress, bounded cache, and recovery state");

  sender->stop("OWNER_STOP");
  const auto stopped = sender->diagnostics();
  require(stopped.stopped && !stopped.connected && !stopped.track_open && !stopped.control_open &&
              stopped.retransmission_cache.packets == 0U && ended_events == 1U && failure.empty(),
          "sender stop must close the peer and clear track, control, and retransmission resources");
  receiver_control->resetCallbacks();
  receiver_track->resetCallbacks();
  receiver.resetCallbacks();
  receiver_control->close();
  receiver_track->close();
  receiver.close();
}

void test_invalid_candidate_and_transport_configuration_fail_closed() {
  bool invalid_turn_rejected = false;
  try {
    glyphrelay::PeerSender invalid({
        .session_id = std::string(kSession),
        .bind_address = "127.0.0.1",
        .ice_server_urls = {"turns://user:password@turn.example.test:5349"},
        .event_callback = [](const glyphrelay::PeerSenderEvent &) {},
        .request_idr_with_parameter_sets = [] {},
    });
    static_cast<void>(invalid);
  } catch (const std::exception &) {
    invalid_turn_rejected = true;
  }
  require(invalid_turn_rejected, "TURN TCP or TLS must not enter the verified UDP-only path");

  glyphrelay::PeerSender sender({
      .session_id = std::string(kSession),
      .bind_address = "127.0.0.1",
      .ice_server_urls = {},
      .event_callback = [](const glyphrelay::PeerSenderEvent &) {},
      .request_idr_with_parameter_sets = [] {},
  });
  const std::string duplicate =
      R"({"candidate":"candidate:1 1 UDP 1 127.0.0.1 9 typ host","sdpMid":"0","sdpMid":"0"})";
  require(!sender.add_remote_candidate(duplicate) &&
              sender.diagnostics().reason == "PEER_REMOTE_CANDIDATE_INVALID" &&
              sender.diagnostics().egress.rejected_datagrams == 0U,
          "duplicate candidate keys must fail closed before reaching ICE");
}

} // namespace

int main() {
  test_loopback_peer_control_media_and_cleanup();
  test_invalid_candidate_and_transport_configuration_fail_closed();
  std::cout << "native peer sender tests passed\n";
  return 0;
}
