#include "glyphrelay/control_protocol.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;

constexpr std::string_view kSession = "abcdefghijklmnopqrstuv";

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

Json one_message(const glyphrelay::SenderControlOutput &output) {
  require(output.valid && output.outbound_messages.size() == 1U,
          "control operation must produce one valid message");
  return Json::parse(output.outbound_messages.front());
}

std::string receiver_message(std::string_view type, std::uint64_t sequence,
                             Json fields = Json::object()) {
  Json message = {
      {"protocolVersion", glyphrelay::kControlProtocol},
      {"sequence", sequence},
      {"sessionId", kSession},
      {"type", type},
  };
  message.update(fields);
  return message.dump();
}

Json receiver_stats(Json fields) {
  Json value = {
      {"latestCallbackTimeMs", nullptr},        {"latestCaptureTimeMs", nullptr},
      {"latestExpectedDisplayTimeMs", nullptr}, {"latestPresentationTimeMs", nullptr},
      {"latestReceiveTimeMs", nullptr},
  };
  value.update(fields);
  return value;
}

void test_exact_lifecycle_and_clock_contract() {
  glyphrelay::SenderControlProtocol protocol{std::string(kSession)};
  const auto hello = one_message(protocol.begin(1U));
  require(hello.size() == 5U && hello.at("type") == "HELLO" && hello.at("mediaEpoch") == 1U &&
              hello.at("sequence") == 1U,
          "HELLO must bind the initial media epoch with an exact schema");

  for (std::uint64_t index = 0U; index < 5U; ++index) {
    const auto sent_at = static_cast<double>(index) * 0.25;
    const auto request = one_message(protocol.request_clock(sent_at));
    const auto request_sequence = index + 2U;
    require(request.size() == 5U && request.at("type") == "CLOCK_REQUEST" &&
                request.at("sequence") == request_sequence &&
                request.at("senderSendTimeMs") == sent_at,
            "initial clock burst must preserve exact request identity and time");
    const auto response =
        protocol.receive(receiver_message("CLOCK_RESPONSE", index + 1U,
                                          {{"receiverReceiveTimeMs", 10.0 + sent_at},
                                           {"receiverSendTimeMs", 11.0 + sent_at},
                                           {"requestSequence", request_sequence},
                                           {"senderSendTimeMs", sent_at}}),
                         100.0 + static_cast<double>(index));
    require(
        response.valid && response.events.size() == 1U &&
            response.events.front().kind == glyphrelay::ReceiverControlEventKind::clock_response &&
            response.events.front().request_sequence == request_sequence &&
            response.events.front().sender_receive_time_ms == 100.0 + static_cast<double>(index),
        "clock response must match one outstanding sender request exactly");
  }
  const auto early = protocol.request_clock(5'000.5);
  require(early.valid && !early.close_channel && early.outbound_messages.empty(),
          "post-burst clock requests must not be emitted before five seconds");
  const auto scheduled = one_message(protocol.request_clock(5'001.0));
  require(scheduled.at("type") == "CLOCK_REQUEST" && scheduled.at("sequence") == 7U,
          "post-burst clock request must be admitted at the exact five-second boundary");

  const auto stats =
      protocol.receive(receiver_message("RECEIVER_STATS", 6U,
                                        receiver_stats({
                                            {"compositorFrames", 30U},
                                            {"decodedFrames", 31U},
                                            {"droppedFrames", 1U},
                                            {"latestCallbackTimeMs", 20.0},
                                            {"latestCaptureTimeMs", 12.0},
                                            {"latestExpectedDisplayTimeMs", 21.0},
                                            {"latestPresentationTimeMs", 19.0},
                                            {"latestPresentedRtpTimestamp", 0xFFFF'FFF0U},
                                            {"latestReceiveTimeMs", 14.0},
                                        })),
                       1'100.0);
  require(stats.valid && stats.events.size() == 1U &&
              stats.events.front().kind == glyphrelay::ReceiverControlEventKind::receiver_stats &&
              stats.events.front().stats.latest_presented_rtp_timestamp == 0xFFFF'FFF0U,
          "bounded cumulative receiver statistics must preserve the latest wire timestamp");

  const auto pause = one_message(protocol.pause(1U));
  require(pause.at("type") == "SESSION_PAUSED" && pause.at("mediaEpoch") == 1U &&
              pause.at("sequence") == 8U,
          "pause must name the locally closed media epoch");
  const auto paused = protocol.receive(
      receiver_message("SESSION_PAUSED_ACK", 7U, {{"requestSequence", 8U}}), 1'101.0);
  require(paused.valid && protocol.diagnostics().phase == glyphrelay::SenderControlPhase::paused,
          "pause acknowledgment must match the pending request sequence");

  const auto resume = one_message(protocol.resume(2U, 9U));
  require(resume.at("type") == "SESSION_RESUMED" && resume.at("mediaEpoch") == 2U &&
              resume.at("dependencyEpoch") == 9U && resume.at("sequence") == 9U,
          "resume must bind both new media and dependency epochs");
  const auto resumed = protocol.receive(
      receiver_message("SESSION_RESUMED_ACK", 8U, {{"requestSequence", 9U}}), 1'102.0);
  require(resumed.valid &&
              protocol.diagnostics().phase == glyphrelay::SenderControlPhase::connected,
          "resume acknowledgment must reopen only the matching pending transition");

  const auto ended_message = one_message(protocol.end(2U, "OWNER_STOP"));
  require(ended_message.at("type") == "SESSION_ENDED" &&
              ended_message.at("reason") == "OWNER_STOP" && ended_message.at("sequence") == 10U,
          "session end must carry a bounded reason and current media epoch");
  const auto ended = protocol.receive(
      receiver_message("SESSION_ENDED_ACK", 9U, {{"requestSequence", 10U}}), 1'103.0);
  require(ended.valid && ended.close_channel &&
              protocol.diagnostics().phase == glyphrelay::SenderControlPhase::ended,
          "matching end acknowledgment must close the reliable control channel");
}

void test_clock_correlation_estimator() {
  glyphrelay::ClockCorrelationEstimator estimator;
  const auto observe = [&estimator](std::uint64_t sequence, double sender_send,
                                    double receiver_receive, double receiver_send,
                                    double sender_receive) {
    return estimator.observe({
        .kind = glyphrelay::ReceiverControlEventKind::clock_response,
        .request_sequence = sequence,
        .sender_send_time_ms = sender_send,
        .receiver_receive_time_ms = receiver_receive,
        .receiver_send_time_ms = receiver_send,
        .sender_receive_time_ms = sender_receive,
        .stats = {},
        .reason = {},
    });
  };
  require(observe(1U, 100.0, 150.0, 151.0, 111.0), "first four-timestamp sample must be accepted");
  require(observe(2U, 200.0, 245.0, 246.0, 209.0), "second bounded-offset sample must be accepted");
  require(observe(3U, 300.0, 342.0, 343.0, 307.0), "third bounded-offset sample must be accepted");
  const auto stable = estimator.snapshot();
  require(stable.valid && stable.samples.size() == 3U && stable.network_delay_ms == 6.0 &&
              stable.offset_ms == 39.0 && stable.uncertainty_ms == 9.0 && stable.reset_count == 0U,
          "clock estimator must use minimum delay plus three-sample offset variation");

  require(observe(4U, 400.0, 500.0, 501.0, 408.0),
          "offset discontinuity sample must establish a fresh correlation epoch");
  const auto reset = estimator.snapshot();
  require(reset.valid && reset.samples.size() == 1U && reset.reset_count == 1U &&
              reset.network_delay_ms == 7.0,
          "offset jumps beyond uncertainty must reset prior clock evidence");
  require(!observe(5U, 500.0, 600.0, 700.0, 510.0) && estimator.snapshot().samples.size() == 1U,
          "negative network-delay samples must not contaminate clock evidence");
}

void initialize(glyphrelay::SenderControlProtocol &protocol) {
  require(protocol.begin(1U).valid, "negative fixture must begin one control session");
}

void test_invalid_input_fails_closed() {
  bool rejected_session = false;
  try {
    glyphrelay::SenderControlProtocol invalid("short");
    static_cast<void>(invalid);
  } catch (const std::invalid_argument &) {
    rejected_session = true;
  }
  require(rejected_session, "control session identifiers must have the exact signaling shape");

  {
    glyphrelay::SenderControlProtocol protocol{std::string(kSession)};
    initialize(protocol);
    const auto command =
        protocol.receive(receiver_message("KEYBOARD_INPUT", 1U, {{"key", "Enter"}}), 0.0);
    require(!command.valid && command.close_channel &&
                protocol.diagnostics().phase == glyphrelay::SenderControlPhase::failed,
            "remote-control messages must fail the entire peer control session");
  }
  {
    glyphrelay::SenderControlProtocol protocol{std::string(kSession)};
    initialize(protocol);
    const std::string duplicate =
        std::string("{\"protocolVersion\":\"") + std::string(glyphrelay::kControlProtocol) +
        "\",\"sequence\":1,\"sequence\":1,\"sessionId\":\"" + std::string(kSession) +
        "\",\"type\":\"RECEIVER_STATS\",\"compositorFrames\":0,"
        "\"decodedFrames\":0,\"droppedFrames\":0,"
        "\"latestCallbackTimeMs\":null,\"latestCaptureTimeMs\":null,"
        "\"latestExpectedDisplayTimeMs\":null,\"latestPresentationTimeMs\":null,"
        "\"latestPresentedRtpTimestamp\":null,\"latestReceiveTimeMs\":null}";
    require(!protocol.receive(duplicate, 0.0).valid,
            "duplicate control keys must fail before schema validation");
  }
  {
    glyphrelay::SenderControlProtocol protocol{std::string(kSession)};
    initialize(protocol);
    const auto commented = std::string("/* extension */") +
                           receiver_message("RECEIVER_STATS", 1U,
                                            receiver_stats({
                                                {"compositorFrames", 0U},
                                                {"decodedFrames", 0U},
                                                {"droppedFrames", 0U},
                                                {"latestPresentedRtpTimestamp", nullptr},
                                            }));
    require(!protocol.receive(commented, 0.0).valid,
            "JSON comments must not extend the exact control grammar");
  }
  {
    glyphrelay::SenderControlProtocol protocol{std::string(kSession)};
    initialize(protocol);
    const auto first = receiver_message("RECEIVER_STATS", 1U,
                                        receiver_stats({
                                            {"compositorFrames", 10U},
                                            {"decodedFrames", 10U},
                                            {"droppedFrames", 1U},
                                            {"latestCallbackTimeMs", 10.0},
                                            {"latestPresentedRtpTimestamp", nullptr},
                                        }));
    require(protocol.receive(first, 0.0).valid, "statistics control must pass");
    const auto regressed = receiver_message("RECEIVER_STATS", 2U,
                                            receiver_stats({
                                                {"compositorFrames", 9U},
                                                {"decodedFrames", 10U},
                                                {"droppedFrames", 1U},
                                                {"latestCallbackTimeMs", 11.0},
                                                {"latestPresentedRtpTimestamp", nullptr},
                                            }));
    require(!protocol.receive(regressed, 1'000.0).valid,
            "cumulative untrusted telemetry must never regress");
  }
  {
    glyphrelay::SenderControlProtocol protocol{std::string(kSession)};
    initialize(protocol);
    for (std::uint64_t index = 0U; index < 10U; ++index) {
      const auto message =
          receiver_message("RECEIVER_STATS", index + 1U,
                           receiver_stats({
                               {"compositorFrames", index},
                               {"decodedFrames", index},
                               {"droppedFrames", 0U},
                               {"latestCallbackTimeMs", static_cast<double>(index)},
                               {"latestPresentedRtpTimestamp", nullptr},
                           }));
      require(protocol.receive(message, static_cast<double>(index)).valid,
              "the first ten receiver messages in a rolling second must pass");
    }
    const auto flood = receiver_message("RECEIVER_STATS", 11U,
                                        receiver_stats({
                                            {"compositorFrames", 10U},
                                            {"decodedFrames", 10U},
                                            {"droppedFrames", 0U},
                                            {"latestCallbackTimeMs", 10.0},
                                            {"latestPresentedRtpTimestamp", nullptr},
                                        }));
    require(!protocol.receive(flood, 999.0).valid &&
                protocol.diagnostics().reason == "CONTROL_RECEIVER_RATE_OR_CLOCK_INVALID",
            "the eleventh receiver message in a rolling second must fail closed");
  }
  {
    glyphrelay::SenderControlProtocol protocol{std::string(kSession)};
    initialize(protocol);
    require(!protocol.receive(std::string(glyphrelay::kMaximumControlBytes + 1U, 'x'), 0.0).valid,
            "control messages larger than four KiB must fail before parsing");
  }
  {
    glyphrelay::SenderControlProtocol protocol{std::string(kSession)};
    initialize(protocol);
    for (std::uint64_t index = 0U; index < 5U; ++index) {
      require(protocol.request_clock(static_cast<double>(index)).outbound_messages.size() == 1U,
              "initial unanswered clock request must remain bounded");
    }
    for (std::uint64_t index = 1U; index <= 3U; ++index) {
      require(protocol.request_clock(4.0 + 5'000.0 * static_cast<double>(index))
                      .outbound_messages.size() == 1U,
              "periodic unanswered clock request must remain bounded");
    }
    const auto exhausted = protocol.request_clock(20'004.0);
    require(!exhausted.valid && exhausted.close_channel &&
                protocol.diagnostics().outstanding_clock_requests == 0U,
            "unanswered clock requests must fail closed before their map can grow unbounded");
  }
}

} // namespace

int main() {
  test_exact_lifecycle_and_clock_contract();
  test_clock_correlation_estimator();
  test_invalid_input_fails_closed();
  std::cout << "sender control protocol tests passed\n";
  return 0;
}
