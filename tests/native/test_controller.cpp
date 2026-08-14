#include "glyphrelay/controller.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

bool close(double left, double right) { return std::abs(left - right) <= 1e-9; }

glyphrelay::ControllerTickInput tick_input(std::uint64_t sequence, std::uint64_t milliseconds,
                                           std::uint64_t cap_bps = 4'000'000U) {
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
      .map_level_histogram = {1U, 2U, 3U, 4U, 5U, 6U},
      .encode_latency_milliseconds = 2.0,
      .pinned_region_violation = false,
  };
}

void test_configuration_and_cumulative_estimators() {
  bool rejected = false;
  try {
    glyphrelay::ProtectedRegionController invalid({.base_payload_target_bps = 99'999U});
    static_cast<void>(invalid);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "a payload target below the frozen minimum must fail");

  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 2'000'000U});
  auto first = tick_input(1U, 0U);
  const auto baseline = controller.tick(first);
  require(!baseline.estimators.wire_egress_bps.one_second,
          "the first cumulative sample must establish only a baseline");

  auto second = tick_input(2U, 100U);
  second.counters = {
      .elementary_stream_bytes = 10'000U,
      .wire_egress_bytes = 12'500U,
      .retransmission_bytes = 1'250U,
      .delivered_frames = 3U,
  };
  const auto measured = controller.tick(second);
  require(measured.estimators.elementary_stream_bps.one_second &&
              close(*measured.estimators.elementary_stream_bps.one_second, 800'000.0) &&
              measured.estimators.wire_egress_bps.one_second &&
              close(*measured.estimators.wire_egress_bps.one_second, 1'000'000.0) &&
              measured.estimators.retransmission_bps.one_second &&
              close(*measured.estimators.retransmission_bps.one_second, 100'000.0) &&
              measured.estimators.delivered_frames_per_second.one_second &&
              close(*measured.estimators.delivered_frames_per_second.one_second, 30.0),
          "cumulative deltas must produce exact rate samples");

  auto reset = tick_input(3U, 200U);
  reset.counters = {.elementary_stream_bytes = 1U,
                    .wire_egress_bytes = 1U,
                    .retransmission_bytes = 1U,
                    .delivered_frames = 1U};
  const auto reset_decision = controller.tick(reset);
  require(reset_decision.counter_reset && !reset_decision.estimators.wire_egress_bps.one_second &&
              !reset_decision.estimators.elementary_stream_bps.one_second,
          "counter decreases must reset affected estimates without a negative sample");
}

void test_feedback_causality_freshness_and_remb() {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  controller.push_feedback({.arrival_sequence = 2U,
                            .sender_arrival_milliseconds = 100U,
                            .source_time_milliseconds = 50U,
                            .loss_fraction = 0.01,
                            .round_trip_time_milliseconds = 40.0,
                            .remb_bits_per_second = 1'000'000.0,
                            .remb_payload_type_valid = true,
                            .remb_rtcp_source_valid = true});
  controller.push_feedback({.arrival_sequence = 3U,
                            .sender_arrival_milliseconds = 100U,
                            .source_time_milliseconds = 51U,
                            .loss_fraction = 0.05,
                            .round_trip_time_milliseconds = 300.0});

  const auto before = controller.tick(tick_input(1U, 0U));
  require(before.consumed_feedback.empty() && !before.feedback_remb_available,
          "a tick must not consume future arrival sequences");
  const auto consumed = controller.tick(tick_input(4U, 200U));
  require(consumed.consumed_feedback.size() == 2U && consumed.feedback_loss_available &&
              consumed.feedback_rtt_available && consumed.feedback_remb_available &&
              close(consumed.effective_wire_cap_bps, 900'000.0),
          "eligible equal-time feedback must be consumed in arrival-sequence order");
  require(consumed.estimators.loss_fraction.one_second &&
              close(*consumed.estimators.loss_fraction.one_second, 0.01),
          "a zero-dt feedback update must obey the frozen EWMA equation");

  const auto stale = controller.tick(tick_input(5U, 2'101U));
  require(!stale.feedback_remb_available && !stale.feedback_loss_available &&
              !stale.estimators.loss_fraction.one_second &&
              close(stale.effective_wire_cap_bps, 4'000'000.0),
          "feedback older than two seconds must become unavailable, not zero");
}

void test_pressure_degradation_timing_and_pin_visibility() {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 2'000'000U});
  auto first = tick_input(1U, 0U, 1'000'000U);
  controller.tick(first);
  controller.tick(tick_input(2U, 100U, 1'000'000U));
  auto third = tick_input(3U, 200U, 1'000'000U);
  third.pinned_region_violation = true;
  const auto pressure = controller.tick(third);
  require(pressure.resulting_state == glyphrelay::ControllerState::rate_pressure &&
              pressure.action == glyphrelay::ControllerAction::reduce_automatic_emphasis &&
              pressure.levels.automatic_emphasis_cap == 3U &&
              pressure.pinned_region_violation_visible,
          "three pressure ticks must take one ordered step without hiding pin violation");
  const auto too_soon = controller.tick(tick_input(4U, 600U, 1'000'000U));
  require(too_soon.action == glyphrelay::ControllerAction::none,
          "knob steps must be separated by at least 500 milliseconds");
  const auto next = controller.tick(tick_input(5U, 700U, 1'000'000U));
  require(next.action == glyphrelay::ControllerAction::reduce_automatic_emphasis &&
              next.levels.automatic_emphasis_cap == 2U,
          "the next pressure step must follow the exact automatic emphasis stack");
}

void test_hard_congestion_recovery_and_profile_epoch() {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  std::uint64_t sequence = 1U;
  glyphrelay::ControllerDecision decision;
  for (std::uint64_t milliseconds = 0U; milliseconds <= 3'500U; milliseconds += 100U) {
    auto input = tick_input(sequence++, milliseconds);
    input.oldest_media_age_milliseconds = 100.0;
    decision = controller.tick(input);
  }
  require(decision.resulting_state == glyphrelay::ControllerState::congested &&
              decision.action == glyphrelay::ControllerAction::reduce_presentation_profile &&
              decision.levels.presentation_profile == "1080p24" &&
              decision.request_idr_with_parameter_sets && decision.starts_geometry_epoch &&
              decision.starts_dependency_epoch,
          "ordered hard-congestion steps must reach a profile transition with a new epoch IDR");

  std::optional<std::uint64_t> recovery_entry;
  std::optional<std::uint64_t> profile_restoration;
  for (std::uint64_t milliseconds = 3'600U; milliseconds <= 9'000U; milliseconds += 100U) {
    auto input = tick_input(sequence++, milliseconds);
    decision = controller.tick(input);
    if (decision.action == glyphrelay::ControllerAction::request_recovery_idr) {
      recovery_entry = milliseconds;
      require(decision.resulting_state == glyphrelay::ControllerState::recovery &&
                  decision.request_idr_with_parameter_sets,
              "recovery entry must request a clean IDR");
    }
    if (decision.action == glyphrelay::ControllerAction::restore_presentation_profile) {
      if (!profile_restoration) {
        profile_restoration = milliseconds;
      }
      require(decision.starts_geometry_epoch && decision.starts_dependency_epoch,
              "profile restoration must start geometry and dependency epochs");
    }
  }
  require(recovery_entry && profile_restoration &&
              *profile_restoration - *recovery_entry == 2'000U &&
              decision.resulting_state == glyphrelay::ControllerState::stable,
          "recovery must restore a profile after two seconds and become stable after compliance");
}

void test_unusable_floor_and_trace_identity() {
  glyphrelay::ProtectedRegionController controller({.base_payload_target_bps = 100'000U});
  glyphrelay::ControllerDecision decision;
  std::uint64_t sequence = 1U;
  for (std::uint64_t milliseconds = 0U; milliseconds <= 8'000U; milliseconds += 100U) {
    auto input = tick_input(sequence++, milliseconds);
    input.pacer_queue_bytes = 4U * 1024U * 1024U;
    decision = controller.tick(input);
    if (decision.resulting_state == glyphrelay::ControllerState::unusable) {
      break;
    }
  }
  require(decision.resulting_state == glyphrelay::ControllerState::unusable &&
              decision.action == glyphrelay::ControllerAction::stop_unusable_link &&
              decision.levels.presentation_profile == "720p15" &&
              decision.levels.automatic_emphasis_cap == 0U &&
              decision.levels.payload_target_bps == 100'000U,
          "three seconds of hard violation at the exact minimum stack must stop media");
  require(decision.trace_json.find("\"action\":\"STOP_UNUSABLE_LINK\"") != std::string::npos &&
              decision.trace_json.find("\"arrivalSequence\":") != std::string::npos &&
              decision.trace_json.find("\"estimatorInputs\":") != std::string::npos &&
              decision.trace_json.find("\"selectedLevelStack\":") != std::string::npos,
          "every production decision must carry the complete canonical trace identity");
}

} // namespace

int main() {
  test_configuration_and_cumulative_estimators();
  test_feedback_causality_freshness_and_remb();
  test_pressure_degradation_timing_and_pin_visibility();
  test_hard_congestion_recovery_and_profile_epoch();
  test_unusable_floor_and_trace_identity();
  std::cout << "controller contract tests passed\n";
  return 0;
}
