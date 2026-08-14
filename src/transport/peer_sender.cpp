#include "glyphrelay/peer_sender.hpp"

#include "glyphrelay/recording_profile.hpp"
#include "glyphrelay_media_handlers.hpp"

#include <nlohmann/json.hpp>

#include <rtc/candidate.hpp>
#include <rtc/configuration.hpp>
#include <rtc/datachannel.hpp>
#include <rtc/description.hpp>
#include <rtc/message.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/rembhandler.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/track.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace glyphrelay {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kPresentation = "720p30";
constexpr std::string_view kControlLabel = "glyphrelay-control-v1";
constexpr std::uint32_t kMediaSsrc = 0x47524C59U;
constexpr std::uint64_t kInitialExtendedSequence = 1U;
constexpr std::uint64_t kInitialMediaEpoch = 1U;
constexpr std::chrono::milliseconds kWorkerInterval{100};
constexpr std::chrono::seconds kControlEndTimeout{2};
constexpr std::size_t kMaximumCandidateBytes = 4U * 1024U;

void append_control(SenderControlOutput &destination, SenderControlOutput source) {
  destination.valid = destination.valid && source.valid;
  destination.close_channel = destination.close_channel || source.close_channel;
  for (auto &message : source.outbound_messages) {
    destination.outbound_messages.push_back(std::move(message));
  }
  for (auto &event : source.events) {
    destination.events.push_back(std::move(event));
  }
}

std::uint8_t select_payload_type(const RecordingProfileCompatibility &offer) {
  const auto selected =
      std::find_if(offer.formats.begin(), offer.formats.end(), [](const auto &format) {
        return format.profile_level.family == H264ProfileFamily::constrained_baseline &&
               format.profile_level.level_idc >= 31U && format.packetization_mode == 1U &&
               format.level_asymmetry_allowed;
      });
  if (selected == offer.formats.end() || selected->payload_type > 127U) {
    throw std::invalid_argument("peer_offer_payload_type_invalid");
  }
  return static_cast<std::uint8_t>(selected->payload_type);
}

void configure_track_description(rtc::Track &track, std::uint8_t payload_type) {
  auto media = track.description();
  if (media.type() != "video" || media.direction() != rtc::Description::Direction::SendOnly) {
    throw std::invalid_argument("peer_offer_track_direction_invalid");
  }
  const auto payloads = media.payloadTypes();
  if (std::find(payloads.begin(), payloads.end(), payload_type) == payloads.end()) {
    throw std::invalid_argument("peer_offer_payload_missing_from_track");
  }
  for (const auto payload : payloads) {
    if (payload != payload_type) {
      media.removeRtpMap(payload);
    }
  }
  media.clearSSRCs();
  media.addSSRC(kMediaSsrc, "glyphrelay", "glyphrelay-stream", "glyphrelay-video");
  track.setDescription(std::move(media));
}

DatagramProtocol map_protocol(rtc::FinalUdpDatagramProtocol protocol) {
  switch (protocol) {
  case rtc::FinalUdpDatagramProtocol::Srtp:
    return DatagramProtocol::srtp;
  case rtc::FinalUdpDatagramProtocol::Srtcp:
    return DatagramProtocol::srtcp;
  case rtc::FinalUdpDatagramProtocol::Dtls:
    return DatagramProtocol::dtls;
  case rtc::FinalUdpDatagramProtocol::Stun:
    return DatagramProtocol::stun;
  case rtc::FinalUdpDatagramProtocol::TurnChannelData:
    return DatagramProtocol::turn_channel_data;
  case rtc::FinalUdpDatagramProtocol::TurnSendIndication:
    return DatagramProtocol::turn_send_indication;
  case rtc::FinalUdpDatagramProtocol::TurnControl:
    return DatagramProtocol::turn_control;
  case rtc::FinalUdpDatagramProtocol::UnknownControl:
    return DatagramProtocol::unknown_control;
  }
  return DatagramProtocol::unknown_control;
}

DatagramProvenance map_provenance(rtc::FinalUdpEgressClass egress_class,
                                  rtc::FinalUdpDatagramProtocol protocol) {
  if (egress_class == rtc::FinalUdpEgressClass::Media) {
    return DatagramProvenance::libdatachannel_media;
  }
  if (protocol == rtc::FinalUdpDatagramProtocol::Stun ||
      protocol == rtc::FinalUdpDatagramProtocol::TurnControl ||
      protocol == rtc::FinalUdpDatagramProtocol::UnknownControl) {
    return DatagramProvenance::libjuice_generated_control;
  }
  return DatagramProvenance::libdatachannel_control;
}

bool exact_candidate_keys(const Json &value) {
  if (!value.is_object() || value.size() < 2U || value.size() > 4U ||
      !value.contains("candidate") || !value.contains("sdpMid")) {
    return false;
  }
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    const auto &key = iterator.key();
    if (key != "candidate" && key != "sdpMid" && key != "sdpMLineIndex" &&
        key != "usernameFragment") {
      return false;
    }
  }
  return true;
}

std::optional<Json> strict_candidate(std::string_view encoded) {
  if (encoded.empty() || encoded.size() > kMaximumCandidateBytes ||
      encoded.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  bool duplicate_key = false;
  std::vector<std::unordered_set<std::string>> object_keys;
  const auto callback = [&](int, Json::parse_event_t event, Json &parsed) {
    if (event == Json::parse_event_t::object_start) {
      object_keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (object_keys.empty() || !object_keys.back().insert(parsed.get<std::string>()).second) {
        duplicate_key = true;
      }
    } else if (event == Json::parse_event_t::object_end && !object_keys.empty()) {
      object_keys.pop_back();
    }
    return true;
  };
  const auto value = Json::parse(encoded.begin(), encoded.end(), callback, false, false);
  if (duplicate_key || value.is_discarded() || !exact_candidate_keys(value) ||
      !value.at("candidate").is_string() || value.at("candidate").get<std::string>().empty() ||
      value.at("candidate").get<std::string>().size() > 2'048U || !value.at("sdpMid").is_string() ||
      value.at("sdpMid").get<std::string>().empty() ||
      value.at("sdpMid").get<std::string>().size() > 64U) {
    return std::nullopt;
  }
  if (value.contains("sdpMLineIndex") && !value.at("sdpMLineIndex").is_null() &&
      (!value.at("sdpMLineIndex").is_number_unsigned() ||
       value.at("sdpMLineIndex").get<std::uint64_t>() > 65'535U)) {
    return std::nullopt;
  }
  if (value.contains("usernameFragment") && !value.at("usernameFragment").is_null() &&
      (!value.at("usernameFragment").is_string() ||
       value.at("usernameFragment").get<std::string>().empty() ||
       value.at("usernameFragment").get<std::string>().size() > 256U)) {
    return std::nullopt;
  }
  return value;
}

std::string local_candidate_json(const rtc::Candidate &candidate) {
  return Json({{"candidate", candidate.candidate()}, {"sdpMid", candidate.mid()}}).dump();
}

} // namespace

struct PeerSender::Implementation {
  explicit Implementation(PeerSenderConfig peer_config)
      : config(std::move(peer_config)), control(config.session_id), started_at(Clock::now()),
        egress(kInitialMediaEpoch), peer(rtc_configuration()) {
    if (!config.event_callback || !config.request_idr_with_parameter_sets) {
      throw std::invalid_argument("peer sender callbacks are required");
    }
    install_callbacks();
    worker = std::thread([this] { run_worker(); });
  }

  ~Implementation() { stop("OWNER_STOP"); }

  rtc::Configuration rtc_configuration() {
    rtc::Configuration result;
    result.bindAddress = config.bind_address;
    result.enableIceTcp = false;
    result.enableIceUdpMux = false;
    result.disableAutoNegotiation = true;
    result.maxMessageSize = kMaximumControlBytes;
    for (const auto &url : config.ice_server_urls) {
      if (url.empty() || url.size() > 2'048U) {
        throw std::invalid_argument("peer ICE server URL invalid");
      }
      rtc::IceServer server(url);
      if (server.type == rtc::IceServer::Type::Turn &&
          server.relayType != rtc::IceServer::RelayType::TurnUdp) {
        throw std::invalid_argument("peer TURN TCP or TLS is unsupported in V1");
      }
      result.iceServers.push_back(std::move(server));
    }
    result.finalUdpSendCallback =
        [this](const rtc::byte *data, std::size_t size, rtc::FinalUdpEgressClass egress_class,
               rtc::FinalUdpDatagramPath path, rtc::FinalUdpDatagramProtocol protocol,
               rtc::FinalUdpIpFamily family, rtc::final_udp_native_send native_send,
               void *native_send_pointer) {
          return send_final(data, size, egress_class, path, protocol, family, native_send,
                            native_send_pointer);
        };
    return result;
  }

  void install_callbacks() {
    peer.onLocalDescription([this](rtc::Description description) {
      emit({.kind = PeerSenderEventKind::local_description,
            .value = static_cast<std::string>(description)});
    });
    peer.onLocalCandidate([this](rtc::Candidate candidate) {
      emit(
          {.kind = PeerSenderEventKind::local_candidate, .value = local_candidate_json(candidate)});
    });
    peer.onStateChange([this](rtc::PeerConnection::State state) {
      if (state == rtc::PeerConnection::State::Connected) {
        {
          std::scoped_lock lock(mutex);
          diagnostics_state.connected = true;
          diagnostics_state.reason = "PEER_CONNECTED";
        }
        changed.notify_all();
        emit({.kind = PeerSenderEventKind::connected});
      } else if (state == rtc::PeerConnection::State::Disconnected) {
        {
          std::scoped_lock lock(mutex);
          diagnostics_state.connected = false;
          diagnostics_state.reason = "PEER_DISCONNECTED";
        }
        emit({.kind = PeerSenderEventKind::disconnected});
      } else if (state == rtc::PeerConnection::State::Failed ||
                 state == rtc::PeerConnection::State::Closed) {
        fail("PEER_CONNECTION_FAILED");
      }
    });
    peer.onIceStateChange([this](rtc::PeerConnection::IceState state) {
      if (state == rtc::PeerConnection::IceState::Failed) {
        fail("PEER_ICE_FAILED");
      }
    });
    peer.onDataChannel(
        [this](std::shared_ptr<rtc::DataChannel> channel) { attach_control(std::move(channel)); });
    peer.onTrack(
        [this](std::shared_ptr<rtc::Track> incoming) { attach_track(std::move(incoming)); });
  }

  bool accept_offer(std::string_view sdp) {
    if (sdp.empty() || sdp.size() > 64U * 1024U || sdp.find('\0') != std::string_view::npos) {
      fail("PEER_OFFER_SIZE_INVALID");
      return false;
    }
    const auto compatibility = evaluate_recording_profile_offer(sdp, kPresentation);
    if (!compatibility.compatible) {
      fail("PEER_OFFER_INCOMPATIBLE:" + compatibility.reason);
      return false;
    }
    try {
      const auto selected_payload = select_payload_type(compatibility);
      {
        std::scoped_lock lock(mutex);
        if (diagnostics_state.offer_accepted || failed || stopping || stopped) {
          throw std::invalid_argument("peer offer state invalid");
        }
        payload_type = selected_payload;
      }
      peer.setRemoteDescription(rtc::Description(std::string(sdp), "offer"));
      {
        std::scoped_lock lock(mutex);
        if (failed || !track || !recovery) {
          throw std::invalid_argument("peer offer did not create one video track");
        }
        diagnostics_state.offer_accepted = true;
        diagnostics_state.reason = "PEER_OFFER_ACCEPTED";
      }
      peer.setLocalDescription(rtc::Description::Type::Answer);
      return true;
    } catch (const std::exception &error) {
      fail(std::string("PEER_OFFER_FAILED:") + error.what());
      return false;
    }
  }

  bool add_remote_candidate(std::string_view encoded_candidate) {
    const auto decoded = strict_candidate(encoded_candidate);
    if (!decoded) {
      fail("PEER_REMOTE_CANDIDATE_INVALID");
      return false;
    }
    bool invalid_state = false;
    {
      std::scoped_lock lock(mutex);
      invalid_state = !diagnostics_state.offer_accepted || failed || stopping || stopped;
    }
    if (invalid_state) {
      fail("PEER_REMOTE_CANDIDATE_STATE_INVALID");
      return false;
    }
    try {
      peer.addRemoteCandidate(rtc::Candidate(decoded->at("candidate").get<std::string>(),
                                             decoded->at("sdpMid").get<std::string>()));
      return true;
    } catch (const std::exception &) {
      fail("PEER_REMOTE_CANDIDATE_REJECTED");
      return false;
    }
  }

  void attach_track(std::shared_ptr<rtc::Track> incoming) {
    try {
      if (!incoming) {
        throw std::invalid_argument("peer track missing");
      }
      std::uint8_t selected_payload = 0U;
      {
        std::scoped_lock lock(mutex);
        if (track || !payload_type) {
          throw std::invalid_argument("peer track count or payload invalid");
        }
        selected_payload = *payload_type;
      }
      configure_track_description(*incoming, selected_payload);
      auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(kMediaSsrc, "glyphrelay",
                                                               selected_payload, 90'000U);
      rtp->setExtendedSequenceNumber(kInitialExtendedSequence);
      rtp->colorRange = 0U;
      rtp->colorPrimaries = 1U;
      rtp->colorTransfer = 1U;
      rtp->colorMatrix = 1U;
      auto packetizer = std::make_shared<rtc_adapter::StrictH264Packetizer>(rtp);
      packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
      auto nack = std::make_shared<rtc_adapter::BoundedNackResponder>(
          kMediaSsrc, [this] { return elapsed_ms_u64(); },
          [this] {
            emit({.kind = PeerSenderEventKind::recovery_requested});
            safe_request_idr();
          },
          [this] { fail("PEER_FEEDBACK_FLOOD"); });
      packetizer->addToChain(nack);
      packetizer->addToChain(std::make_shared<rtc::RembHandler>([this](unsigned int bitrate) {
        {
          std::scoped_lock lock(mutex);
          diagnostics_state.latest_remb_bps = bitrate;
        }
        emit({.kind = PeerSenderEventKind::remb, .number = bitrate});
      }));
      incoming->setMediaHandler(packetizer);
      incoming->onOpen([this] {
        {
          std::scoped_lock lock(mutex);
          diagnostics_state.track_open = true;
        }
        changed.notify_all();
      });
      incoming->onClosed([this] {
        bool should_fail = false;
        {
          std::scoped_lock lock(mutex);
          diagnostics_state.track_open = false;
          should_fail = !stopping && !stopped;
        }
        if (should_fail) {
          fail("PEER_TRACK_CLOSED");
        }
      });
      {
        std::scoped_lock lock(mutex);
        track = std::move(incoming);
        rtp_configuration = std::move(rtp);
        packetizer_handler = std::move(packetizer);
        recovery = std::move(nack);
      }
      changed.notify_all();
    } catch (const std::exception &error) {
      fail(std::string("PEER_TRACK_CONFIGURATION_FAILED:") + error.what());
    }
  }

  void attach_control(std::shared_ptr<rtc::DataChannel> channel) {
    if (!channel || channel->label() != kControlLabel || channel->protocol() != "") {
      if (channel) {
        channel->close();
      }
      fail("PEER_CONTROL_CHANNEL_IDENTITY_INVALID");
      return;
    }
    const auto reliability = channel->reliability();
    if (reliability.unordered || reliability.maxPacketLifeTime || reliability.maxRetransmits) {
      channel->close();
      fail("PEER_CONTROL_CHANNEL_RELIABILITY_INVALID");
      return;
    }
    bool duplicate_channel = false;
    {
      std::scoped_lock lock(mutex);
      if (control_channel || stopped) {
        duplicate_channel = true;
      } else {
        control_channel = channel;
      }
    }
    if (duplicate_channel) {
      channel->close();
      fail("PEER_MULTIPLE_CONTROL_CHANNELS");
      return;
    }
    channel->onOpen([this] { control_opened(); });
    channel->onMessage([this](rtc::binary) { fail("PEER_CONTROL_BINARY_REJECTED"); },
                       [this](std::string message) { control_received(std::move(message)); });
    channel->onError([this](std::string) { fail("PEER_CONTROL_CHANNEL_ERROR"); });
    channel->onClosed([this] {
      SenderControlOutput output;
      bool expected = false;
      {
        std::scoped_lock lock(mutex);
        diagnostics_state.control_open = false;
        const auto phase = control.diagnostics().phase;
        expected = stopping || stopped || phase == SenderControlPhase::ended ||
                   phase == SenderControlPhase::failed;
        if (!expected) {
          output = control.channel_closed();
        }
      }
      changed.notify_all();
      if (!expected) {
        apply_control(std::move(output));
      }
    });
  }

  void control_opened() {
    SenderControlOutput output;
    {
      std::scoped_lock lock(mutex);
      if (stopping || stopped) {
        return;
      }
      diagnostics_state.control_open = true;
      output = control.begin(kInitialMediaEpoch);
      const auto now = elapsed_ms();
      for (std::size_t sample = 0U; sample < 5U && output.valid; ++sample) {
        append_control(output, control.request_clock(now));
      }
    }
    changed.notify_all();
    apply_control(std::move(output));
    emit({.kind = PeerSenderEventKind::control_open});
  }

  void control_received(std::string message) {
    SenderControlOutput output;
    {
      std::scoped_lock lock(mutex);
      output = control.receive(message, elapsed_ms());
    }
    std::fill(message.begin(), message.end(), '\0');
    message.clear();
    apply_control(std::move(output));
    changed.notify_all();
  }

  void apply_control(SenderControlOutput output) {
    std::shared_ptr<rtc::DataChannel> channel;
    {
      std::scoped_lock lock(mutex);
      channel = control_channel;
    }
    for (auto &message : output.outbound_messages) {
      bool sent = false;
      try {
        sent = channel && channel->isOpen() && channel->send(message);
      } catch (const std::exception &) {
        sent = false;
      }
      std::fill(message.begin(), message.end(), '\0');
      message.clear();
      if (!sent) {
        fail("PEER_CONTROL_SEND_FAILED");
        return;
      }
    }
    std::optional<std::string> terminal_reason;
    for (const auto &event : output.events) {
      if (event.kind == ReceiverControlEventKind::receiver_stats) {
        emit({.kind = PeerSenderEventKind::receiver_stats, .receiver_stats = event.stats});
      } else if (event.kind == ReceiverControlEventKind::terminal ||
                 event.kind == ReceiverControlEventKind::protocol_error) {
        terminal_reason = event.reason;
      }
    }
    if (terminal_reason) {
      fail(*terminal_reason);
    }
    if (!output.valid) {
      std::string reason;
      {
        std::scoped_lock lock(mutex);
        reason = control.diagnostics().reason;
      }
      fail(std::move(reason));
      return;
    }
    if (output.close_channel && channel && !channel->isClosed()) {
      channel->close();
    }
  }

  bool send_access_unit(const RecordedAccessUnit &access_unit) {
    std::shared_ptr<rtc::Track> active_track;
    std::shared_ptr<rtc_adapter::BoundedNackResponder> active_recovery;
    std::shared_ptr<rtc_adapter::StrictH264Packetizer> active_packetizer;
    std::uint64_t access_unit_id = 0U;
    bool begin_epoch = false;
    {
      std::scoped_lock lock(mutex);
      if (failed || stopping || stopped || !diagnostics_state.connected ||
          !diagnostics_state.track_open || !track || !recovery || !packetizer_handler ||
          !access_unit.bytes || access_unit.bytes->empty() ||
          access_unit.media_epoch != kInitialMediaEpoch || access_unit.dependency_epoch == 0U ||
          access_unit.extended_rtp_timestamp == 0U) {
        diagnostics_state.reason = "PEER_ACCESS_UNIT_STATE_OR_IDENTITY_INVALID";
        return false;
      }
      if (diagnostics_state.dependency_epoch == 0U ||
          diagnostics_state.dependency_epoch != access_unit.dependency_epoch) {
        if (!access_unit.keyframe || !access_unit.parameter_sets_present) {
          diagnostics_state.reason = "PEER_DEPENDENCY_EPOCH_REQUIRES_IDR";
          return false;
        }
        diagnostics_state.dependency_epoch = access_unit.dependency_epoch;
        begin_epoch = true;
      }
      active_track = track;
      active_recovery = recovery;
      active_packetizer = packetizer_handler;
      access_unit_id = ++next_access_unit_id;
    }
    if (begin_epoch &&
        !active_recovery->begin_epoch(access_unit.media_epoch, access_unit.dependency_epoch,
                                      RecoveryTrigger::dependency_epoch_transition)) {
      fail("PEER_RECOVERY_EPOCH_REJECTED");
      return false;
    }
    auto message = rtc::make_message(access_unit.bytes->size(), rtc::Message::Binary);
    std::memcpy(message->data(), access_unit.bytes->data(), access_unit.bytes->size());
    message->frameId = access_unit.source_frame_id;
    message->mediaEpoch = access_unit.media_epoch;
    message->accessUnitId = access_unit_id;
    message->dependencyEpoch = access_unit.dependency_epoch;
    message->extendedTimestamp = access_unit.extended_rtp_timestamp;
    if (!active_track->sendMessage(std::move(message))) {
      fail("PEER_TRACK_SEND_FAILED");
      return false;
    }
    if (const auto rejection = active_packetizer->take_last_rejection()) {
      fail("PEER_PACKETIZER_REJECTED:" + *rejection);
      return false;
    }
    bool counter_overflow = false;
    {
      std::scoped_lock lock(mutex);
      ++diagnostics_state.access_units_sent;
      if (access_unit.bytes->size() >
          std::numeric_limits<std::uint64_t>::max() - diagnostics_state.bytes_sent) {
        counter_overflow = true;
      } else {
        diagnostics_state.bytes_sent += access_unit.bytes->size();
        diagnostics_state.reason = "PEER_ACCESS_UNIT_SENT";
      }
    }
    if (counter_overflow) {
      fail("PEER_BYTE_COUNTER_OVERFLOW");
      return false;
    }
    return true;
  }

  void stop(std::string_view reason) {
    std::shared_ptr<rtc::DataChannel> channel;
    std::shared_ptr<rtc::Track> active_track;
    std::shared_ptr<rtc_adapter::BoundedNackResponder> active_recovery;
    SenderControlOutput output;
    {
      std::scoped_lock lock(mutex);
      if (stopped || stopping) {
        return;
      }
      stopping = true;
      diagnostics_state.connected = false;
      diagnostics_state.reason = std::string(reason);
      channel = control_channel;
      active_track = track;
      active_recovery = recovery;
    }
    static_cast<void>(egress.close_media(MediaBoundaryReason::stop));
    if (active_recovery) {
      active_recovery->stop();
    }
    {
      std::scoped_lock lock(mutex);
      const auto phase = control.diagnostics().phase;
      if (channel && channel->isOpen() &&
          (phase == SenderControlPhase::connected || phase == SenderControlPhase::paused ||
           phase == SenderControlPhase::pause_pending ||
           phase == SenderControlPhase::resume_pending)) {
        output = control.end(kInitialMediaEpoch, reason);
      }
    }
    if (!output.outbound_messages.empty()) {
      apply_control(std::move(output));
      std::unique_lock lock(mutex);
      changed.wait_for(lock, kControlEndTimeout, [&] {
        const auto phase = control.diagnostics().phase;
        return phase == SenderControlPhase::ended || phase == SenderControlPhase::failed;
      });
    }
    {
      std::scoped_lock lock(mutex);
      shutdown_worker = true;
    }
    changed.notify_all();
    if (channel) {
      channel->resetCallbacks();
      channel->close();
    }
    if (active_track) {
      active_track->resetCallbacks();
      active_track->close();
    }
    peer.resetCallbacks();
    peer.close();
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
      worker.join();
    }
    {
      std::scoped_lock lock(mutex);
      control_channel.reset();
      track.reset();
      packetizer_handler.reset();
      recovery.reset();
      rtp_configuration.reset();
      diagnostics_state.track_open = false;
      diagnostics_state.control_open = false;
      diagnostics_state.stopped = true;
      stopped = true;
      stopping = false;
    }
    emit({.kind = PeerSenderEventKind::ended, .value = std::string(reason)});
  }

  void run_worker() {
    std::unique_lock lock(mutex);
    while (!shutdown_worker) {
      changed.wait_for(lock, kWorkerInterval);
      if (shutdown_worker) {
        break;
      }
      if (!diagnostics_state.control_open ||
          control.diagnostics().phase != SenderControlPhase::connected) {
        continue;
      }
      auto output = control.request_clock(elapsed_ms());
      lock.unlock();
      if (!output.outbound_messages.empty() || !output.valid) {
        apply_control(std::move(output));
      }
      lock.lock();
    }
  }

  PeerSenderDiagnostics diagnostics() const {
    std::scoped_lock lock(mutex);
    auto result = diagnostics_state;
    result.control = control.diagnostics();
    if (recovery) {
      result.recovery = recovery->diagnostics();
      result.retransmission_cache = recovery->cache_snapshot();
    }
    result.egress = egress.snapshot();
    return result;
  }

  int send_final(const rtc::byte *data, std::size_t size, rtc::FinalUdpEgressClass egress_class,
                 rtc::FinalUdpDatagramPath path, rtc::FinalUdpDatagramProtocol protocol,
                 rtc::FinalUdpIpFamily family, rtc::final_udp_native_send native_send,
                 void *native_send_pointer) {
    if (data == nullptr || native_send == nullptr) {
      return -1;
    }
    const FinalDatagramMetadata metadata{
        .classification = egress_class == rtc::FinalUdpEgressClass::Media ? DatagramClass::media
                                                                          : DatagramClass::control,
        .path = path == rtc::FinalUdpDatagramPath::Direct ? DatagramPath::direct_udp
                                                          : DatagramPath::turn_udp,
        .ip_family =
            family == rtc::FinalUdpIpFamily::Ipv4 ? DatagramIpFamily::ipv4 : DatagramIpFamily::ipv6,
        .provenance = map_provenance(egress_class, protocol),
        .protocol = map_protocol(protocol),
        .media_epoch = egress_class == rtc::FinalUdpEgressClass::Media ? kInitialMediaEpoch : 0U,
    };
    const auto datagram = std::span(data, size);
    const auto result =
        egress.send_final(std::as_bytes(datagram), metadata, [native_send, native_send_pointer] {
          return static_cast<std::ptrdiff_t>(native_send(native_send_pointer));
        });
    return result.native_result < static_cast<std::ptrdiff_t>(std::numeric_limits<int>::min()) ||
                   result.native_result >
                       static_cast<std::ptrdiff_t>(std::numeric_limits<int>::max())
               ? -1
               : static_cast<int>(result.native_result);
  }

  void fail(std::string reason) {
    bool notify = false;
    {
      std::scoped_lock lock(mutex);
      notify = !failed && !stopping && !stopped;
      if (notify) {
        fail_locked(reason);
      }
    }
    if (notify) {
      static_cast<void>(egress.close_media(MediaBoundaryReason::stop));
      changed.notify_all();
      emit({.kind = PeerSenderEventKind::failed, .value = std::move(reason)});
    }
  }

  void fail_locked(const std::string &reason) {
    failed = true;
    diagnostics_state.connected = false;
    diagnostics_state.reason = reason;
  }

  void emit(PeerSenderEvent event) const {
    try {
      config.event_callback(event);
    } catch (...) {
    }
  }

  void safe_request_idr() const {
    try {
      config.request_idr_with_parameter_sets();
    } catch (...) {
    }
  }

  double elapsed_ms() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - started_at).count();
  }

  std::uint64_t elapsed_ms_u64() const {
    const auto value =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count();
    return value < 0 ? 0U : static_cast<std::uint64_t>(value);
  }

  PeerSenderConfig config;
  mutable std::mutex mutex;
  std::condition_variable changed;
  SenderControlProtocol control;
  Clock::time_point started_at;
  MediaEgressGate egress;
  rtc::PeerConnection peer;
  std::thread worker;
  std::shared_ptr<rtc::Track> track;
  std::shared_ptr<rtc::DataChannel> control_channel;
  std::shared_ptr<rtc::RtpPacketizationConfig> rtp_configuration;
  std::shared_ptr<rtc_adapter::StrictH264Packetizer> packetizer_handler;
  std::shared_ptr<rtc_adapter::BoundedNackResponder> recovery;
  std::optional<std::uint8_t> payload_type;
  PeerSenderDiagnostics diagnostics_state{.media_epoch = kInitialMediaEpoch, .reason = "PEER_NEW"};
  std::uint64_t next_access_unit_id = 0U;
  bool shutdown_worker = false;
  bool stopping = false;
  bool stopped = false;
  bool failed = false;
};

PeerSender::PeerSender(PeerSenderConfig config)
    : implementation_(std::make_unique<Implementation>(std::move(config))) {}

PeerSender::~PeerSender() = default;
PeerSender::PeerSender(PeerSender &&) noexcept = default;
PeerSender &PeerSender::operator=(PeerSender &&) noexcept = default;

bool PeerSender::accept_offer(std::string_view sdp) { return implementation_->accept_offer(sdp); }

bool PeerSender::add_remote_candidate(std::string_view encoded_candidate) {
  return implementation_->add_remote_candidate(encoded_candidate);
}

bool PeerSender::send_access_unit(const RecordedAccessUnit &access_unit) {
  return implementation_->send_access_unit(access_unit);
}

void PeerSender::stop(std::string_view reason) { implementation_->stop(reason); }

PeerSenderDiagnostics PeerSender::diagnostics() const { return implementation_->diagnostics(); }

std::string_view peer_sender_event_name(PeerSenderEventKind kind) {
  switch (kind) {
  case PeerSenderEventKind::local_description:
    return "LOCAL_DESCRIPTION";
  case PeerSenderEventKind::local_candidate:
    return "LOCAL_CANDIDATE";
  case PeerSenderEventKind::connected:
    return "CONNECTED";
  case PeerSenderEventKind::disconnected:
    return "DISCONNECTED";
  case PeerSenderEventKind::control_open:
    return "CONTROL_OPEN";
  case PeerSenderEventKind::receiver_stats:
    return "RECEIVER_STATS";
  case PeerSenderEventKind::remb:
    return "REMB";
  case PeerSenderEventKind::recovery_requested:
    return "RECOVERY_REQUESTED";
  case PeerSenderEventKind::ended:
    return "ENDED";
  case PeerSenderEventKind::failed:
    return "FAILED";
  }
  return "UNKNOWN";
}

} // namespace glyphrelay
