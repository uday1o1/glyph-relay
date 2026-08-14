#include "glyphrelay/annex_b.hpp"
#include "glyphrelay/h264_sps.hpp"
#include "glyphrelay/share_command.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

class ScriptedTransport final : public glyphrelay::ShareTransport {
public:
  enum class Mode { normal, fail_start, recover_after_first, disconnect_after_first };

  explicit ScriptedTransport(Mode mode = Mode::normal) : mode_(mode) {}

  bool start() override {
    if (mode_ == Mode::fail_start) {
      diagnostics_.reason = "SCRIPTED_SIGNALING_UNAVAILABLE";
      return false;
    }
    diagnostics_.started = true;
    diagnostics_.reason = "SCRIPTED_SIGNALING_STARTED";
    events_.push_back(
        {glyphrelay::ShareTransportEventKind::session_created, "abcdefghijklmnopqrstuv"});
    events_.push_back({glyphrelay::ShareTransportEventKind::join_created,
                       "https://share.example.test/#join=abcdefghijklmnopqrstuv.token"});
    events_.push_back({glyphrelay::ShareTransportEventKind::peer_ready, "receiver_ready"});
    return true;
  }

  std::optional<glyphrelay::ShareTransportEvent> poll_event(std::chrono::milliseconds) override {
    if (events_.empty()) {
      return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    if (event.kind == glyphrelay::ShareTransportEventKind::peer_ready) {
      diagnostics_.peer_ready = true;
    }
    return event;
  }

  bool send_access_unit(const glyphrelay::RecordedAccessUnit &access_unit) override {
    if (!diagnostics_.peer_ready || diagnostics_.stopped || !access_unit.bytes ||
        access_unit.bytes->empty()) {
      diagnostics_.reason = "SCRIPTED_SEND_REJECTED";
      return false;
    }
    if (sent_.empty() && (!access_unit.keyframe || !access_unit.parameter_sets_present)) {
      diagnostics_.reason = "SCRIPTED_FIRST_ACCESS_UNIT_NOT_RECOVERY";
      return false;
    }
    sent_.push_back(access_unit);
    ++diagnostics_.access_units_sent;
    diagnostics_.bytes_sent += access_unit.bytes->size();
    diagnostics_.reason = "SCRIPTED_ACCESS_UNIT_SENT";
    if (sent_.size() == 1U && mode_ == Mode::recover_after_first) {
      events_.push_back({glyphrelay::ShareTransportEventKind::recovery_requested, "SCRIPTED_PLI"});
    } else if (sent_.size() == 1U && mode_ == Mode::disconnect_after_first) {
      events_.push_back(
          {glyphrelay::ShareTransportEventKind::peer_disconnected, "SCRIPTED_DISCONNECT"});
    }
    return true;
  }

  void stop(std::string_view reason) override {
    diagnostics_.stopped = true;
    diagnostics_.peer_ready = false;
    diagnostics_.reason = std::string(reason);
    events_.clear();
  }

  glyphrelay::ShareTransportDiagnostics diagnostics() const override { return diagnostics_; }

  const std::vector<glyphrelay::RecordedAccessUnit> &sent() const { return sent_; }

private:
  std::deque<glyphrelay::ShareTransportEvent> events_;
  std::vector<glyphrelay::RecordedAccessUnit> sent_;
  glyphrelay::ShareTransportDiagnostics diagnostics_;
  Mode mode_;
};

class SyntheticShareSource final : public glyphrelay::RecordFrameSource {
public:
  SyntheticShareSource(std::optional<std::filesystem::path> output,
                       const ScriptedTransport &transport)
      : output_(std::move(output)), transport_(transport) {}

  glyphrelay::RecordSourceResult select_window() override {
    ++selection_calls_;
    selected_ = true;
    return {true, false, "synthetic_window_selected"};
  }

  glyphrelay::RecordSourceResult start_capture() override {
    require(selected_, "share capture must follow portal selection");
    if (output_) {
      require(!transport_.diagnostics().started,
              "share --record capture must start before remote session creation");
      const auto prepared = glyphrelay::inspect_recording(*output_);
      require(prepared.passed &&
                  prepared.state == glyphrelay::RecordingInspectionState::prepared_incomplete,
              "share --record durable barrier must precede capture admission");
    } else {
      require(transport_.diagnostics().started && transport_.diagnostics().peer_ready,
              "share capture without recording must wait for a compatible receiver");
    }
    ++start_calls_;
    started_ = true;
    return {true, false, "synthetic_capture_started"};
  }

  std::optional<glyphrelay::CapturedFrameLease> take_latest() override {
    if (!started_ || frame_index_ == frame_count_) {
      return std::nullopt;
    }
    constexpr std::size_t width = 320U;
    constexpr std::size_t height = 180U;
    pixels_.resize(width * height * 4U);
    for (std::size_t y = 0U; y < height; ++y) {
      for (std::size_t x = 0U; x < width; ++x) {
        const auto offset = (y * width + x) * 4U;
        pixels_[offset] = static_cast<std::uint8_t>((x + frame_index_ * 7U) & 0xFFU);
        pixels_[offset + 1U] = static_cast<std::uint8_t>((y + frame_index_ * 3U) & 0xFFU);
        pixels_[offset + 2U] = static_cast<std::uint8_t>((x + y) & 0xFFU);
        pixels_[offset + 3U] = 255U;
      }
    }
    glyphrelay::SharedMemoryBufferView view = {
        .bytes = pixels_,
        .width = width,
        .height = height,
        .pitch = width * 4U,
        .crop = {0U, 0U, width, height},
        .pixel_order = glyphrelay::PackedPixelOrder::bgra,
        .orientation = glyphrelay::CaptureOrientation::upright,
        .cursor_mode = glyphrelay::CursorMode::hidden,
        .cursor = std::nullopt,
        .damage = {},
    };
    const auto timestamp =
        1'000'000'000ULL + static_cast<std::uint64_t>(frame_index_) * 33'333'334ULL;
    const auto ingested = pool_.ingest(view, timestamp, []() {});
    require(ingested.accepted && ingested.requeued,
            "synthetic share frame must enter the bounded capture pool");
    ++frame_index_;
    return pool_.take_latest();
  }

  std::optional<glyphrelay::CaptureState> poll_terminal() override {
    if (started_ && frame_index_ == frame_count_) {
      return glyphrelay::CaptureState::closed;
    }
    return std::nullopt;
  }

  glyphrelay::CapturePoolDiagnostics diagnostics() const override { return pool_.diagnostics(); }

  void stop(glyphrelay::CaptureState terminal_state) override {
    if (pool_.diagnostics().admission_open) {
      pool_.stop(terminal_state);
    }
    started_ = false;
  }

  std::size_t selection_calls() const { return selection_calls_; }
  std::size_t start_calls() const { return start_calls_; }

private:
  static constexpr std::size_t frame_count_ = 5U;
  std::optional<std::filesystem::path> output_;
  const ScriptedTransport &transport_;
  glyphrelay::SharedMemoryCapturePool pool_{3U};
  std::vector<std::uint8_t> pixels_;
  std::size_t frame_index_ = 0U;
  std::size_t selection_calls_ = 0U;
  std::size_t start_calls_ = 0U;
  bool selected_ = false;
  bool started_ = false;
};

void remove_recording(const std::filesystem::path &output) {
  std::filesystem::remove(output);
  std::filesystem::remove(output.string() + ".json");
  std::filesystem::remove(output.string() + ".complete");
  std::filesystem::remove(output.string() + ".journal");
}

void test_unconfigured_fails_before_side_effects() {
  ScriptedTransport transport;
  SyntheticShareSource source(std::nullopt, transport);
  const auto result = glyphrelay::run_share_pipeline({}, source, transport);
  require(result.exit_code == 2 && result.reason == "signaling_origin_unconfigured" &&
              source.selection_calls() == 0U && source.start_calls() == 0U &&
              !transport.diagnostics().started,
          "unconfigured share must fail before portal, capture, or signaling side effects");

  ScriptedTransport ca_transport;
  SyntheticShareSource ca_source(std::nullopt, ca_transport);
  const auto ca_result = glyphrelay::run_share_pipeline(
      {.signaling_origin = "https://share.example.test",
       .signaling_ca_path = std::filesystem::path("/glyphrelay/absent-signaling-ca.pem")},
      ca_source, ca_transport);
  require(ca_result.exit_code == 2 && ca_result.reason == "signaling_ca_path_unavailable" &&
              ca_source.selection_calls() == 0U && ca_source.start_calls() == 0U &&
              !ca_transport.diagnostics().started,
          "unavailable signaling CA must fail before portal, capture, or signaling side effects");
}

#if GLYPHRELAY_HAS_OPENH264 && GLYPHRELAY_HAS_DURABLE_RECORDING
void test_share_record_pipeline(const std::filesystem::path &output) {
  remove_recording(output);
  ScriptedTransport transport(ScriptedTransport::Mode::recover_after_first);
  SyntheticShareSource source(output, transport);
  std::vector<std::string> states;
  const auto result = glyphrelay::run_share_pipeline(
      {.signaling_origin = "https://share.example.test",
       .signaling_ca_path = std::nullopt,
       .bitrate_profile = "2m",
       .recording_path = output,
       .json = false},
      source, transport, {},
      [&states](std::string_view state, std::string_view) { states.emplace_back(state); });
  if (!(result.exit_code == 0 && result.reason == "capture_closed" &&
        result.join_url.starts_with("https://share.example.test/#join=") &&
        result.captured_frames == 5U && result.encoded_access_units == 5U &&
        result.transported_access_units == 5U && result.recording_error.empty() &&
        result.recorder.completed && result.recorder.committed_access_units == 5U)) {
    std::cerr << "share result: exit=" << result.exit_code << " reason=" << result.reason
              << " captured=" << result.captured_frames
              << " encoded=" << result.encoded_access_units
              << " transported=" << result.transported_access_units
              << " recording_error=" << result.recording_error
              << " recorder_reason=" << result.recorder.reason
              << " committed=" << result.recorder.committed_access_units
              << " transport_reason=" << result.transport.reason << '\n';
  }
  require(result.exit_code == 0 && result.reason == "capture_closed" &&
              result.join_url.starts_with("https://share.example.test/#join=") &&
              result.captured_frames == 5U && result.encoded_access_units == 5U &&
              result.transported_access_units == 5U && result.recording_error.empty() &&
              result.recorder.completed && result.recorder.committed_access_units == 5U,
          "share --record must complete capture, encode, fanout, transport, and recording");
  require(result.transport_queue.maximum_access_units == 3U &&
              result.transport_queue.maximum_bytes == 8U * 1024U * 1024U &&
              result.transport_queue.maximum_age_ns == 100'000'000ULL &&
              result.transport_queue.access_units == 0U && result.capture.leased == 0U,
          "share must expose and drain every bounded capture and transport queue");
  require(!transport.sent().empty() && transport.sent().front().keyframe &&
              transport.sent().front().parameter_sets_present &&
              transport.sent().front().dependency_epoch == 2U,
          "receiver admission during recording must start a fresh recovery dependency epoch");
  require(transport.sent().size() == 5U && transport.sent()[1].keyframe &&
              transport.sent()[1].parameter_sets_present &&
              transport.sent()[1].dependency_epoch == 3U,
          "transport recovery must advance the dependency epoch and force a complete IDR");
  const auto parsed = glyphrelay::parse_annex_b_access_unit(*transport.sent().front().bytes);
  require(parsed.passed, "the first live access unit must be strict Annex B");
  const auto sps_nal =
      std::find_if(parsed.access_unit.nal_units.begin(), parsed.access_unit.nal_units.end(),
                   [](const glyphrelay::AnnexBNalUnit &nal) { return nal.unit_type == 7U; });
  require(sps_nal != parsed.access_unit.nal_units.end(),
          "the first live access unit must contain an SPS");
  const auto sps = glyphrelay::parse_h264_sps(parsed.access_unit.payload(*sps_nal));
  require(sps.passed && sps.info.visible_width == 1280U && sps.info.visible_height == 720U &&
              sps.info.profile_level.level_idc == 31U,
          "live OpenH264 output must declare the negotiated 720p Level 3.1 contract");
  require(std::find(states.begin(), states.end(), "recording_ready") != states.end() &&
              std::find(states.begin(), states.end(), "join_open") != states.end() &&
              std::find(states.begin(), states.end(), "connected") != states.end(),
          "share must expose recording, join-link, and connection states");
  const auto inspected = glyphrelay::inspect_recording(output);
  require(inspected.passed && inspected.state == glyphrelay::RecordingInspectionState::complete &&
              inspected.committed_access_units == 5U,
          "share --record output must pass the independent recording inspector");
}

void test_signaling_failure_preserves_recording(const std::filesystem::path &output) {
  remove_recording(output);
  ScriptedTransport transport(ScriptedTransport::Mode::fail_start);
  SyntheticShareSource source(output, transport);
  std::vector<std::string> states;
  const auto result = glyphrelay::run_share_pipeline(
      {.signaling_origin = "https://share.example.test",
       .signaling_ca_path = std::nullopt,
       .bitrate_profile = "1m",
       .recording_path = output,
       .json = false},
      source, transport, {},
      [&states](std::string_view state, std::string_view) { states.emplace_back(state); });
  require(result.exit_code == 0 && result.reason == "capture_closed" && result.join_url.empty() &&
              result.transported_access_units == 0U && result.recorder.completed &&
              result.recorder.committed_access_units == 5U &&
              std::find(states.begin(), states.end(), "signaling_failed") != states.end(),
          "signaling failure must create no link and preserve an explicit local recording");
}

void test_peer_disconnect_preserves_recording(const std::filesystem::path &output) {
  remove_recording(output);
  ScriptedTransport transport(ScriptedTransport::Mode::disconnect_after_first);
  SyntheticShareSource source(output, transport);
  std::vector<std::string> states;
  const auto result = glyphrelay::run_share_pipeline(
      {.signaling_origin = "https://share.example.test",
       .signaling_ca_path = std::nullopt,
       .bitrate_profile = "1m",
       .recording_path = output,
       .json = false},
      source, transport, {},
      [&states](std::string_view state, std::string_view) { states.emplace_back(state); });
  require(result.exit_code == 0 && result.reason == "capture_closed" &&
              result.transported_access_units == 1U && result.recorder.completed &&
              result.recorder.committed_access_units == 5U &&
              std::find(states.begin(), states.end(), "revoked") != states.end() &&
              result.transport_queue.access_units == 0U,
          "peer disconnect must revoke and drain transport while explicit recording continues");
}

void test_live_share_defers_capture() {
  ScriptedTransport transport;
  SyntheticShareSource source(std::nullopt, transport);
  const auto result =
      glyphrelay::run_share_pipeline({.signaling_origin = "https://share.example.test",
                                      .signaling_ca_path = std::nullopt,
                                      .bitrate_profile = "1m",
                                      .recording_path = std::nullopt,
                                      .json = false},
                                     source, transport);
  require(result.exit_code == 0 && result.reason == "capture_closed" &&
              source.start_calls() == 1U && result.encoded_access_units == 5U &&
              result.transported_access_units == 5U && result.recorder.accepted_access_units == 0U,
          "live-only share must defer capture and encode until receiver readiness");
}
#endif

} // namespace

int main(int argc, char **argv) {
  test_unconfigured_fails_before_side_effects();
#if GLYPHRELAY_HAS_OPENH264 && GLYPHRELAY_HAS_DURABLE_RECORDING
  require(argc == 2, "Linux share command test requires one recording output path");
  test_share_record_pipeline(argv[1]);
  test_signaling_failure_preserves_recording(std::string(argv[1]) + ".signaling-failure.h264");
  test_peer_disconnect_preserves_recording(std::string(argv[1]) + ".disconnect.h264");
  test_live_share_defers_capture();
#else
  require(argc == 1, "portable share command test accepts no output path");
#endif
  return 0;
}
