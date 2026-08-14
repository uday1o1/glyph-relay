#include "glyphrelay/owner_signaling.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;

struct Arguments {
  std::string origin;
  std::string ca_certificate;
};

std::optional<Arguments> parse_arguments(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "Usage: glyphrelay_owner_signaling_fixture --origin ORIGIN [--ca FILE]\n";
    return std::nullopt;
  }
  const bool has_ca =
      argc == 5 && std::string_view(argv[3]) == "--ca" && !std::string_view(argv[4]).empty();
  if ((argc != 3 && !has_ca) || std::string_view(argv[1]) != "--origin" ||
      std::string_view(argv[2]).empty()) {
    throw std::invalid_argument("fixture_arguments_invalid");
  }
  return Arguments{.origin = argv[2], .ca_certificate = has_ca ? argv[4] : ""};
}

void emit(std::string_view type, std::string_view value = {}) {
  Json message = {{"type", type}};
  if (!value.empty()) {
    message["value"] = value;
  }
  std::cout << message.dump() << '\n' << std::flush;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    if (!arguments) {
      return 0;
    }
    glyphrelay::OwnerSignalingClient client(
        {.origin = arguments->origin, .ca_certificate_pem_file = arguments->ca_certificate});
    if (!client.start()) {
      emit("ERROR", client.diagnostics().reason);
      return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto event = client.wait_for_event(std::chrono::milliseconds(250));
      if (!event) {
        continue;
      }
      switch (event->kind) {
      case glyphrelay::OwnerSignalingEventKind::session_created:
        emit("SESSION_CREATED", event->value);
        break;
      case glyphrelay::OwnerSignalingEventKind::join_created:
        emit("JOIN_CREATED", event->value);
        break;
      case glyphrelay::OwnerSignalingEventKind::receiver_reserved:
        emit("RECEIVER_RESERVED");
        break;
      case glyphrelay::OwnerSignalingEventKind::receiver_offer:
        if (!client.send_answer(event->value)) {
          emit("ERROR", client.diagnostics().reason);
          return 1;
        }
        emit("OFFER_ANSWERED");
        break;
      case glyphrelay::OwnerSignalingEventKind::receiver_restart_offer:
        if (!client.send_answer(event->value, true)) {
          emit("ERROR", client.diagnostics().reason);
          return 1;
        }
        emit("RESTART_ANSWERED");
        break;
      case glyphrelay::OwnerSignalingEventKind::receiver_candidate:
        if (!client.send_candidate(event->value)) {
          emit("ERROR", client.diagnostics().reason);
          return 1;
        }
        emit("CANDIDATE_ECHOED");
        break;
      case glyphrelay::OwnerSignalingEventKind::receiver_disconnected:
        emit("RECEIVER_DISCONNECTED", event->value);
        client.stop();
        return 0;
      case glyphrelay::OwnerSignalingEventKind::terminal:
        emit("TERMINAL", event->value);
        return event->value == "OWNER_SIGNAL_STOPPED" ? 0 : 1;
      }
    }
    client.stop(true);
    emit("ERROR", "fixture_timeout");
    return 1;
  } catch (const std::exception &error) {
    emit("ERROR", error.what());
    return 1;
  }
}
