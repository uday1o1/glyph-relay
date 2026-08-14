#include "glyphrelay/sha256.hpp"

#include <rtc/rtc.hpp>
#include <rtc/rtp.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::uint16_t kFirstPort = 41'000U;
constexpr std::uint16_t kSecondPort = 41'001U;
constexpr std::uint32_t kSsrc = 0x47524C59U;
constexpr std::uint8_t kPayloadType = 102U;
constexpr std::size_t kMaximumEvents = 4'096U;

class FixtureError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

enum class Scenario { direct_ipv4, direct_ipv6, turn_udp };

struct Arguments {
  Scenario scenario = Scenario::direct_ipv4;
  std::filesystem::path output;
};

struct Event {
  std::string agent;
  std::string egress_class;
  std::string path;
  std::string protocol;
  std::string family;
  std::string payload_sha256;
  std::uint16_t source_port = 0U;
  std::size_t payload_bytes = 0U;
  std::size_t ip_total_bytes = 0U;
  int native_result = 0;
};

std::string_view scenario_name(Scenario scenario) {
  switch (scenario) {
  case Scenario::direct_ipv4:
    return "direct-ipv4";
  case Scenario::direct_ipv6:
    return "direct-ipv6";
  case Scenario::turn_udp:
    return "turn-udp";
  }
  throw FixtureError("scenario_invalid");
}

Scenario parse_scenario(std::string_view value) {
  if (value == "direct-ipv4") {
    return Scenario::direct_ipv4;
  }
  if (value == "direct-ipv6") {
    return Scenario::direct_ipv6;
  }
  if (value == "turn-udp") {
    return Scenario::turn_udp;
  }
  throw FixtureError("scenario_invalid");
}

Arguments parse_arguments(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "Usage: glyphrelay_m0_transport_fixture --scenario "
                 "direct-ipv4|direct-ipv6|turn-udp --output FILE\n";
    std::exit(0);
  }
  if (argc != 5) {
    throw FixtureError("transport_fixture_arguments_invalid");
  }
  std::optional<std::string_view> scenario;
  std::optional<std::filesystem::path> output;
  for (int index = 1; index < argc; index += 2) {
    const std::string_view option(argv[index]);
    const std::string_view value(argv[index + 1]);
    if (option == "--scenario" && !scenario) {
      scenario = value;
    } else if (option == "--output" && !output) {
      output = std::filesystem::path(value);
    } else {
      throw FixtureError("transport_fixture_argument_unknown_or_duplicate");
    }
  }
  if (!scenario || !output || output->empty() || std::filesystem::exists(*output)) {
    throw FixtureError("transport_fixture_output_invalid");
  }
  return {parse_scenario(*scenario), *output};
}

std::string required_environment(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    throw FixtureError(std::string("transport_fixture_environment_missing:") + name);
  }
  return value;
}

std::uint16_t parse_port(std::string_view value) {
  std::uint32_t port = 0U;
  for (const char character : value) {
    if (character < '0' || character > '9') {
      throw FixtureError("turn_port_invalid");
    }
    port = port * 10U + static_cast<std::uint32_t>(character - '0');
    if (port > std::numeric_limits<std::uint16_t>::max()) {
      throw FixtureError("turn_port_invalid");
    }
  }
  if (port == 0U) {
    throw FixtureError("turn_port_invalid");
  }
  return static_cast<std::uint16_t>(port);
}

std::string_view class_name(rtc::FinalUdpEgressClass value) {
  return value == rtc::FinalUdpEgressClass::Media ? "MEDIA" : "CONTROL";
}

std::string_view path_name(rtc::FinalUdpDatagramPath value) {
  return value == rtc::FinalUdpDatagramPath::Turn ? "TURN_UDP" : "DIRECT_UDP";
}

std::string_view family_name(rtc::FinalUdpIpFamily value) {
  return value == rtc::FinalUdpIpFamily::Ipv6 ? "IPV6" : "IPV4";
}

std::string_view protocol_name(rtc::FinalUdpDatagramProtocol value) {
  switch (value) {
  case rtc::FinalUdpDatagramProtocol::UnknownControl:
    return "UNKNOWN_CONTROL";
  case rtc::FinalUdpDatagramProtocol::Srtp:
    return "SRTP";
  case rtc::FinalUdpDatagramProtocol::Srtcp:
    return "SRTCP";
  case rtc::FinalUdpDatagramProtocol::Dtls:
    return "DTLS";
  case rtc::FinalUdpDatagramProtocol::Stun:
    return "STUN";
  case rtc::FinalUdpDatagramProtocol::TurnChannelData:
    return "TURN_CHANNEL_DATA";
  case rtc::FinalUdpDatagramProtocol::TurnSendIndication:
    return "TURN_SEND_INDICATION";
  case rtc::FinalUdpDatagramProtocol::TurnControl:
    return "TURN_CONTROL";
  }
  throw FixtureError("transport_fixture_protocol_invalid");
}

class EventCollector {
public:
  EventCollector(std::string agent, std::uint16_t source_port)
      : agent_(std::move(agent)), source_port_(source_port) {}

  int send(const rtc::byte *data, std::size_t size, rtc::FinalUdpEgressClass egress_class,
           rtc::FinalUdpDatagramPath path, rtc::FinalUdpDatagramProtocol protocol,
           rtc::FinalUdpIpFamily family, rtc::final_udp_native_send native_send,
           void *native_send_ptr) {
    if (data == nullptr || native_send == nullptr || native_send_ptr == nullptr ||
        (size == 0U && (egress_class != rtc::FinalUdpEgressClass::Control ||
                        path != rtc::FinalUdpDatagramPath::Direct ||
                        protocol != rtc::FinalUdpDatagramProtocol::UnknownControl))) {
      invalid_.store(true);
      return -1;
    }
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t *>(data), size);
    const auto digest = glyphrelay::sha256_hex(bytes);
    const int result = native_send(native_send_ptr);
    Event event{
        .agent = agent_,
        .egress_class = std::string(class_name(egress_class)),
        .path = std::string(path_name(path)),
        .protocol = std::string(protocol_name(protocol)),
        .family = std::string(family_name(family)),
        .payload_sha256 = digest,
        .source_port = source_port_,
        .payload_bytes = size,
        .ip_total_bytes = size + 8U + (family == rtc::FinalUdpIpFamily::Ipv6 ? 40U : 20U),
        .native_result = result,
    };
    {
      std::lock_guard lock(mutex_);
      if (events_.size() >= kMaximumEvents) {
        invalid_.store(true);
      } else {
        events_.push_back(std::move(event));
      }
    }
    return result;
  }

  std::vector<Event> events() const {
    std::lock_guard lock(mutex_);
    return events_;
  }

  bool invalid() const { return invalid_.load(); }

private:
  std::string agent_;
  std::uint16_t source_port_;
  mutable std::mutex mutex_;
  std::vector<Event> events_;
  std::atomic_bool invalid_ = false;
};

rtc::Configuration configuration(Scenario scenario, std::uint16_t port, EventCollector &collector,
                                 bool relay_peer) {
  rtc::Configuration config;
  config.bindAddress = scenario == Scenario::direct_ipv6 ? "::1" : "127.0.0.1";
  config.portRangeBegin = port;
  config.portRangeEnd = port;
  config.enableIceTcp = false;
  config.enableIceUdpMux = false;
  config.finalUdpSendCallback =
      [&collector](const rtc::byte *data, std::size_t size, rtc::FinalUdpEgressClass egress_class,
                   rtc::FinalUdpDatagramPath path, rtc::FinalUdpDatagramProtocol protocol,
                   rtc::FinalUdpIpFamily family, rtc::final_udp_native_send native_send,
                   void *native_send_ptr) {
        return collector.send(data, size, egress_class, path, protocol, family, native_send,
                              native_send_ptr);
      };
  if (scenario == Scenario::turn_udp && relay_peer) {
    const auto host = required_environment("GLYPHRELAY_M0_TURN_HOST");
    const auto username = required_environment("GLYPHRELAY_M0_TURN_USERNAME");
    const auto password = required_environment("GLYPHRELAY_M0_TURN_PASSWORD");
    const auto turn_port = parse_port(required_environment("GLYPHRELAY_M0_TURN_PORT"));
    config.iceServers.emplace_back(host, turn_port, username, password,
                                   rtc::IceServer::RelayType::TurnUdp);
    config.iceTransportPolicy = rtc::TransportPolicy::Relay;
  }
  return config;
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::steady_clock::duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

rtc::binary rtp_packet() {
  constexpr std::size_t payload_bytes = 512U;
  rtc::binary packet(sizeof(rtc::RtpHeader) + payload_bytes, rtc::byte{0U});
  auto *header = reinterpret_cast<rtc::RtpHeader *>(packet.data());
  header->setPayloadType(kPayloadType);
  header->setSeqNumber(65'534U);
  header->setTimestamp(90'000U);
  header->setSsrc(kSsrc);
  header->setMarker(true);
  header->preparePacket();
  std::fill(packet.begin() + static_cast<std::ptrdiff_t>(sizeof(rtc::RtpHeader)), packet.end(),
            rtc::byte{0x47U});
  return packet;
}

std::string candidate_type(const rtc::Candidate &candidate) {
  switch (candidate.type()) {
  case rtc::Candidate::Type::Host:
    return "HOST";
  case rtc::Candidate::Type::ServerReflexive:
    return "SERVER_REFLEXIVE";
  case rtc::Candidate::Type::PeerReflexive:
    return "PEER_REFLEXIVE";
  case rtc::Candidate::Type::Relayed:
    return "RELAYED";
  case rtc::Candidate::Type::Unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

void require_events(const std::vector<Event> &events, Scenario scenario) {
  if (events.empty()) {
    throw FixtureError("transport_fixture_no_egress_events");
  }
  const std::string expected_path = scenario == Scenario::turn_udp ? "TURN_UDP" : "DIRECT_UDP";
  const std::string expected_family = scenario == Scenario::direct_ipv6 ? "IPV6" : "IPV4";
  std::size_t media = 0U;
  for (const auto &event : events) {
    if (event.native_result != static_cast<int>(event.payload_bytes) ||
        (scenario != Scenario::turn_udp && event.path != expected_path) ||
        event.family != expected_family) {
      throw FixtureError("transport_fixture_event_contract_failed:" + event.agent + ":" +
                         event.path + ":" + event.family + ":" +
                         std::to_string(event.native_result) + ":" +
                         std::to_string(event.payload_bytes));
    }
    if (event.egress_class == "MEDIA") {
      ++media;
      const bool protocol_valid =
          scenario == Scenario::turn_udp
              ? event.protocol == "TURN_CHANNEL_DATA" || event.protocol == "TURN_SEND_INDICATION"
              : event.protocol == "SRTP";
      if (!protocol_valid || event.agent != "first") {
        throw FixtureError("transport_fixture_media_classification_failed");
      }
      if (scenario == Scenario::turn_udp && event.path != "TURN_UDP") {
        throw FixtureError("transport_fixture_media_path_failed");
      }
    } else if (scenario == Scenario::turn_udp && event.path == "DIRECT_UDP" &&
               event.protocol != "STUN" && event.protocol != "DTLS" && event.protocol != "SRTCP" &&
               event.protocol != "UNKNOWN_CONTROL") {
      throw FixtureError("transport_fixture_direct_control_classification_failed:" + event.agent +
                         ":" + event.protocol);
    }
  }
  if (media != 1U || std::none_of(events.begin(), events.end(), [](const Event &event) {
        return event.egress_class == "CONTROL";
      })) {
    throw FixtureError("transport_fixture_event_cardinality_failed");
  }
}

std::pair<std::string, std::string> run_fixture(Scenario scenario, EventCollector &first_collector,
                                                EventCollector &second_collector) {
  std::shared_ptr<rtc::Track> received_track;
  std::promise<rtc::binary> received_promise;
  auto received_future = received_promise.get_future();
  std::atomic_bool received_once = false;
  std::mutex callback_error_mutex;
  std::string callback_error;
  const auto record_callback_error = [&](std::string reason) {
    std::lock_guard lock(callback_error_mutex);
    if (callback_error.empty()) {
      callback_error = std::move(reason);
    }
  };

  std::string first_candidate_type;
  std::string second_candidate_type;
  {
    rtc::PeerConnection first(configuration(scenario, kFirstPort, first_collector, true));
    rtc::PeerConnection second(configuration(scenario, kSecondPort, second_collector, false));
    first.onLocalDescription([&](rtc::Description description) {
      if (scenario == Scenario::turn_udp) {
        return;
      }
      try {
        second.setRemoteDescription(std::string(description));
      } catch (const std::exception &error) {
        record_callback_error(std::string("first_description:") + error.what());
      }
    });
    first.onLocalCandidate([&](rtc::Candidate candidate) {
      if (scenario == Scenario::turn_udp) {
        return;
      }
      try {
        second.addRemoteCandidate(std::string(candidate));
      } catch (const std::exception &error) {
        record_callback_error(std::string("first_candidate:") + error.what());
      }
    });
    first.onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
      if (scenario != Scenario::turn_udp ||
          state != rtc::PeerConnection::GatheringState::Complete) {
        return;
      }
      try {
        const auto description = first.localDescription();
        if (!description) {
          record_callback_error("first_gathered_description_missing");
          return;
        }
        second.setRemoteDescription(std::string(*description));
      } catch (const std::exception &error) {
        record_callback_error(std::string("first_gathered_description:") + error.what());
      }
    });
    second.onLocalDescription([&](rtc::Description description) {
      try {
        first.setRemoteDescription(std::string(description));
      } catch (const std::exception &error) {
        record_callback_error(std::string("second_description:") + error.what());
      }
    });
    second.onLocalCandidate([&](rtc::Candidate candidate) {
      try {
        first.addRemoteCandidate(std::string(candidate));
      } catch (const std::exception &error) {
        record_callback_error(std::string("second_candidate:") + error.what());
      }
    });
    second.onTrack([&](std::shared_ptr<rtc::Track> track) {
      track->onMessage(
          [&](rtc::binary message) {
            if (!received_once.exchange(true)) {
              received_promise.set_value(std::move(message));
            }
          },
          nullptr);
      std::atomic_store(&received_track, std::move(track));
    });

    rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
    media.addH264Codec(kPayloadType);
    media.addSSRC(kSsrc, "glyphrelay-m0-transport");
    auto sending_track = first.addTrack(media);
    first.setLocalDescription();

    const bool open = wait_until(
        [&] {
          auto receiver = std::atomic_load(&received_track);
          return first.state() == rtc::PeerConnection::State::Connected &&
                 second.state() == rtc::PeerConnection::State::Connected &&
                 sending_track->isOpen() && receiver && receiver->isOpen();
        },
        30s);
    {
      std::lock_guard lock(callback_error_mutex);
      if (!callback_error.empty()) {
        throw FixtureError("transport_fixture_signaling_failed:" + callback_error);
      }
    }
    if (!open) {
      throw FixtureError("transport_fixture_connection_timeout");
    }
    rtc::Candidate first_local;
    rtc::Candidate first_remote;
    rtc::Candidate second_local;
    rtc::Candidate second_remote;
    if (!first.getSelectedCandidatePair(&first_local, &first_remote) ||
        !second.getSelectedCandidatePair(&second_local, &second_remote)) {
      throw FixtureError("transport_fixture_selected_candidate_missing");
    }
    first_candidate_type = candidate_type(first_local);
    second_candidate_type = candidate_type(second_local);
    const bool relay = scenario == Scenario::turn_udp;
    const bool candidate_types_valid =
        relay ? first_candidate_type == "RELAYED" && second_candidate_type != "RELAYED"
              : first_candidate_type != "RELAYED" && second_candidate_type != "RELAYED";
    if (!candidate_types_valid) {
      throw FixtureError("transport_fixture_selected_path_invalid:" + first_candidate_type + ":" +
                         second_candidate_type);
    }

    const auto packet = rtp_packet();
    if (!sending_track->send(packet)) {
      throw FixtureError("transport_fixture_rtp_send_failed");
    }
    if (received_future.wait_for(10s) != std::future_status::ready ||
        received_future.get() != packet) {
      throw FixtureError("transport_fixture_rtp_receive_failed");
    }
    std::this_thread::sleep_for(500ms);
    first.close();
    second.close();
    std::this_thread::sleep_for(200ms);
  }
  if (first_collector.invalid() || second_collector.invalid()) {
    throw FixtureError("transport_fixture_collector_invalid");
  }
  return {first_candidate_type, second_candidate_type};
}

void write_json(const Arguments &arguments, std::span<const Event> events,
                const std::pair<std::string, std::string> &candidate_types) {
  std::ofstream output(arguments.output, std::ios::binary);
  if (!output) {
    throw FixtureError("transport_fixture_output_open_failed");
  }
  output << "{\n"
         << "  \"schemaVersion\": 1,\n"
         << "  \"protocol\": \"glyphrelay-m0-transport-fixture-v1\",\n"
         << "  \"status\": \"PASSED\",\n"
         << "  \"scenario\": \"" << scenario_name(arguments.scenario) << "\",\n"
         << "  \"firstSourcePort\": " << kFirstPort << ",\n"
         << "  \"secondSourcePort\": " << kSecondPort << ",\n"
         << "  \"firstSelectedCandidateType\": \"" << candidate_types.first << "\",\n"
         << "  \"secondSelectedCandidateType\": \"" << candidate_types.second << "\",\n"
         << "  \"events\": [\n";
  for (std::size_t index = 0U; index < events.size(); ++index) {
    const auto &event = events[index];
    output << "    {\"agent\": \"" << event.agent << "\", \"sourcePort\": " << event.source_port
           << ", \"class\": \"" << event.egress_class << "\", \"path\": \"" << event.path
           << "\", \"protocol\": \"" << event.protocol << "\", \"family\": \"" << event.family
           << "\", \"payloadBytes\": " << event.payload_bytes
           << ", \"ipTotalBytes\": " << event.ip_total_bytes << ", \"payloadSha256\": \""
           << event.payload_sha256 << "\", \"nativeResult\": " << event.native_result << "}"
           << (index + 1U == events.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  output.flush();
  if (!output) {
    throw FixtureError("transport_fixture_output_write_failed");
  }
}

int run(const Arguments &arguments) {
  const auto *debug = std::getenv("GLYPHRELAY_M0_TRANSPORT_DEBUG");
  rtc::InitLogger(debug != nullptr && std::string_view(debug) == "1" ? rtc::LogLevel::Verbose
                                                                     : rtc::LogLevel::Warning);
  EventCollector first_collector("first", kFirstPort);
  EventCollector second_collector("second", kSecondPort);
  const auto candidate_types = run_fixture(arguments.scenario, first_collector, second_collector);
  auto events = first_collector.events();
  auto second_events = second_collector.events();
  events.insert(events.end(), std::make_move_iterator(second_events.begin()),
                std::make_move_iterator(second_events.end()));
  require_events(events, arguments.scenario);
  write_json(arguments, events, candidate_types);
  std::cout << "{\"events\":" << events.size() << ",\"scenario\":\""
            << scenario_name(arguments.scenario) << "\",\"status\":\"PASSED\"}\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return run(parse_arguments(argc, argv));
  } catch (const std::exception &error) {
    std::cerr << "glyphrelay_m0_transport_fixture: " << error.what() << '\n';
    return 1;
  }
}
