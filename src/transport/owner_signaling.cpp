#include "glyphrelay/owner_signaling.hpp"

#include <nlohmann/json.hpp>

#include <rtc/configuration.hpp>
#include <rtc/websocket.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace glyphrelay {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaximumSignalBytes = 64U * 1024U;
constexpr std::size_t kMaximumSdpBytes = 64U * 1024U;
constexpr std::size_t kMaximumCandidateBytes = 4U * 1024U;
constexpr std::size_t kMaximumPendingEvents = 32U;
constexpr std::uint64_t kMaximumSafeInteger = 9'007'199'254'740'991ULL;
constexpr auto kOwnerLivenessTimeout = std::chrono::seconds(5);
constexpr auto kWatchdogInterval = std::chrono::milliseconds(100);

const std::regex kSessionPattern("^[A-Za-z0-9_-]{22}$");
const std::regex kCapabilityPattern("^[A-Za-z0-9_-]{22,128}$");
const std::regex kDnsOrIpv4HostPattern("^[a-z0-9.-]+$");
const std::regex kIpv6HostPattern("^[0-9a-f:.%]+$");

bool valid_bounded_text(std::string_view value, std::size_t maximum_bytes) {
  return !value.empty() && value.size() <= maximum_bytes &&
         value.find('\0') == std::string_view::npos;
}

void secure_clear(std::string &value) {
  for (char &character : value) {
    volatile char *memory = &character;
    *memory = 0;
  }
  value.clear();
  value.shrink_to_fit();
}

bool valid_safe_integer(const Json &value) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    return number > 0U && number <= kMaximumSafeInteger;
  }
  if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    return number > 0 && static_cast<std::uint64_t>(number) <= kMaximumSafeInteger;
  }
  return false;
}

bool valid_deadline(const Json &value) {
  if (!value.is_number()) {
    return false;
  }
  const auto number = value.get<double>();
  return std::isfinite(number) && number > 0.0 &&
         number <= static_cast<double>(kMaximumSafeInteger);
}

std::uint64_t safe_integer(const Json &value) {
  return value.is_number_unsigned() ? value.get<std::uint64_t>()
                                    : static_cast<std::uint64_t>(value.get<std::int64_t>());
}

bool exact_keys(const Json &value, std::initializer_list<std::string_view> expected) {
  if (!value.is_object() || value.size() != expected.size()) {
    return false;
  }
  return std::all_of(expected.begin(), expected.end(),
                     [&](std::string_view key) { return value.contains(std::string(key)); });
}

bool valid_string_field(const Json &value, std::string_view key, std::size_t maximum_bytes) {
  const auto iterator = value.find(std::string(key));
  return iterator != value.end() && iterator->is_string() &&
         valid_bounded_text(iterator->get_ref<const std::string &>(), maximum_bytes);
}

bool active_phase(OwnerSignalingPhase phase) {
  return phase == OwnerSignalingPhase::creating || phase == OwnerSignalingPhase::owner_only ||
         phase == OwnerSignalingPhase::join_pending || phase == OwnerSignalingPhase::join_open ||
         phase == OwnerSignalingPhase::join_reserved || phase == OwnerSignalingPhase::negotiating ||
         phase == OwnerSignalingPhase::connected ||
         phase == OwnerSignalingPhase::restart_negotiating;
}

void append_output(OwnerSignalingOutput &destination, OwnerSignalingOutput source) {
  destination.valid = destination.valid && source.valid;
  destination.close_transport = destination.close_transport || source.close_transport;
  for (auto &message : source.outbound_messages) {
    destination.outbound_messages.push_back(std::move(message));
  }
  for (auto &event : source.events) {
    destination.events.push_back(std::move(event));
  }
}

std::string serialize(Json value) {
  const auto encoded = value.dump();
  if (encoded.empty() || encoded.size() > kMaximumSignalBytes) {
    throw std::runtime_error("OWNER_SIGNAL_MESSAGE_SIZE_INVALID");
  }
  return encoded;
}

struct ParsedOrigin {
  std::string endpoint;
};

unsigned int parse_port(std::string_view text) {
  if (text.empty() || text.size() > 5U || !std::all_of(text.begin(), text.end(), [](char value) {
        return value >= '0' && value <= '9';
      })) {
    throw std::invalid_argument("signaling_origin_port_invalid");
  }
  unsigned int port = 0U;
  for (const char value : text) {
    port = port * 10U + static_cast<unsigned int>(value - '0');
  }
  if (port == 0U || port > 65'535U) {
    throw std::invalid_argument("signaling_origin_port_invalid");
  }
  return port;
}

ParsedOrigin parse_origin(const std::string &origin) {
  const bool secure = origin.starts_with("https://");
  const bool insecure = origin.starts_with("http://");
  if (!secure && !insecure) {
    throw std::invalid_argument("signaling_origin_scheme_invalid");
  }
  const std::size_t scheme_bytes = secure ? 8U : 7U;
  const std::string_view authority(origin.data() + scheme_bytes, origin.size() - scheme_bytes);
  if (authority.empty() || authority.find_first_of("/?#@\\\r\n") != std::string_view::npos) {
    throw std::invalid_argument("signaling_origin_authority_invalid");
  }

  std::string_view host;
  std::optional<unsigned int> port;
  if (authority.front() == '[') {
    const auto closing = authority.find(']');
    if (closing == std::string_view::npos || closing == 1U ||
        !std::regex_match(std::string(authority.substr(1U, closing - 1U)), kIpv6HostPattern)) {
      throw std::invalid_argument("signaling_origin_host_invalid");
    }
    host = authority.substr(0U, closing + 1U);
    const auto remainder = authority.substr(closing + 1U);
    if (!remainder.empty()) {
      if (remainder.front() != ':') {
        throw std::invalid_argument("signaling_origin_authority_invalid");
      }
      port = parse_port(remainder.substr(1U));
    }
  } else {
    const auto separator = authority.rfind(':');
    if (separator != std::string_view::npos) {
      if (authority.find(':') != separator) {
        throw std::invalid_argument("signaling_origin_host_invalid");
      }
      host = authority.substr(0U, separator);
      port = parse_port(authority.substr(separator + 1U));
    } else {
      host = authority;
    }
    if (host.empty() || !std::regex_match(std::string(host), kDnsOrIpv4HostPattern)) {
      throw std::invalid_argument("signaling_origin_host_invalid");
    }
  }

  const bool loopback = host == "127.0.0.1" || host == "[::1]";
  if (!secure && !loopback) {
    throw std::invalid_argument("insecure_non_loopback_signaling_origin");
  }
  if (port && ((secure && *port == 443U) || (insecure && *port == 80U))) {
    throw std::invalid_argument("signaling_origin_noncanonical_default_port");
  }
  return {.endpoint =
              std::string(secure ? "wss://" : "ws://") + std::string(authority) + "/v1/signal"};
}

} // namespace

OwnerSignalingProtocol::OwnerSignalingProtocol(std::string expected_origin)
    : expected_origin_(std::move(expected_origin)) {
  static_cast<void>(parse_origin(expected_origin_));
}

OwnerSignalingProtocol::~OwnerSignalingProtocol() { clear_owner_capability(); }

OwnerSignalingOutput OwnerSignalingProtocol::begin(bool automatically_create_join) {
  if (phase_ != OwnerSignalingPhase::idle) {
    return fail("OWNER_SIGNAL_BEGIN_STATE_INVALID");
  }
  automatically_create_join_ = automatically_create_join;
  phase_ = OwnerSignalingPhase::creating;
  reason_ = "OWNER_SIGNAL_SESSION_CREATING";
  client_sequence_ = 1U;
  Json message = {
      {"protocolVersion", kSignalingProtocol},
      {"sequence", client_sequence_},
      {"type", "CREATE_SESSION"},
  };
  OwnerSignalingOutput output;
  output.outbound_messages.push_back(serialize(std::move(message)));
  return output;
}

OwnerSignalingOutput OwnerSignalingProtocol::receive(std::string_view message) {
  if (!active()) {
    return fail("OWNER_SIGNAL_RECEIVE_STATE_INVALID");
  }
  if (!valid_bounded_text(message, kMaximumSignalBytes)) {
    return fail("OWNER_SIGNAL_MESSAGE_SIZE_INVALID");
  }
  bool duplicate_key = false;
  std::vector<std::unordered_set<std::string>> object_keys;
  const auto reject_duplicate_keys = [&](int, Json::parse_event_t event, Json &parsed) {
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
  const auto decoded =
      Json::parse(message.begin(), message.end(), reject_duplicate_keys, false, false);
  if (duplicate_key || decoded.is_discarded() || !decoded.is_object() ||
      !valid_string_field(decoded, "protocolVersion", 64U) ||
      decoded.at("protocolVersion") != kSignalingProtocol ||
      !valid_string_field(decoded, "type", 64U) || !decoded.contains("sequence") ||
      !valid_safe_integer(decoded.at("sequence")) ||
      safe_integer(decoded.at("sequence")) != server_sequence_ + 1U ||
      !valid_string_field(decoded, "sessionId", 128U) ||
      !std::regex_match(decoded.at("sessionId").get<std::string>(), kSessionPattern)) {
    return fail("OWNER_SIGNAL_MESSAGE_INVALID");
  }

  const auto type = decoded.at("type").get<std::string>();
  const auto incoming_session = decoded.at("sessionId").get<std::string>();
  if (type != "SESSION_CREATED" && incoming_session != session_id_) {
    return fail("OWNER_SIGNAL_SESSION_MISMATCH");
  }

  OwnerSignalingOutput output;
  if (type == "SESSION_CREATED") {
    if (phase_ != OwnerSignalingPhase::creating ||
        !exact_keys(decoded, {"absoluteDeadlineMs", "ownerCapability", "protocolVersion",
                              "sequence", "sessionId", "type"}) ||
        !valid_string_field(decoded, "ownerCapability", 128U) ||
        !std::regex_match(decoded.at("ownerCapability").get<std::string>(), kCapabilityPattern) ||
        !decoded.contains("absoluteDeadlineMs") ||
        !valid_deadline(decoded.at("absoluteDeadlineMs"))) {
      return fail("OWNER_SIGNAL_SESSION_CREATED_INVALID");
    }
    session_id_ = incoming_session;
    owner_capability_ = decoded.at("ownerCapability").get<std::string>();
    server_sequence_ += 1U;
    phase_ = OwnerSignalingPhase::owner_only;
    reason_ = "OWNER_SIGNAL_SESSION_CREATED";
    output.events.push_back(
        {.kind = OwnerSignalingEventKind::session_created, .value = session_id_});
    if (automatically_create_join_) {
      append_output(output, create_join());
    }
    return output;
  }

  if (type == "JOIN_CREATED") {
    if (phase_ != OwnerSignalingPhase::join_pending ||
        !exact_keys(decoded, {"joinExpiresAtMs", "joinUrl", "protocolVersion", "sequence",
                              "sessionId", "type"}) ||
        !valid_string_field(decoded, "joinUrl", 512U) || !decoded.contains("joinExpiresAtMs") ||
        !valid_deadline(decoded.at("joinExpiresAtMs"))) {
      return fail("OWNER_SIGNAL_JOIN_CREATED_INVALID");
    }
    const auto join_url = decoded.at("joinUrl").get<std::string>();
    const auto prefix = expected_origin_ + "/#join=" + session_id_ + ".";
    if (!join_url.starts_with(prefix) ||
        !std::regex_match(join_url.substr(prefix.size()), kCapabilityPattern)) {
      return fail("OWNER_SIGNAL_JOIN_URL_INVALID");
    }
    server_sequence_ += 1U;
    phase_ = OwnerSignalingPhase::join_open;
    reason_ = "OWNER_SIGNAL_JOIN_CREATED";
    output.events.push_back({.kind = OwnerSignalingEventKind::join_created, .value = join_url});
    return output;
  }

  if (type == "RECEIVER_RESERVED") {
    if (phase_ != OwnerSignalingPhase::join_open ||
        !exact_keys(decoded, {"protocolVersion", "sequence", "sessionId", "type"})) {
      return fail("OWNER_SIGNAL_RECEIVER_RESERVED_INVALID");
    }
    server_sequence_ += 1U;
    phase_ = OwnerSignalingPhase::join_reserved;
    reason_ = "OWNER_SIGNAL_RECEIVER_RESERVED";
    output.events.push_back({.kind = OwnerSignalingEventKind::receiver_reserved, .value = {}});
    return output;
  }

  if (type == "RECEIVER_OFFER" || type == "RECEIVER_ICE_RESTART_OFFER") {
    const bool restart = type == "RECEIVER_ICE_RESTART_OFFER";
    if ((restart ? phase_ != OwnerSignalingPhase::connected
                 : phase_ != OwnerSignalingPhase::join_reserved) ||
        !exact_keys(decoded, {"protocolVersion", "sdp", "sequence", "sessionId", "type"}) ||
        !valid_string_field(decoded, "sdp", kMaximumSdpBytes)) {
      return fail("OWNER_SIGNAL_RECEIVER_OFFER_INVALID");
    }
    server_sequence_ += 1U;
    phase_ = restart ? OwnerSignalingPhase::restart_negotiating : OwnerSignalingPhase::negotiating;
    reason_ = restart ? "OWNER_SIGNAL_RESTART_OFFER" : "OWNER_SIGNAL_RECEIVER_OFFER";
    output.events.push_back({.kind = restart ? OwnerSignalingEventKind::receiver_restart_offer
                                             : OwnerSignalingEventKind::receiver_offer,
                             .value = decoded.at("sdp").get<std::string>()});
    return output;
  }

  if (type == "RECEIVER_ICE_CANDIDATE") {
    if ((phase_ != OwnerSignalingPhase::negotiating && phase_ != OwnerSignalingPhase::connected &&
         phase_ != OwnerSignalingPhase::restart_negotiating) ||
        !exact_keys(decoded, {"candidate", "protocolVersion", "sequence", "sessionId", "type"}) ||
        !valid_string_field(decoded, "candidate", kMaximumCandidateBytes)) {
      return fail("OWNER_SIGNAL_RECEIVER_CANDIDATE_INVALID");
    }
    server_sequence_ += 1U;
    output.events.push_back({.kind = OwnerSignalingEventKind::receiver_candidate,
                             .value = decoded.at("candidate").get<std::string>()});
    return output;
  }

  if (type == "HEARTBEAT") {
    if (!exact_keys(decoded, {"deadlineMs", "heartbeatSequence", "protocolVersion", "sequence",
                              "sessionId", "type"}) ||
        !decoded.contains("deadlineMs") || !valid_deadline(decoded.at("deadlineMs")) ||
        !decoded.contains("heartbeatSequence") ||
        !valid_safe_integer(decoded.at("heartbeatSequence"))) {
      return fail("OWNER_SIGNAL_HEARTBEAT_INVALID");
    }
    server_sequence_ += 1U;
    const auto heartbeat = safe_integer(decoded.at("heartbeatSequence"));
    if (client_sequence_ >= kMaximumSafeInteger) {
      return fail("OWNER_SIGNAL_CLIENT_SEQUENCE_EXHAUSTED");
    }
    client_sequence_ += 1U;
    Json response = {
        {"heartbeatSequence", heartbeat},
        {"ownerCapability", owner_capability_},
        {"protocolVersion", kSignalingProtocol},
        {"sequence", client_sequence_},
        {"sessionId", session_id_},
        {"type", "OWNER_HEARTBEAT_ACK"},
    };
    output.outbound_messages.push_back(serialize(std::move(response)));
    return output;
  }

  if (type == "RECEIVER_DISCONNECTED") {
    if ((phase_ != OwnerSignalingPhase::join_reserved &&
         phase_ != OwnerSignalingPhase::negotiating && phase_ != OwnerSignalingPhase::connected &&
         phase_ != OwnerSignalingPhase::restart_negotiating) ||
        !exact_keys(decoded, {"protocolVersion", "reason", "sequence", "sessionId", "type"}) ||
        !valid_string_field(decoded, "reason", 256U)) {
      return fail("OWNER_SIGNAL_RECEIVER_DISCONNECTED_INVALID");
    }
    server_sequence_ += 1U;
    phase_ = OwnerSignalingPhase::owner_only;
    reason_ = "OWNER_SIGNAL_RECEIVER_DISCONNECTED";
    output.events.push_back({.kind = OwnerSignalingEventKind::receiver_disconnected,
                             .value = decoded.at("reason").get<std::string>()});
    return output;
  }

  if (type == "SESSION_REVOKED" || type == "SESSION_EXPIRED") {
    if (!exact_keys(decoded, {"protocolVersion", "reason", "sequence", "sessionId", "type"}) ||
        !valid_string_field(decoded, "reason", 256U)) {
      return fail("OWNER_SIGNAL_TERMINAL_MESSAGE_INVALID");
    }
    server_sequence_ += 1U;
    const auto terminal_reason = decoded.at("reason").get<std::string>();
    phase_ =
        type == "SESSION_REVOKED" ? OwnerSignalingPhase::revoked : OwnerSignalingPhase::expired;
    reason_ = terminal_reason;
    clear_owner_capability();
    output.close_transport = true;
    output.events.push_back({.kind = OwnerSignalingEventKind::terminal, .value = terminal_reason});
    return output;
  }

  return fail("OWNER_SIGNAL_TYPE_INVALID");
}

OwnerSignalingOutput OwnerSignalingProtocol::create_join() {
  if (phase_ != OwnerSignalingPhase::owner_only) {
    return fail("OWNER_SIGNAL_CREATE_JOIN_STATE_INVALID");
  }
  auto output = authenticated_message("CREATE_JOIN");
  if (output.valid) {
    phase_ = OwnerSignalingPhase::join_pending;
    reason_ = "OWNER_SIGNAL_JOIN_PENDING";
  }
  return output;
}

OwnerSignalingOutput OwnerSignalingProtocol::send_answer(std::string_view sdp, bool restart) {
  const auto expected =
      restart ? OwnerSignalingPhase::restart_negotiating : OwnerSignalingPhase::negotiating;
  if (phase_ != expected || !valid_bounded_text(sdp, kMaximumSdpBytes)) {
    return fail("OWNER_SIGNAL_ANSWER_INVALID");
  }
  auto output =
      authenticated_message(restart ? "OWNER_ICE_RESTART_ANSWER" : "OWNER_ANSWER", "sdp", sdp);
  if (output.valid) {
    phase_ = OwnerSignalingPhase::connected;
    reason_ = restart ? "OWNER_SIGNAL_RESTART_ANSWER_SENT" : "OWNER_SIGNAL_ANSWER_SENT";
  }
  return output;
}

OwnerSignalingOutput OwnerSignalingProtocol::send_candidate(std::string_view candidate) {
  if ((phase_ != OwnerSignalingPhase::negotiating && phase_ != OwnerSignalingPhase::connected &&
       phase_ != OwnerSignalingPhase::restart_negotiating) ||
      !valid_bounded_text(candidate, kMaximumCandidateBytes)) {
    return fail("OWNER_SIGNAL_CANDIDATE_INVALID");
  }
  return authenticated_message("OWNER_ICE_CANDIDATE", "candidate", candidate);
}

OwnerSignalingOutput OwnerSignalingProtocol::stop(bool revoke) {
  OwnerSignalingOutput output;
  if (active() && !owner_capability_.empty()) {
    output = authenticated_message(revoke ? "OWNER_REVOKE" : "OWNER_STOP");
  }
  clear_owner_capability();
  phase_ = OwnerSignalingPhase::stopped;
  reason_ = revoke ? "OWNER_SIGNAL_REVOKED_LOCALLY" : "OWNER_SIGNAL_STOPPED";
  output.close_transport = true;
  output.events.push_back({.kind = OwnerSignalingEventKind::terminal, .value = reason_});
  return output;
}

OwnerSignalingOutput OwnerSignalingProtocol::transport_failed(std::string reason) {
  if (!active()) {
    return {};
  }
  return fail(std::move(reason));
}

OwnerSignalingDiagnostics OwnerSignalingProtocol::diagnostics() const {
  return {
      .phase = phase_,
      .transport_open = false,
      .owner_capability_present = !owner_capability_.empty(),
      .client_sequence = client_sequence_,
      .server_sequence = server_sequence_,
      .session_id = session_id_,
      .reason = reason_,
  };
}

bool OwnerSignalingProtocol::active() const { return active_phase(phase_); }

OwnerSignalingOutput OwnerSignalingProtocol::fail(std::string reason) {
  OwnerSignalingOutput output;
  output.valid = false;
  if (phase_ == OwnerSignalingPhase::failed || phase_ == OwnerSignalingPhase::revoked ||
      phase_ == OwnerSignalingPhase::expired || phase_ == OwnerSignalingPhase::stopped) {
    return output;
  }
  reason_ = std::move(reason);
  phase_ = OwnerSignalingPhase::failed;
  clear_owner_capability();
  output.close_transport = true;
  output.events.push_back({.kind = OwnerSignalingEventKind::terminal, .value = reason_});
  return output;
}

OwnerSignalingOutput OwnerSignalingProtocol::authenticated_message(std::string_view type,
                                                                   std::string_view field_name,
                                                                   std::string_view field_value) {
  if (owner_capability_.empty() || session_id_.empty() || client_sequence_ >= kMaximumSafeInteger) {
    return fail("OWNER_SIGNAL_AUTHENTICATED_SEND_INVALID");
  }
  client_sequence_ += 1U;
  Json message = {
      {"ownerCapability", owner_capability_},
      {"protocolVersion", kSignalingProtocol},
      {"sequence", client_sequence_},
      {"sessionId", session_id_},
      {"type", type},
  };
  if (!field_name.empty()) {
    message[std::string(field_name)] = field_value;
  }
  OwnerSignalingOutput output;
  try {
    output.outbound_messages.push_back(serialize(std::move(message)));
  } catch (const std::exception &) {
    return fail("OWNER_SIGNAL_MESSAGE_SIZE_INVALID");
  }
  return output;
}

void OwnerSignalingProtocol::clear_owner_capability() { secure_clear(owner_capability_); }

struct OwnerSignalingClient::Implementation {
  explicit Implementation(OwnerSignalingClientConfig client_config)
      : config(std::move(client_config)), parsed_origin(parse_origin(config.origin)),
        websocket(websocket_config()), protocol(config.origin), watchdog([this] { watch(); }) {
    websocket.onOpen([this] { opened(); });
    websocket.onMessage([this](rtc::binary) { failed("OWNER_SIGNAL_BINARY_MESSAGE_REJECTED"); },
                        [this](rtc::string message) { received(std::move(message)); });
    websocket.onError([this](rtc::string) { failed("OWNER_SIGNAL_TRANSPORT_ERROR"); });
    websocket.onClosed([this] { closed(); });
  }

  ~Implementation() {
    {
      std::scoped_lock lock(mutex);
      shutdown = true;
    }
    condition.notify_all();
    websocket.resetCallbacks();
    websocket.forceClose();
    if (watchdog.joinable()) {
      watchdog.join();
    }
  }

  rtc::WebSocket::Configuration websocket_config() const {
    rtc::WebSocket::Configuration result;
    result.disableTlsVerification = false;
    result.origin = config.origin;
    result.connectionTimeout = std::chrono::seconds(5);
    result.pingInterval = std::chrono::milliseconds::zero();
    result.maxMessageSize = kMaximumSignalBytes;
    result.caCertificatePemFile = config.ca_certificate_pem_file;
    return result;
  }

  bool start() {
    {
      std::scoped_lock lock(mutex);
      if (started || shutdown) {
        return false;
      }
      started = true;
    }
    try {
      websocket.open(parsed_origin.endpoint);
      return true;
    } catch (const std::exception &) {
      failed("OWNER_SIGNAL_OPEN_FAILED");
      return false;
    }
  }

  bool create_join() {
    return run([&] { return protocol.create_join(); });
  }

  bool send_answer(std::string_view sdp, bool restart) {
    return run([&] { return protocol.send_answer(sdp, restart); });
  }

  bool send_candidate(std::string_view candidate) {
    return run([&] { return protocol.send_candidate(candidate); });
  }

  void stop(bool revoke) {
    OwnerSignalingOutput output;
    {
      std::scoped_lock lock(mutex);
      output = protocol.stop(revoke);
    }
    apply(std::move(output));
  }

  template <typename Operation> bool run(Operation operation) {
    OwnerSignalingOutput output;
    {
      std::scoped_lock lock(mutex);
      if (shutdown) {
        return false;
      }
      output = operation();
    }
    const bool valid = output.valid;
    apply(std::move(output));
    return valid;
  }

  void opened() {
    OwnerSignalingOutput output;
    {
      std::scoped_lock lock(mutex);
      if (shutdown) {
        return;
      }
      transport_open = true;
      last_server_activity = Clock::now();
      output = protocol.begin(config.automatically_create_join);
    }
    apply(std::move(output));
  }

  void received(std::string message) {
    OwnerSignalingOutput output;
    {
      std::scoped_lock lock(mutex);
      if (shutdown) {
        secure_clear(message);
        return;
      }
      output = protocol.receive(message);
      secure_clear(message);
      if (output.valid) {
        last_server_activity = Clock::now();
      }
    }
    apply(std::move(output));
  }

  void closed() {
    OwnerSignalingOutput output;
    {
      std::scoped_lock lock(mutex);
      transport_open = false;
      if (!shutdown) {
        output = protocol.transport_failed("OWNER_SIGNAL_TRANSPORT_CLOSED");
      }
    }
    apply(std::move(output));
  }

  void failed(std::string reason) {
    OwnerSignalingOutput output;
    {
      std::scoped_lock lock(mutex);
      transport_open = false;
      if (!shutdown) {
        if (reason == "OWNER_SIGNAL_EVENT_QUEUE_OVERFLOW") {
          events.clear();
        }
        output = protocol.transport_failed(std::move(reason));
      }
    }
    apply(std::move(output));
  }

  void apply(OwnerSignalingOutput output) {
    for (auto &message : output.outbound_messages) {
      bool sent = false;
      try {
        sent = websocket.isOpen() && websocket.send(message);
      } catch (const std::exception &) {
        sent = false;
      }
      secure_clear(message);
      if (!sent) {
        failed("OWNER_SIGNAL_SEND_FAILED");
        return;
      }
    }

    bool event_overflow = false;
    {
      std::scoped_lock lock(mutex);
      for (auto &event : output.events) {
        if (events.size() >= kMaximumPendingEvents) {
          event_overflow = true;
          break;
        }
        events.push_back(std::move(event));
      }
    }
    condition.notify_all();
    if (event_overflow) {
      failed("OWNER_SIGNAL_EVENT_QUEUE_OVERFLOW");
      return;
    }
    if (output.close_transport && !websocket.isClosed()) {
      websocket.close();
    }
  }

  std::optional<OwnerSignalingEvent> wait_for_event(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex);
    condition.wait_for(lock, timeout, [&] { return !events.empty() || shutdown; });
    if (events.empty()) {
      return std::nullopt;
    }
    auto event = std::move(events.front());
    events.pop_front();
    return event;
  }

  OwnerSignalingDiagnostics diagnostics() const {
    std::scoped_lock lock(mutex);
    auto result = protocol.diagnostics();
    result.transport_open = transport_open;
    return result;
  }

  void watch() {
    std::unique_lock lock(mutex);
    while (!shutdown) {
      condition.wait_for(lock, kWatchdogInterval);
      if (shutdown) {
        break;
      }
      if (!transport_open || !protocol.active() ||
          Clock::now() - last_server_activity < kOwnerLivenessTimeout) {
        continue;
      }
      auto output = protocol.transport_failed("OWNER_SIGNAL_HEARTBEAT_TIMEOUT");
      transport_open = false;
      lock.unlock();
      apply(std::move(output));
      lock.lock();
    }
  }

  OwnerSignalingClientConfig config;
  ParsedOrigin parsed_origin;
  rtc::WebSocket websocket;
  OwnerSignalingProtocol protocol;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<OwnerSignalingEvent> events;
  Clock::time_point last_server_activity = Clock::now();
  bool started = false;
  bool transport_open = false;
  bool shutdown = false;
  std::thread watchdog;
};

OwnerSignalingClient::OwnerSignalingClient(OwnerSignalingClientConfig config)
    : implementation_(std::make_unique<Implementation>(std::move(config))) {}

OwnerSignalingClient::~OwnerSignalingClient() = default;
OwnerSignalingClient::OwnerSignalingClient(OwnerSignalingClient &&) noexcept = default;
OwnerSignalingClient &OwnerSignalingClient::operator=(OwnerSignalingClient &&) noexcept = default;

bool OwnerSignalingClient::start() { return implementation_->start(); }
bool OwnerSignalingClient::create_join() { return implementation_->create_join(); }

bool OwnerSignalingClient::send_answer(std::string_view sdp, bool restart) {
  return implementation_->send_answer(sdp, restart);
}

bool OwnerSignalingClient::send_candidate(std::string_view candidate) {
  return implementation_->send_candidate(candidate);
}

void OwnerSignalingClient::stop(bool revoke) { implementation_->stop(revoke); }

std::optional<OwnerSignalingEvent>
OwnerSignalingClient::wait_for_event(std::chrono::milliseconds timeout) {
  return implementation_->wait_for_event(timeout);
}

OwnerSignalingDiagnostics OwnerSignalingClient::diagnostics() const {
  return implementation_->diagnostics();
}

std::string_view owner_signaling_phase_name(OwnerSignalingPhase phase) {
  switch (phase) {
  case OwnerSignalingPhase::idle:
    return "IDLE";
  case OwnerSignalingPhase::creating:
    return "CREATING";
  case OwnerSignalingPhase::owner_only:
    return "OWNER_ONLY";
  case OwnerSignalingPhase::join_pending:
    return "JOIN_PENDING";
  case OwnerSignalingPhase::join_open:
    return "JOIN_OPEN";
  case OwnerSignalingPhase::join_reserved:
    return "JOIN_RESERVED";
  case OwnerSignalingPhase::negotiating:
    return "NEGOTIATING";
  case OwnerSignalingPhase::connected:
    return "CONNECTED";
  case OwnerSignalingPhase::restart_negotiating:
    return "RESTART_NEGOTIATING";
  case OwnerSignalingPhase::revoked:
    return "REVOKED";
  case OwnerSignalingPhase::expired:
    return "EXPIRED";
  case OwnerSignalingPhase::failed:
    return "FAILED";
  case OwnerSignalingPhase::stopped:
    return "STOPPED";
  }
  return "UNKNOWN";
}

} // namespace glyphrelay
