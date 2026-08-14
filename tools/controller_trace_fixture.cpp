#include "glyphrelay/controller.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Arguments {
  std::string fixture;
  std::filesystem::path output;
};

Arguments parse_arguments(int argc, char **argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      std::cout << "usage: glyphrelay_controller_trace_fixture --fixture NAME --output PATH\n";
      std::exit(0);
    }
    if (argument == "--fixture" && index + 1 < argc) {
      arguments.fixture = argv[++index];
    } else if (argument == "--output" && index + 1 < argc) {
      arguments.output = argv[++index];
    } else {
      throw std::invalid_argument("unknown or incomplete controller fixture argument");
    }
  }
  if (arguments.fixture.empty() || arguments.output.empty()) {
    throw std::invalid_argument("--fixture and --output are required");
  }
  return arguments;
}

glyphrelay::ControllerTickInput tick_input(std::uint64_t sequence, std::uint64_t milliseconds,
                                           std::uint64_t cap_bps) {
  return {
      .arrival_sequence = sequence,
      .sender_arrival_milliseconds = milliseconds,
      .source_time_milliseconds = milliseconds,
      .dependency_epoch = 1U,
      .user_wire_cap_bps = cap_bps,
      .counters = {},
      .oldest_media_age_milliseconds = 0.0,
      .pacer_queue_bytes = 0U,
      .pacer_queue_packets = 0U,
      .drop_fraction = 0.0,
      .protected_fraction = 0.2,
      .map_level_histogram = {10U, 20U, 30U, 40U, 50U, 60U},
      .encode_latency_milliseconds = 2.0,
      .pinned_region_violation = false,
  };
}

void emit(std::ofstream &stream, glyphrelay::ProtectedRegionController &controller,
          glyphrelay::ControllerTickInput input) {
  stream << controller.tick(input).trace_json << '\n';
  if (!stream) {
    throw std::runtime_error("failed to write controller trace fixture");
  }
}

void stable_link(std::ofstream &stream) {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  std::uint64_t wire_bytes = 0U;
  std::uint64_t elementary_bytes = 0U;
  std::uint64_t frames = 0U;
  for (std::uint64_t tick = 0U; tick <= 20U; ++tick) {
    auto input = tick_input(tick + 1U, tick * 100U, 4'000'000U);
    input.counters.wire_egress_bytes = wire_bytes;
    input.counters.elementary_stream_bytes = elementary_bytes;
    input.counters.delivered_frames = frames;
    emit(stream, controller, input);
    wire_bytes += 25'000U;
    elementary_bytes += 20'000U;
    frames += 3U;
  }
}

void emphasis_overshoot(std::ofstream &stream) {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 2'000'000U});
  for (std::uint64_t tick = 0U; tick <= 15U; ++tick) {
    auto input = tick_input(tick + 1U, tick * 100U, 1'000'000U);
    input.pinned_region_violation = tick >= 2U;
    emit(stream, controller, input);
  }
}

void stale_remb(std::ofstream &stream) {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  controller.push_feedback({.arrival_sequence = 1U,
                            .sender_arrival_milliseconds = 0U,
                            .source_time_milliseconds = 0U,
                            .loss_fraction = 0.0,
                            .round_trip_time_milliseconds = 50.0,
                            .remb_bits_per_second = 1'000'000.0,
                            .remb_payload_type_valid = true,
                            .remb_rtcp_source_valid = true,
                            .receiver_decoded_frames = std::nullopt,
                            .receiver_dropped_frames = std::nullopt});
  for (std::uint64_t tick = 0U; tick <= 22U; ++tick) {
    emit(stream, controller, tick_input(tick + 2U, tick * 100U, 4'000'000U));
  }
}

void missing_remb(std::ofstream &stream) {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  for (std::uint64_t tick = 0U; tick <= 10U; ++tick) {
    emit(stream, controller, tick_input(tick + 1U, tick * 100U, 2'000'000U));
  }
}

void sudden_collapse(std::ofstream &stream) {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  for (std::uint64_t tick = 0U; tick <= 30U; ++tick) {
    auto input = tick_input(tick + 1U, tick * 100U, 2'000'000U);
    if (tick >= 5U) {
      input.oldest_media_age_milliseconds = 100.0;
    }
    emit(stream, controller, input);
  }
}

void high_rtt_without_loss(std::ofstream &stream) {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  controller.push_feedback({.arrival_sequence = 1U,
                            .sender_arrival_milliseconds = 0U,
                            .source_time_milliseconds = 0U,
                            .loss_fraction = 0.0,
                            .round_trip_time_milliseconds = 300.0,
                            .remb_bits_per_second = std::nullopt,
                            .remb_payload_type_valid = false,
                            .remb_rtcp_source_valid = false,
                            .receiver_decoded_frames = std::nullopt,
                            .receiver_dropped_frames = std::nullopt});
  for (std::uint64_t tick = 0U; tick <= 10U; ++tick) {
    emit(stream, controller, tick_input(tick + 2U, tick * 100U, 4'000'000U));
  }
}

void recovery(std::ofstream &stream) {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  std::uint64_t sequence = 1U;
  for (std::uint64_t milliseconds = 0U; milliseconds <= 4'500U; milliseconds += 100U) {
    auto input = tick_input(sequence++, milliseconds, 4'000'000U);
    input.oldest_media_age_milliseconds = 100.0;
    emit(stream, controller, input);
  }
  for (std::uint64_t milliseconds = 4'600U; milliseconds <= 12'000U; milliseconds += 100U) {
    emit(stream, controller, tick_input(sequence++, milliseconds, 4'000'000U));
  }
}

void unusable(std::ofstream &stream) {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  for (std::uint64_t tick = 0U; tick <= 80U; ++tick) {
    auto input = tick_input(tick + 1U, tick * 100U, 4'000'000U);
    input.pacer_queue_bytes = 4U * 1024U * 1024U;
    emit(stream, controller, input);
    if (controller.state() == glyphrelay::ControllerState::unusable) {
      return;
    }
  }
  throw std::runtime_error("unusable fixture did not reach UNUSABLE");
}

using Fixture = std::function<void(std::ofstream &)>;

Fixture resolve_fixture(std::string_view name) {
  if (name == "stable_link") {
    return stable_link;
  }
  if (name == "emphasis_overshoot") {
    return emphasis_overshoot;
  }
  if (name == "stale_remb") {
    return stale_remb;
  }
  if (name == "missing_remb") {
    return missing_remb;
  }
  if (name == "sudden_collapse") {
    return sudden_collapse;
  }
  if (name == "high_rtt_without_loss") {
    return high_rtt_without_loss;
  }
  if (name == "recovery") {
    return recovery;
  }
  if (name == "unusable") {
    return unusable;
  }
  throw std::invalid_argument("unknown controller trace fixture");
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    if (std::filesystem::exists(arguments.output)) {
      throw std::runtime_error("controller trace output already exists");
    }
    std::ofstream stream(arguments.output, std::ios::binary | std::ios::out);
    if (!stream) {
      throw std::runtime_error("failed to create controller trace output");
    }
    resolve_fixture(arguments.fixture)(stream);
    stream.flush();
    if (!stream) {
      throw std::runtime_error("failed to flush controller trace output");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "controller trace fixture failed: " << error.what() << '\n';
    return 1;
  }
}
