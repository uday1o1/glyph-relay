#include "glyphrelay/controller.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace glyphrelay {
namespace {

constexpr std::array<std::uint8_t, 5> kAutomaticCaps = {4U, 3U, 2U, 1U, 0U};
constexpr std::array<double, 4> kThresholdDeltas = {0.0, 0.05, 0.10, 0.15};
constexpr std::uint64_t kMinimumPayloadBps = 100'000U;
constexpr std::uint64_t kFeedbackMaximumAgeMilliseconds = 2'000U;
constexpr std::uint64_t kActionIntervalMilliseconds = 500U;
constexpr std::uint64_t kReversalIntervalMilliseconds = 2'000U;
constexpr std::uint64_t kRecoveryEntryMilliseconds = 1'000U;
constexpr std::uint64_t kStableReturnMilliseconds = 5'000U;
constexpr std::uint64_t kUnusableViolationMilliseconds = 3'000U;
constexpr std::uint64_t kPacerHardBytes = 4U * 1024U * 1024U;

struct PresentationProfile {
  std::string_view name;
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t frames_per_second;
};

constexpr std::array<PresentationProfile, 4> kProfiles = {
    PresentationProfile{"1080p30", 1920U, 1080U, 30U},
    PresentationProfile{"1080p24", 1920U, 1080U, 24U},
    PresentationProfile{"720p24", 1280U, 720U, 24U},
    PresentationProfile{"720p15", 1280U, 720U, 15U},
};

void require_finite_nonnegative(double value, const char *field) {
  if (!std::isfinite(value) || value < 0.0) {
    throw std::invalid_argument(std::string(field) + " must be finite and nonnegative");
  }
}

void require_fraction(double value, const char *field) {
  if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
    throw std::invalid_argument(std::string(field) + " must be a finite fraction");
  }
}

std::uint64_t round_ties_to_even(double value) {
  require_finite_nonnegative(value, "rounded value");
  if (value >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
    throw std::overflow_error("rounded value exceeds uint64");
  }
  const double floor_value = std::floor(value);
  const double fraction = value - floor_value;
  auto integer = static_cast<std::uint64_t>(floor_value);
  if (fraction > 0.5 || (fraction == 0.5 && integer % 2U != 0U)) {
    ++integer;
  }
  return integer;
}

double update_ewma(double previous, double sample, double dt_milliseconds,
                   double tau_milliseconds) {
  const double alpha = 1.0 - std::exp(-dt_milliseconds / tau_milliseconds);
  return alpha * sample + (1.0 - alpha) * previous;
}

void append_unsigned(std::string &output, std::uint64_t value) {
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    throw std::runtime_error("failed to serialize unsigned integer");
  }
  output.append(buffer.data(), result.ptr);
}

void append_size(std::string &output, std::size_t value) {
  append_unsigned(output, static_cast<std::uint64_t>(value));
}

void append_double(std::string &output, double value) {
  if (!std::isfinite(value)) {
    throw std::runtime_error("refusing to serialize non-finite controller value");
  }
  std::array<char, 64> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general,
                    std::numeric_limits<double>::max_digits10);
  if (result.ec != std::errc{}) {
    throw std::runtime_error("failed to serialize controller number");
  }
  output.append(buffer.data(), result.ptr);
  const std::string_view rendered(buffer.data(),
                                  static_cast<std::size_t>(result.ptr - buffer.data()));
  if (rendered.find_first_of(".eE") == std::string_view::npos) {
    output.append(".0");
  }
}

void append_bool(std::string &output, bool value) { output.append(value ? "true" : "false"); }

void append_optional_unsigned(std::string &output, const std::optional<std::uint64_t> &value) {
  if (value) {
    append_unsigned(output, *value);
  } else {
    output.append("null");
  }
}

void append_optional_double(std::string &output, const std::optional<double> &value) {
  if (value) {
    append_double(output, *value);
  } else {
    output.append("null");
  }
}

void append_json_string(std::string &output, std::string_view value) {
  output.push_back('"');
  for (const char character : value) {
    switch (character) {
    case '"':
      output.append("\\\"");
      break;
    case '\\':
      output.append("\\\\");
      break;
    case '\n':
      output.append("\\n");
      break;
    case '\r':
      output.append("\\r");
      break;
    case '\t':
      output.append("\\t");
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20U) {
        throw std::runtime_error("controller trace string contains a control character");
      }
      output.push_back(character);
    }
  }
  output.push_back('"');
}

void append_estimate(std::string &output, const ControllerEstimate &estimate) {
  output.append("{\"fiveSecond\":");
  append_optional_double(output, estimate.five_second);
  output.append(",\"oneSecond\":");
  append_optional_double(output, estimate.one_second);
  output.push_back('}');
}

bool fresh(const std::optional<std::uint64_t> &arrival, std::uint64_t now) {
  return arrival && now >= *arrival && now - *arrival <= kFeedbackMaximumAgeMilliseconds;
}

} // namespace

struct ProtectedRegionController::RateEstimator {
  std::optional<std::uint64_t> previous_counter;
  std::optional<std::uint64_t> previous_milliseconds;
  ControllerEstimate estimate;

  bool update(std::uint64_t counter, std::uint64_t now_milliseconds, double scale) {
    if (!previous_counter || !previous_milliseconds) {
      previous_counter = counter;
      previous_milliseconds = now_milliseconds;
      return false;
    }
    if (now_milliseconds <= *previous_milliseconds) {
      throw std::invalid_argument("controller counter time must increase");
    }
    if (counter < *previous_counter) {
      previous_counter = counter;
      previous_milliseconds = now_milliseconds;
      estimate = {};
      return true;
    }
    const auto delta = counter - *previous_counter;
    const auto dt = now_milliseconds - *previous_milliseconds;
    const double sample = static_cast<double>(delta) * scale * 1000.0 / static_cast<double>(dt);
    if (estimate.one_second) {
      estimate.one_second =
          update_ewma(*estimate.one_second, sample, static_cast<double>(dt), 1000.0);
      estimate.five_second =
          update_ewma(*estimate.five_second, sample, static_cast<double>(dt), 5000.0);
    } else {
      estimate.one_second = sample;
      estimate.five_second = sample;
    }
    previous_counter = counter;
    previous_milliseconds = now_milliseconds;
    return false;
  }
};

struct ProtectedRegionController::GaugeEstimator {
  std::optional<std::uint64_t> previous_milliseconds;
  ControllerEstimate estimate;

  void update(double sample, std::uint64_t now_milliseconds) {
    require_finite_nonnegative(sample, "controller gauge");
    if (!previous_milliseconds) {
      previous_milliseconds = now_milliseconds;
      estimate.one_second = sample;
      estimate.five_second = sample;
      return;
    }
    if (now_milliseconds < *previous_milliseconds) {
      throw std::invalid_argument("controller gauge time regressed");
    }
    const auto dt = now_milliseconds - *previous_milliseconds;
    estimate.one_second =
        update_ewma(*estimate.one_second, sample, static_cast<double>(dt), 1000.0);
    estimate.five_second =
        update_ewma(*estimate.five_second, sample, static_cast<double>(dt), 5000.0);
    previous_milliseconds = now_milliseconds;
  }
};

struct ProtectedRegionController::FeedbackValue {
  std::optional<double> value;
  std::optional<std::uint64_t> arrival_milliseconds;
};

struct ProtectedRegionController::DegradationStep {
  ControllerAction action = ControllerAction::none;
  std::uint64_t applied_milliseconds = 0;
};

ProtectedRegionController::ProtectedRegionController(ControllerConfig config)
    : config_(config), elementary_estimator_(std::make_unique<RateEstimator>()),
      wire_estimator_(std::make_unique<RateEstimator>()),
      retransmission_estimator_(std::make_unique<RateEstimator>()),
      delivered_estimator_(std::make_unique<RateEstimator>()),
      queue_age_estimator_(std::make_unique<GaugeEstimator>()),
      loss_estimator_(std::make_unique<GaugeEstimator>()),
      rtt_estimator_(std::make_unique<GaugeEstimator>()),
      latest_loss_(std::make_unique<FeedbackValue>()),
      latest_rtt_(std::make_unique<FeedbackValue>()),
      latest_remb_(std::make_unique<FeedbackValue>()) {
  if (config_.base_payload_target_bps < kMinimumPayloadBps) {
    throw std::invalid_argument("base payload target is below the frozen profile minimum");
  }
  require_fraction(config_.base_entry_threshold, "base entry threshold");
  require_fraction(config_.base_exit_threshold, "base exit threshold");
  if (config_.base_exit_threshold >= config_.base_entry_threshold ||
      config_.base_entry_threshold - config_.base_exit_threshold < 0.10) {
    throw std::invalid_argument("base thresholds must preserve at least 0.10 hysteresis");
  }
}

ProtectedRegionController::~ProtectedRegionController() = default;

void ProtectedRegionController::push_feedback(const ControllerFeedback &feedback) {
  if (feedback.arrival_sequence == 0U) {
    throw std::invalid_argument("feedback arrival sequence zero is reserved");
  }
  if (last_feedback_sequence_ && feedback.arrival_sequence <= *last_feedback_sequence_) {
    throw std::invalid_argument("feedback arrival sequence must strictly increase");
  }
  if (last_feedback_arrival_milliseconds_ &&
      feedback.sender_arrival_milliseconds < *last_feedback_arrival_milliseconds_) {
    throw std::invalid_argument("feedback arrival time must not regress");
  }
  if (last_tick_sequence_ && feedback.arrival_sequence <= *last_tick_sequence_) {
    throw std::invalid_argument("feedback pushed after its arrival sequence was already ticked");
  }
  if (last_tick_milliseconds_ && feedback.sender_arrival_milliseconds < *last_tick_milliseconds_) {
    throw std::invalid_argument("feedback pushed after its sender arrival time was already ticked");
  }
  if (feedback.loss_fraction) {
    require_fraction(*feedback.loss_fraction, "feedback loss");
  }
  if (feedback.round_trip_time_milliseconds) {
    require_finite_nonnegative(*feedback.round_trip_time_milliseconds, "feedback RTT");
  }
  if (feedback.remb_bits_per_second) {
    require_finite_nonnegative(*feedback.remb_bits_per_second, "feedback REMB");
    if (*feedback.remb_bits_per_second == 0.0) {
      throw std::invalid_argument("feedback REMB must be positive");
    }
  }
  last_feedback_sequence_ = feedback.arrival_sequence;
  last_feedback_arrival_milliseconds_ = feedback.sender_arrival_milliseconds;
  pending_feedback_.push_back(feedback);
}

void ProtectedRegionController::consume_feedback(const ControllerTickInput &input,
                                                 std::vector<ControllerFeedback> &consumed) {
  while (!pending_feedback_.empty()) {
    const auto &candidate = pending_feedback_.front();
    if (candidate.arrival_sequence >= input.arrival_sequence ||
        candidate.sender_arrival_milliseconds > input.sender_arrival_milliseconds) {
      break;
    }
    auto feedback = candidate;
    pending_feedback_.pop_front();
    if (feedback.loss_fraction) {
      loss_estimator_->update(*feedback.loss_fraction, feedback.sender_arrival_milliseconds);
      latest_loss_->value = feedback.loss_fraction;
      latest_loss_->arrival_milliseconds = feedback.sender_arrival_milliseconds;
    }
    if (feedback.round_trip_time_milliseconds) {
      rtt_estimator_->update(*feedback.round_trip_time_milliseconds,
                             feedback.sender_arrival_milliseconds);
      latest_rtt_->value = feedback.round_trip_time_milliseconds;
      latest_rtt_->arrival_milliseconds = feedback.sender_arrival_milliseconds;
    }
    if (feedback.remb_bits_per_second && feedback.remb_payload_type_valid &&
        feedback.remb_rtcp_source_valid) {
      latest_remb_->value = feedback.remb_bits_per_second;
      latest_remb_->arrival_milliseconds = feedback.sender_arrival_milliseconds;
    }
    consumed.push_back(std::move(feedback));
  }
}

std::uint64_t ProtectedRegionController::current_payload_target_bps() const {
  const double reduced = static_cast<double>(config_.base_payload_target_bps) *
                         std::pow(0.90, static_cast<double>(payload_step_));
  return std::max(kMinimumPayloadBps, round_ties_to_even(reduced));
}

bool ProtectedRegionController::payload_at_minimum() const {
  return current_payload_target_bps() == kMinimumPayloadBps;
}

bool ProtectedRegionController::at_unusable_levels() const {
  return automatic_emphasis_index_ + 1U == kAutomaticCaps.size() && payload_at_minimum() &&
         profile_index_ + 1U == kProfiles.size();
}

ControllerLevelStack ProtectedRegionController::current_levels() const {
  const auto &profile = kProfiles.at(profile_index_);
  return {
      .automatic_emphasis_cap = kAutomaticCaps.at(automatic_emphasis_index_),
      .protected_threshold_delta = kThresholdDeltas.at(protected_threshold_index_),
      .payload_and_vbv_step = payload_step_,
      .payload_target_bps = current_payload_target_bps(),
      .presentation_profile = std::string(profile.name),
      .width = profile.width,
      .height = profile.height,
      .frames_per_second = profile.frames_per_second,
  };
}

bool ProtectedRegionController::action_interval_elapsed(std::uint64_t now_milliseconds) const {
  return !last_action_milliseconds_ ||
         now_milliseconds - *last_action_milliseconds_ >= kActionIntervalMilliseconds;
}

bool ProtectedRegionController::restoration_interval_elapsed(std::uint64_t now_milliseconds) const {
  if (degradation_history_.empty()) {
    return false;
  }
  if (now_milliseconds - degradation_history_.back().applied_milliseconds <
      kReversalIntervalMilliseconds) {
    return false;
  }
  return !last_restoration_milliseconds_ ||
         now_milliseconds - *last_restoration_milliseconds_ >= kReversalIntervalMilliseconds;
}

ControllerAction ProtectedRegionController::degrade(std::uint64_t now_milliseconds) {
  ControllerAction action = ControllerAction::none;
  if (automatic_emphasis_index_ + 1U < kAutomaticCaps.size()) {
    ++automatic_emphasis_index_;
    action = ControllerAction::reduce_automatic_emphasis;
  } else if (protected_threshold_index_ + 1U < kThresholdDeltas.size()) {
    ++protected_threshold_index_;
    action = ControllerAction::tighten_protected_threshold;
  } else if (!payload_at_minimum()) {
    ++payload_step_;
    action = ControllerAction::reduce_payload_and_vbv;
  } else if (profile_index_ + 1U < kProfiles.size()) {
    ++profile_index_;
    action = ControllerAction::reduce_presentation_profile;
  }
  if (action != ControllerAction::none) {
    degradation_history_.push_back({action, now_milliseconds});
    last_action_milliseconds_ = now_milliseconds;
  }
  return action;
}

ControllerAction ProtectedRegionController::restore(std::uint64_t now_milliseconds) {
  if (!restoration_interval_elapsed(now_milliseconds)) {
    return ControllerAction::none;
  }
  const auto applied = degradation_history_.back().action;
  degradation_history_.pop_back();
  ControllerAction action = ControllerAction::none;
  switch (applied) {
  case ControllerAction::reduce_presentation_profile:
    --profile_index_;
    action = ControllerAction::restore_presentation_profile;
    break;
  case ControllerAction::reduce_payload_and_vbv:
    --payload_step_;
    action = ControllerAction::restore_payload_and_vbv;
    break;
  case ControllerAction::tighten_protected_threshold:
    --protected_threshold_index_;
    action = ControllerAction::restore_protected_threshold;
    break;
  case ControllerAction::reduce_automatic_emphasis:
    --automatic_emphasis_index_;
    action = ControllerAction::restore_automatic_emphasis;
    break;
  default:
    throw std::logic_error("controller degradation history is invalid");
  }
  last_action_milliseconds_ = now_milliseconds;
  last_restoration_milliseconds_ = now_milliseconds;
  return action;
}

ControllerDecision ProtectedRegionController::tick(const ControllerTickInput &input) {
  if (input.arrival_sequence == 0U || input.dependency_epoch == 0U ||
      input.user_wire_cap_bps == 0U) {
    throw std::invalid_argument("controller tick identifiers and cap must be nonzero");
  }
  if (last_tick_sequence_ && input.arrival_sequence <= *last_tick_sequence_) {
    throw std::invalid_argument("controller tick arrival sequence must strictly increase");
  }
  if (last_tick_milliseconds_ && input.sender_arrival_milliseconds <= *last_tick_milliseconds_) {
    throw std::invalid_argument("controller tick time must strictly increase");
  }
  require_finite_nonnegative(input.oldest_media_age_milliseconds, "oldest media age");
  require_fraction(input.drop_fraction, "drop fraction");
  require_fraction(input.protected_fraction, "protected fraction");
  require_finite_nonnegative(input.encode_latency_milliseconds, "encode latency");
  last_tick_sequence_ = input.arrival_sequence;
  last_tick_milliseconds_ = input.sender_arrival_milliseconds;

  ControllerDecision decision;
  decision.arrival_sequence = input.arrival_sequence;
  decision.sender_arrival_milliseconds = input.sender_arrival_milliseconds;
  decision.source_time_milliseconds = input.source_time_milliseconds;
  decision.dependency_epoch = input.dependency_epoch;
  decision.controller_config = config_;
  decision.prior_state = state_;
  decision.pinned_region_violation_visible = input.pinned_region_violation;
  consume_feedback(input, decision.consumed_feedback);

  const bool elementary_reset = elementary_estimator_->update(
      input.counters.elementary_stream_bytes, input.sender_arrival_milliseconds, 8.0);
  const bool wire_reset = wire_estimator_->update(input.counters.wire_egress_bytes,
                                                  input.sender_arrival_milliseconds, 8.0);
  const bool retransmission_reset = retransmission_estimator_->update(
      input.counters.retransmission_bytes, input.sender_arrival_milliseconds, 8.0);
  const bool delivered_reset = delivered_estimator_->update(input.counters.delivered_frames,
                                                            input.sender_arrival_milliseconds, 1.0);
  decision.counter_reset =
      elementary_reset || wire_reset || retransmission_reset || delivered_reset;
  queue_age_estimator_->update(input.oldest_media_age_milliseconds,
                               input.sender_arrival_milliseconds);

  decision.estimators.elementary_stream_bps = elementary_estimator_->estimate;
  decision.estimators.wire_egress_bps = wire_estimator_->estimate;
  decision.estimators.retransmission_bps = retransmission_estimator_->estimate;
  decision.estimators.oldest_media_age_milliseconds = queue_age_estimator_->estimate;
  decision.estimators.delivered_frames_per_second = delivered_estimator_->estimate;

  decision.feedback_loss_available =
      latest_loss_->value &&
      fresh(latest_loss_->arrival_milliseconds, input.sender_arrival_milliseconds);
  decision.feedback_rtt_available = latest_rtt_->value && fresh(latest_rtt_->arrival_milliseconds,
                                                                input.sender_arrival_milliseconds);
  decision.feedback_remb_available =
      latest_remb_->value &&
      fresh(latest_remb_->arrival_milliseconds, input.sender_arrival_milliseconds);
  if (decision.feedback_loss_available) {
    decision.estimators.loss_fraction = loss_estimator_->estimate;
  }
  if (decision.feedback_rtt_available) {
    decision.estimators.round_trip_time_milliseconds = rtt_estimator_->estimate;
  }

  decision.effective_wire_cap_bps = static_cast<double>(input.user_wire_cap_bps);
  if (decision.feedback_remb_available) {
    decision.effective_wire_cap_bps =
        std::min(decision.effective_wire_cap_bps, 0.90 * *latest_remb_->value);
  }
  decision.control_reserve_bps = std::max(0.10 * decision.effective_wire_cap_bps, 64'000.0);
  decision.payload_budget_bps =
      std::max(decision.effective_wire_cap_bps - decision.control_reserve_bps, 0.0);

  const auto wire_one_second = wire_estimator_->estimate.one_second;
  const auto loss_one_second = decision.estimators.loss_fraction.one_second;
  const auto rtt_one_second = decision.estimators.round_trip_time_milliseconds.one_second;
  const bool pressure =
      (wire_one_second && *wire_one_second > 0.95 * decision.effective_wire_cap_bps) ||
      input.oldest_media_age_milliseconds > 50.0 || (loss_one_second && *loss_one_second > 0.03) ||
      (rtt_one_second && *rtt_one_second > 250.0) ||
      static_cast<double>(current_payload_target_bps()) > decision.payload_budget_bps;
  const bool hard_violation =
      (wire_one_second && *wire_one_second > 1.10 * decision.effective_wire_cap_bps) ||
      input.oldest_media_age_milliseconds >= 100.0 || input.pacer_queue_bytes >= kPacerHardBytes;
  const double frame_interval =
      1000.0 / static_cast<double>(kProfiles.at(profile_index_).frames_per_second);
  const bool recovery_compliant = wire_one_second &&
                                  *wire_one_second < 0.85 * decision.effective_wire_cap_bps &&
                                  input.oldest_media_age_milliseconds < frame_interval &&
                                  (!loss_one_second || *loss_one_second < 0.01);

  if (pressure) {
    ++pressure_ticks_;
  } else {
    pressure_ticks_ = 0U;
  }
  if (recovery_compliant) {
    if (!recovery_compliant_since_milliseconds_) {
      recovery_compliant_since_milliseconds_ = input.sender_arrival_milliseconds;
    }
  } else {
    recovery_compliant_since_milliseconds_.reset();
    stable_compliant_since_milliseconds_.reset();
  }

  if (state_ != ControllerState::unusable) {
    if (hard_violation) {
      state_ = ControllerState::congested;
      recovery_entered_milliseconds_.reset();
      stable_compliant_since_milliseconds_.reset();
      decision.reason = "hard_congestion_predicate";
    } else if ((state_ == ControllerState::stable || state_ == ControllerState::recovery) &&
               pressure_ticks_ >= 3U) {
      state_ = ControllerState::rate_pressure;
      recovery_entered_milliseconds_.reset();
      stable_compliant_since_milliseconds_.reset();
      decision.reason = "three_consecutive_pressure_ticks";
    } else if ((state_ == ControllerState::rate_pressure || state_ == ControllerState::congested) &&
               recovery_compliant_since_milliseconds_ &&
               input.sender_arrival_milliseconds - *recovery_compliant_since_milliseconds_ >=
                   kRecoveryEntryMilliseconds) {
      state_ = ControllerState::recovery;
      recovery_entered_milliseconds_ = input.sender_arrival_milliseconds;
      stable_compliant_since_milliseconds_ = *recovery_compliant_since_milliseconds_;
      last_restoration_milliseconds_ = input.sender_arrival_milliseconds;
      decision.action = ControllerAction::request_recovery_idr;
      decision.request_idr_with_parameter_sets = true;
      decision.reason = "continuous_recovery_entry";
    } else if (state_ == ControllerState::recovery && stable_compliant_since_milliseconds_ &&
               input.sender_arrival_milliseconds - *stable_compliant_since_milliseconds_ >=
                   kStableReturnMilliseconds) {
      state_ = ControllerState::stable;
      decision.reason = "five_seconds_continuously_compliant";
    }
  }

  if (state_ != ControllerState::unusable && at_unusable_levels() && hard_violation) {
    if (!unusable_violation_since_milliseconds_) {
      unusable_violation_since_milliseconds_ = input.sender_arrival_milliseconds;
    }
    if (input.sender_arrival_milliseconds - *unusable_violation_since_milliseconds_ >=
        kUnusableViolationMilliseconds) {
      state_ = ControllerState::unusable;
      decision.action = ControllerAction::stop_unusable_link;
      decision.reason = "minimum_stack_hard_violation_for_three_seconds";
    }
  } else {
    unusable_violation_since_milliseconds_.reset();
  }

  if (decision.action == ControllerAction::none &&
      (state_ == ControllerState::rate_pressure || state_ == ControllerState::congested) &&
      action_interval_elapsed(input.sender_arrival_milliseconds)) {
    decision.action = degrade(input.sender_arrival_milliseconds);
    if (decision.action != ControllerAction::none) {
      decision.reason = "ordered_degradation_step";
    }
  } else if (decision.action == ControllerAction::none && recovery_compliant &&
             (state_ == ControllerState::recovery || state_ == ControllerState::stable)) {
    decision.action = restore(input.sender_arrival_milliseconds);
    if (decision.action != ControllerAction::none) {
      decision.reason = "reverse_order_restoration_step";
    }
  }

  if (decision.action == ControllerAction::reduce_presentation_profile ||
      decision.action == ControllerAction::restore_presentation_profile) {
    decision.request_idr_with_parameter_sets = true;
    decision.starts_geometry_epoch = true;
    decision.starts_dependency_epoch = true;
  }
  if (decision.reason.empty()) {
    decision.reason = "no_state_or_level_change";
  }
  decision.resulting_state = state_;
  decision.levels = current_levels();
  decision.entry_threshold =
      std::min(config_.base_entry_threshold + decision.levels.protected_threshold_delta, 1.0);
  decision.exit_threshold =
      std::min(config_.base_exit_threshold + decision.levels.protected_threshold_delta,
               decision.entry_threshold - 0.10);
  decision.trace_json = controller_trace_json(input, decision);
  return decision;
}

ControllerState ProtectedRegionController::state() const { return state_; }

ControllerLevelStack ProtectedRegionController::levels() const { return current_levels(); }

std::string controller_state_name(ControllerState state) {
  switch (state) {
  case ControllerState::stable:
    return "STABLE";
  case ControllerState::rate_pressure:
    return "RATE_PRESSURE";
  case ControllerState::congested:
    return "CONGESTED";
  case ControllerState::recovery:
    return "RECOVERY";
  case ControllerState::unusable:
    return "UNUSABLE";
  }
  return "UNKNOWN";
}

std::string controller_action_name(ControllerAction action) {
  switch (action) {
  case ControllerAction::none:
    return "NONE";
  case ControllerAction::reduce_automatic_emphasis:
    return "REDUCE_AUTOMATIC_EMPHASIS";
  case ControllerAction::tighten_protected_threshold:
    return "TIGHTEN_PROTECTED_THRESHOLD";
  case ControllerAction::reduce_payload_and_vbv:
    return "REDUCE_PAYLOAD_AND_VBV";
  case ControllerAction::reduce_presentation_profile:
    return "REDUCE_PRESENTATION_PROFILE";
  case ControllerAction::request_recovery_idr:
    return "REQUEST_RECOVERY_IDR";
  case ControllerAction::restore_presentation_profile:
    return "RESTORE_PRESENTATION_PROFILE";
  case ControllerAction::restore_payload_and_vbv:
    return "RESTORE_PAYLOAD_AND_VBV";
  case ControllerAction::restore_protected_threshold:
    return "RESTORE_PROTECTED_THRESHOLD";
  case ControllerAction::restore_automatic_emphasis:
    return "RESTORE_AUTOMATIC_EMPHASIS";
  case ControllerAction::stop_unusable_link:
    return "STOP_UNUSABLE_LINK";
  }
  return "UNKNOWN";
}

std::string controller_trace_json(const ControllerTickInput &input,
                                  const ControllerDecision &decision) {
  std::string output;
  output.reserve(2'048U);
  output.append("{\"action\":");
  append_json_string(output, controller_action_name(decision.action));
  output.append(",\"arrivalSequence\":");
  append_unsigned(output, decision.arrival_sequence);
  output.append(",\"consumedFeedback\":[");
  for (std::size_t index = 0; index < decision.consumed_feedback.size(); ++index) {
    if (index != 0U) {
      output.push_back(',');
    }
    const auto &feedback = decision.consumed_feedback[index];
    output.append("{\"arrivalSequence\":");
    append_unsigned(output, feedback.arrival_sequence);
    output.append(",\"lossFraction\":");
    append_optional_double(output, feedback.loss_fraction);
    output.append(",\"receiverDecodedFrames\":");
    append_optional_unsigned(output, feedback.receiver_decoded_frames);
    output.append(",\"receiverDroppedFrames\":");
    append_optional_unsigned(output, feedback.receiver_dropped_frames);
    output.append(",\"rembBitsPerSecond\":");
    append_optional_double(output, feedback.remb_bits_per_second);
    output.append(",\"rembPayloadTypeValid\":");
    append_bool(output, feedback.remb_payload_type_valid);
    output.append(",\"rembRtcpSourceValid\":");
    append_bool(output, feedback.remb_rtcp_source_valid);
    output.append(",\"roundTripTimeMilliseconds\":");
    append_optional_double(output, feedback.round_trip_time_milliseconds);
    output.append(",\"senderArrivalMilliseconds\":");
    append_unsigned(output, feedback.sender_arrival_milliseconds);
    output.append(",\"sourceTimeMilliseconds\":");
    append_optional_unsigned(output, feedback.source_time_milliseconds);
    output.push_back('}');
  }
  output.append("],\"dependencyEpoch\":");
  append_unsigned(output, decision.dependency_epoch);
  output.append(",\"estimatorInputs\":{\"deliveredFramesPerSecond\":");
  append_estimate(output, decision.estimators.delivered_frames_per_second);
  output.append(",\"elementaryStreamBps\":");
  append_estimate(output, decision.estimators.elementary_stream_bps);
  output.append(",\"lossFraction\":");
  append_estimate(output, decision.estimators.loss_fraction);
  output.append(",\"oldestMediaAgeMilliseconds\":");
  append_estimate(output, decision.estimators.oldest_media_age_milliseconds);
  output.append(",\"retransmissionBps\":");
  append_estimate(output, decision.estimators.retransmission_bps);
  output.append(",\"roundTripTimeMilliseconds\":");
  append_estimate(output, decision.estimators.round_trip_time_milliseconds);
  output.append(",\"wireEgressBps\":");
  append_estimate(output, decision.estimators.wire_egress_bps);
  output.append("},\"priorState\":");
  append_json_string(output, controller_state_name(decision.prior_state));
  output.append(",\"rawInput\":{\"controllerConfig\":{\"baseEntryThreshold\":");
  append_double(output, decision.controller_config.base_entry_threshold);
  output.append(",\"baseExitThreshold\":");
  append_double(output, decision.controller_config.base_exit_threshold);
  output.append(",\"basePayloadTargetBps\":");
  append_unsigned(output, decision.controller_config.base_payload_target_bps);
  output.append("},\"counters\":{\"deliveredFrames\":");
  append_unsigned(output, input.counters.delivered_frames);
  output.append(",\"elementaryStreamBytes\":");
  append_unsigned(output, input.counters.elementary_stream_bytes);
  output.append(",\"retransmissionBytes\":");
  append_unsigned(output, input.counters.retransmission_bytes);
  output.append(",\"wireEgressBytes\":");
  append_unsigned(output, input.counters.wire_egress_bytes);
  output.append("},\"dropFraction\":");
  append_double(output, input.drop_fraction);
  output.append(",\"encodeLatencyMilliseconds\":");
  append_double(output, input.encode_latency_milliseconds);
  output.append(",\"mapLevelHistogram\":[");
  for (std::size_t index = 0; index < input.map_level_histogram.size(); ++index) {
    if (index != 0U) {
      output.push_back(',');
    }
    append_unsigned(output, input.map_level_histogram[index]);
  }
  output.append("],\"oldestMediaAgeMilliseconds\":");
  append_double(output, input.oldest_media_age_milliseconds);
  output.append(",\"pacerQueueBytes\":");
  append_unsigned(output, input.pacer_queue_bytes);
  output.append(",\"pacerQueuePackets\":");
  append_unsigned(output, input.pacer_queue_packets);
  output.append(",\"pinnedRegionViolation\":");
  append_bool(output, input.pinned_region_violation);
  output.append(",\"protectedFraction\":");
  append_double(output, input.protected_fraction);
  output.append(",\"userWireCapBps\":");
  append_unsigned(output, input.user_wire_cap_bps);
  output.append("},\"reason\":");
  append_json_string(output, decision.reason);
  output.append(",\"resultingState\":");
  append_json_string(output, controller_state_name(decision.resulting_state));
  output.append(",\"roundingResults\":{\"controlReserveBps\":");
  append_double(output, decision.control_reserve_bps);
  output.append(",\"counterReset\":");
  append_bool(output, decision.counter_reset);
  output.append(",\"effectiveWireCapBps\":");
  append_double(output, decision.effective_wire_cap_bps);
  output.append(",\"entryThreshold\":");
  append_double(output, decision.entry_threshold);
  output.append(",\"exitThreshold\":");
  append_double(output, decision.exit_threshold);
  output.append(",\"feedbackLossAvailable\":");
  append_bool(output, decision.feedback_loss_available);
  output.append(",\"feedbackRembAvailable\":");
  append_bool(output, decision.feedback_remb_available);
  output.append(",\"feedbackRttAvailable\":");
  append_bool(output, decision.feedback_rtt_available);
  output.append(",\"payloadBudgetBps\":");
  append_double(output, decision.payload_budget_bps);
  output.append(",\"pinnedRegionViolationVisible\":");
  append_bool(output, decision.pinned_region_violation_visible);
  output.append(",\"requestIdrWithParameterSets\":");
  append_bool(output, decision.request_idr_with_parameter_sets);
  output.append(",\"startsDependencyEpoch\":");
  append_bool(output, decision.starts_dependency_epoch);
  output.append(",\"startsGeometryEpoch\":");
  append_bool(output, decision.starts_geometry_epoch);
  output.append("},\"selectedLevelStack\":{\"automaticEmphasisCap\":");
  append_unsigned(output, decision.levels.automatic_emphasis_cap);
  output.append(",\"framesPerSecond\":");
  append_unsigned(output, decision.levels.frames_per_second);
  output.append(",\"height\":");
  append_unsigned(output, decision.levels.height);
  output.append(",\"payloadAndVbvStep\":");
  append_size(output, decision.levels.payload_and_vbv_step);
  output.append(",\"payloadTargetBps\":");
  append_unsigned(output, decision.levels.payload_target_bps);
  output.append(",\"presentationProfile\":");
  append_json_string(output, decision.levels.presentation_profile);
  output.append(",\"protectedThresholdDelta\":");
  append_double(output, decision.levels.protected_threshold_delta);
  output.append(",\"width\":");
  append_unsigned(output, decision.levels.width);
  output.append("},\"senderArrivalMilliseconds\":");
  append_unsigned(output, decision.sender_arrival_milliseconds);
  output.append(",\"sourceTimeMilliseconds\":");
  append_optional_unsigned(output, decision.source_time_milliseconds);
  output.push_back('}');
  return output;
}

} // namespace glyphrelay
