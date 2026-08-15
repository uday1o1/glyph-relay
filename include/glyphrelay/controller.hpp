#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace glyphrelay {

enum class ControllerState {
  stable,
  rate_pressure,
  congested,
  recovery,
  unusable,
};

enum class ControllerAction {
  none,
  reduce_automatic_emphasis,
  tighten_protected_threshold,
  reduce_payload_and_vbv,
  reduce_presentation_profile,
  request_recovery_idr,
  restore_presentation_profile,
  restore_payload_and_vbv,
  restore_protected_threshold,
  restore_automatic_emphasis,
  stop_unusable_link,
};

struct ControllerConfig {
  std::uint64_t base_payload_target_bps = 4'000'000U;
  double base_entry_threshold = 0.50;
  double base_exit_threshold = 0.30;
};

struct ControllerCounters {
  std::uint64_t elementary_stream_bytes = 0;
  std::uint64_t wire_egress_bytes = 0;
  std::uint64_t retransmission_bytes = 0;
  std::uint64_t delivered_frames = 0;
};

struct ControllerFeedback {
  std::uint64_t arrival_sequence = 0;
  std::uint64_t sender_arrival_milliseconds = 0;
  std::optional<std::uint64_t> source_time_milliseconds;
  std::optional<double> loss_fraction;
  std::optional<double> round_trip_time_milliseconds;
  std::optional<double> remb_bits_per_second;
  bool remb_payload_type_valid = false;
  bool remb_rtcp_source_valid = false;
  std::optional<std::uint64_t> receiver_decoded_frames;
  std::optional<std::uint64_t> receiver_dropped_frames;
};

struct ControllerTickInput {
  std::uint64_t arrival_sequence = 0;
  std::uint64_t sender_arrival_milliseconds = 0;
  std::optional<std::uint64_t> source_time_milliseconds;
  std::uint64_t dependency_epoch = 0;
  std::uint64_t user_wire_cap_bps = 0;
  ControllerCounters counters;
  double oldest_media_age_milliseconds = 0.0;
  std::uint64_t pacer_queue_bytes = 0;
  std::uint64_t pacer_queue_packets = 0;
  double drop_fraction = 0.0;
  double protected_fraction = 0.0;
  std::array<std::uint64_t, 6> map_level_histogram{};
  double encode_latency_milliseconds = 0.0;
  bool pinned_region_violation = false;
};

struct ControllerEstimate {
  std::optional<double> one_second;
  std::optional<double> five_second;
};

struct ControllerEstimatorSnapshot {
  ControllerEstimate elementary_stream_bps;
  ControllerEstimate wire_egress_bps;
  ControllerEstimate retransmission_bps;
  ControllerEstimate loss_fraction;
  ControllerEstimate round_trip_time_milliseconds;
  ControllerEstimate oldest_media_age_milliseconds;
  ControllerEstimate delivered_frames_per_second;
};

struct ControllerLevelStack {
  std::uint8_t automatic_emphasis_cap = 4;
  double protected_threshold_delta = 0.0;
  std::size_t payload_and_vbv_step = 0;
  std::uint64_t payload_target_bps = 0;
  std::string presentation_profile = "720p30";
  std::uint32_t width = 1280;
  std::uint32_t height = 720;
  std::uint32_t frames_per_second = 30;
};

struct ControllerDecision {
  std::uint64_t arrival_sequence = 0;
  std::uint64_t sender_arrival_milliseconds = 0;
  std::optional<std::uint64_t> source_time_milliseconds;
  std::uint64_t dependency_epoch = 0;
  ControllerConfig controller_config;
  ControllerState prior_state = ControllerState::stable;
  ControllerState resulting_state = ControllerState::stable;
  ControllerAction action = ControllerAction::none;
  ControllerEstimatorSnapshot estimators;
  ControllerLevelStack levels;
  double entry_threshold = 0.0;
  double exit_threshold = 0.0;
  double effective_wire_cap_bps = 0.0;
  double control_reserve_bps = 0.0;
  double payload_budget_bps = 0.0;
  bool request_idr_with_parameter_sets = false;
  bool starts_geometry_epoch = false;
  bool starts_dependency_epoch = false;
  bool pinned_region_violation_visible = false;
  bool feedback_loss_available = false;
  bool feedback_rtt_available = false;
  bool feedback_remb_available = false;
  bool counter_reset = false;
  std::vector<ControllerFeedback> consumed_feedback;
  std::string reason;
  std::string trace_json;
};

class ProtectedRegionController {
public:
  explicit ProtectedRegionController(ControllerConfig config = {});
  ~ProtectedRegionController();

  ProtectedRegionController(const ProtectedRegionController &) = delete;
  ProtectedRegionController &operator=(const ProtectedRegionController &) = delete;
  ProtectedRegionController(ProtectedRegionController &&) = delete;
  ProtectedRegionController &operator=(ProtectedRegionController &&) = delete;

  void push_feedback(const ControllerFeedback &feedback);
  ControllerDecision tick(const ControllerTickInput &input);
  ControllerState state() const;
  ControllerLevelStack levels() const;

private:
  struct RateEstimator;
  struct GaugeEstimator;
  struct FeedbackValue;
  struct DegradationStep;

  ControllerLevelStack current_levels() const;
  std::uint64_t current_payload_target_bps() const;
  bool payload_at_minimum() const;
  bool at_unusable_levels() const;
  ControllerAction degrade(std::uint64_t now_milliseconds);
  ControllerAction restore(std::uint64_t now_milliseconds);
  bool action_interval_elapsed(std::uint64_t now_milliseconds) const;
  bool restoration_interval_elapsed(std::uint64_t now_milliseconds) const;
  void consume_feedback(const ControllerTickInput &input,
                        std::vector<ControllerFeedback> &consumed);

  ControllerConfig config_;
  ControllerState state_ = ControllerState::stable;
  std::deque<ControllerFeedback> pending_feedback_;
  std::optional<std::uint64_t> last_feedback_sequence_;
  std::optional<std::uint64_t> last_feedback_arrival_milliseconds_;
  std::optional<std::uint64_t> last_tick_sequence_;
  std::optional<std::uint64_t> last_tick_milliseconds_;
  std::size_t automatic_emphasis_index_ = 0;
  std::size_t protected_threshold_index_ = 0;
  std::size_t payload_step_ = 0;
  std::size_t profile_index_ = 0;
  std::size_t pressure_ticks_ = 0;
  std::optional<std::uint64_t> recovery_compliant_since_milliseconds_;
  std::optional<std::uint64_t> recovery_entered_milliseconds_;
  std::optional<std::uint64_t> stable_compliant_since_milliseconds_;
  std::optional<std::uint64_t> unusable_violation_since_milliseconds_;
  std::optional<std::uint64_t> last_action_milliseconds_;
  std::optional<std::uint64_t> last_restoration_milliseconds_;
  std::vector<DegradationStep> degradation_history_;
  std::unique_ptr<RateEstimator> elementary_estimator_;
  std::unique_ptr<RateEstimator> wire_estimator_;
  std::unique_ptr<RateEstimator> retransmission_estimator_;
  std::unique_ptr<RateEstimator> delivered_estimator_;
  std::unique_ptr<GaugeEstimator> queue_age_estimator_;
  std::unique_ptr<GaugeEstimator> loss_estimator_;
  std::unique_ptr<GaugeEstimator> rtt_estimator_;
  std::unique_ptr<FeedbackValue> latest_loss_;
  std::unique_ptr<FeedbackValue> latest_rtt_;
  std::unique_ptr<FeedbackValue> latest_remb_;
};

std::string controller_state_name(ControllerState state);
std::string controller_action_name(ControllerAction action);
std::string controller_trace_json(const ControllerTickInput &input,
                                  const ControllerDecision &decision);

} // namespace glyphrelay
