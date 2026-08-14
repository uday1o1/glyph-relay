#include "glyphrelay/owner_signaling.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;

constexpr const char *kOrigin = "http://127.0.0.1:8443";
constexpr const char *kSession = "abcdefghijklmnopqrstuv";
constexpr const char *kOwnerCapability = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq";
constexpr const char *kJoinCapability = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefg";

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

std::string server_message(std::string type, std::uint64_t sequence, Json fields = Json::object()) {
  Json message = {
      {"protocolVersion", glyphrelay::kSignalingProtocol},
      {"sequence", sequence},
      {"sessionId", kSession},
      {"type", std::move(type)},
  };
  message.update(fields);
  return message.dump();
}

Json one_outbound(const glyphrelay::OwnerSignalingOutput &output) {
  require(output.valid && output.outbound_messages.size() == 1U,
          "fixture operation must emit exactly one valid client message");
  return Json::parse(output.outbound_messages.front());
}

void test_origin_validation() {
  for (const std::string invalid : {
           "https://share.example.test/",
           "http://share.example.test",
           "https://user@share.example.test",
           "https://share.example.test:443",
           "https://SHARE.example.test",
           "https://share.example.test\r\nX-Forged: yes",
       }) {
    bool rejected = false;
    try {
      glyphrelay::OwnerSignalingProtocol protocol(invalid);
      static_cast<void>(protocol);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected, "noncanonical, insecure, or injectable signaling origins must fail");
  }

  glyphrelay::OwnerSignalingProtocol ipv4("http://127.0.0.1:8080");
  glyphrelay::OwnerSignalingProtocol ipv6("http://[::1]:8080");
  glyphrelay::OwnerSignalingProtocol secure("https://share.example.test");
  static_cast<void>(ipv4);
  static_cast<void>(ipv6);
  static_cast<void>(secure);
}

void test_complete_owner_protocol_flow() {
  glyphrelay::OwnerSignalingProtocol protocol(kOrigin);
  const auto create = one_outbound(protocol.begin(true));
  require(create.size() == 3U && create.at("type") == "CREATE_SESSION" &&
              create.at("sequence") == 1U && !create.contains("ownerCapability"),
          "session creation must have an exact unauthenticated first-message schema");

  const auto created = protocol.receive(server_message(
      "SESSION_CREATED", 1U,
      {{"absoluteDeadlineMs", 28'800'000.5}, {"ownerCapability", kOwnerCapability}}));
  require(created.valid && created.events.size() == 1U &&
              created.events.front().kind == glyphrelay::OwnerSignalingEventKind::session_created,
          "valid session creation must produce one non-secret local event");
  const auto create_join = one_outbound(created);
  require(create_join.size() == 5U && create_join.at("type") == "CREATE_JOIN" &&
              create_join.at("sequence") == 2U &&
              create_join.at("ownerCapability") == kOwnerCapability,
          "automatic join creation must use the bound owner capability and next sequence");
  const auto after_created = protocol.diagnostics();
  require(after_created.phase == glyphrelay::OwnerSignalingPhase::join_pending &&
              after_created.owner_capability_present && after_created.session_id == kSession &&
              after_created.reason.find(kOwnerCapability) == std::string::npos,
          "diagnostics must expose state without returning the owner capability");

  const std::string join_url = std::string(kOrigin) + "/#join=" + kSession + "." + kJoinCapability;
  const auto joined = protocol.receive(
      server_message("JOIN_CREATED", 2U, {{"joinExpiresAtMs", 600'000.25}, {"joinUrl", join_url}}));
  require(joined.valid && joined.events.size() == 1U &&
              joined.events.front().kind == glyphrelay::OwnerSignalingEventKind::join_created &&
              joined.events.front().value == join_url &&
              protocol.diagnostics().phase == glyphrelay::OwnerSignalingPhase::join_open,
          "a link must match the configured origin, session, and fragment capability exactly");

  const auto heartbeat = one_outbound(protocol.receive(
      server_message("HEARTBEAT", 3U, {{"deadlineMs", 7'000.75}, {"heartbeatSequence", 1U}})));
  require(heartbeat.size() == 6U && heartbeat.at("type") == "OWNER_HEARTBEAT_ACK" &&
              heartbeat.at("sequence") == 3U && heartbeat.at("heartbeatSequence") == 1U,
          "heartbeat acknowledgment must preserve the server heartbeat and client sequence");

  const auto reserved = protocol.receive(server_message("RECEIVER_RESERVED", 4U));
  require(reserved.valid && reserved.events.size() == 1U &&
              reserved.events.front().kind ==
                  glyphrelay::OwnerSignalingEventKind::receiver_reserved,
          "one receiver reservation must invalidate the open-link phase");
  const auto offered = protocol.receive(server_message("RECEIVER_OFFER", 5U, {{"sdp", "v=0\r\n"}}));
  require(offered.valid && offered.events.size() == 1U &&
              offered.events.front().kind == glyphrelay::OwnerSignalingEventKind::receiver_offer &&
              offered.events.front().value == "v=0\r\n",
          "a valid receiver offer must enter bounded negotiation");

  const std::string candidate =
      R"({"candidate":"candidate:1 1 UDP 1 127.0.0.1 9 typ host","sdpMid":"0"})";
  const auto remote_candidate =
      protocol.receive(server_message("RECEIVER_ICE_CANDIDATE", 6U, {{"candidate", candidate}}));
  require(remote_candidate.valid && remote_candidate.events.size() == 1U &&
              remote_candidate.events.front().value == candidate,
          "remote candidates must remain opaque bounded signaling values");

  const auto local_candidate = one_outbound(protocol.send_candidate(candidate));
  require(local_candidate.at("type") == "OWNER_ICE_CANDIDATE" &&
              local_candidate.at("sequence") == 4U && local_candidate.at("candidate") == candidate,
          "local candidates must use the authenticated owner schema");
  const auto answer = one_outbound(protocol.send_answer("v=0\r\n", false));
  require(answer.at("type") == "OWNER_ANSWER" && answer.at("sequence") == 5U &&
              protocol.diagnostics().phase == glyphrelay::OwnerSignalingPhase::connected,
          "the owner answer must transition the local protocol to connected");

  const auto restart = protocol.receive(server_message(
      "RECEIVER_ICE_RESTART_OFFER", 7U, {{"sdp", "v=0\r\na=ice-options:trickle\r\n"}}));
  require(restart.valid && restart.events.front().kind ==
                               glyphrelay::OwnerSignalingEventKind::receiver_restart_offer,
          "ICE restart must be accepted only on the existing authenticated session");
  const auto restart_answer = one_outbound(protocol.send_answer("v=0\r\n", true));
  require(restart_answer.at("type") == "OWNER_ICE_RESTART_ANSWER" &&
              protocol.diagnostics().phase == glyphrelay::OwnerSignalingPhase::connected,
          "an authenticated restart answer must preserve the connected role session");

  const auto disconnected = protocol.receive(
      server_message("RECEIVER_DISCONNECTED", 8U, {{"reason", "SIGNALING_CLOSED"}}));
  require(disconnected.valid &&
              disconnected.events.front().kind ==
                  glyphrelay::OwnerSignalingEventKind::receiver_disconnected &&
              protocol.diagnostics().phase == glyphrelay::OwnerSignalingPhase::owner_only,
          "receiver disconnect must return only the owner to OWNER_ONLY");
  const auto replacement = one_outbound(protocol.create_join());
  require(replacement.at("type") == "CREATE_JOIN" && replacement.at("sequence") == 7U,
          "a replacement single-use link must require an explicit owner action");

  const auto transport_failure = protocol.transport_failed("OWNER_SIGNAL_TRANSPORT_CLOSED");
  require(!transport_failure.valid && transport_failure.close_transport &&
              transport_failure.events.size() == 1U &&
              protocol.diagnostics().phase == glyphrelay::OwnerSignalingPhase::failed &&
              !protocol.diagnostics().owner_capability_present,
          "owner transport loss must fail closed and destroy the memory-only capability");
}

void test_invalid_server_messages_fail_closed() {
  const auto initialize = [](glyphrelay::OwnerSignalingProtocol &protocol) {
    static_cast<void>(protocol.begin(false));
    const auto output = protocol.receive(
        server_message("SESSION_CREATED", 1U,
                       {{"absoluteDeadlineMs", 100U}, {"ownerCapability", kOwnerCapability}}));
    require(output.valid, "invalid-message fixture must create one owner session");
  };

  {
    glyphrelay::OwnerSignalingProtocol protocol(kOrigin);
    initialize(protocol);
    auto message = Json::parse(
        server_message("HEARTBEAT", 2U, {{"deadlineMs", 10U}, {"heartbeatSequence", 1U}}));
    message["unexpected"] = true;
    const auto result = protocol.receive(message.dump());
    require(!result.valid && result.close_transport &&
                !protocol.diagnostics().owner_capability_present,
            "unknown server fields must close signaling and erase owner authentication");
  }
  {
    glyphrelay::OwnerSignalingProtocol protocol(kOrigin);
    initialize(protocol);
    auto message = Json::parse(
        server_message("HEARTBEAT", 2U, {{"deadlineMs", 10U}, {"heartbeatSequence", 1U}}));
    message["sessionId"] = "zyxwvutsrqponmlkjihgfe";
    require(!protocol.receive(message.dump()).valid,
            "a validly shaped message for another session must fail closed");
  }
  {
    glyphrelay::OwnerSignalingProtocol protocol(kOrigin);
    initialize(protocol);
    static_cast<void>(protocol.create_join());
    const auto forged = protocol.receive(
        server_message("JOIN_CREATED", 2U,
                       {{"joinExpiresAtMs", 100U},
                        {"joinUrl", std::string("https://attacker.invalid/#join=") + kSession +
                                        "." + kJoinCapability}}));
    require(!forged.valid &&
                protocol.diagnostics().phase == glyphrelay::OwnerSignalingPhase::failed,
            "a server link outside the configured exact origin must fail closed");
  }
  {
    glyphrelay::OwnerSignalingProtocol protocol(kOrigin);
    initialize(protocol);
    const auto stale = protocol.receive(
        server_message("HEARTBEAT", 1U, {{"deadlineMs", 10U}, {"heartbeatSequence", 1U}}));
    require(!stale.valid,
            "a replayed or stale server sequence must fail the connection generation");
  }
  {
    glyphrelay::OwnerSignalingProtocol protocol(kOrigin);
    initialize(protocol);
    const auto commented =
        std::string("/* forged */") +
        server_message("HEARTBEAT", 2U, {{"deadlineMs", 10U}, {"heartbeatSequence", 1U}});
    require(!protocol.receive(commented).valid,
            "JSON comments must not extend the exact signaling grammar");
  }
  {
    glyphrelay::OwnerSignalingProtocol protocol(kOrigin);
    initialize(protocol);
    const std::string duplicate =
        std::string("{\"protocolVersion\":\"") + std::string(glyphrelay::kSignalingProtocol) +
        "\",\"sequence\":2,\"sequence\":2,\"sessionId\":\"" + kSession +
        "\",\"type\":\"HEARTBEAT\",\"deadlineMs\":10,\"heartbeatSequence\":1}";
    require(!protocol.receive(duplicate).valid,
            "duplicate JSON keys must fail before exact schema validation");
  }
}

void test_stop_clears_capability_after_authenticating_message() {
  glyphrelay::OwnerSignalingProtocol protocol(kOrigin);
  static_cast<void>(protocol.begin(false));
  static_cast<void>(protocol.receive(
      server_message("SESSION_CREATED", 1U,
                     {{"absoluteDeadlineMs", 100U}, {"ownerCapability", kOwnerCapability}})));
  const auto stopped = protocol.stop(false);
  const auto message = one_outbound(stopped);
  require(message.at("type") == "OWNER_STOP" && message.at("ownerCapability") == kOwnerCapability &&
              stopped.close_transport &&
              protocol.diagnostics().phase == glyphrelay::OwnerSignalingPhase::stopped &&
              !protocol.diagnostics().owner_capability_present,
          "owner stop must authenticate once, close transport, and erase the capability");
}

} // namespace

int main() {
  test_origin_validation();
  test_complete_owner_protocol_flow();
  test_invalid_server_messages_fail_closed();
  test_stop_clears_capability_after_authenticating_message();
  std::cout << "owner signaling tests passed\n";
  return 0;
}
