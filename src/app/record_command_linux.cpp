#include "glyphrelay/record_command.hpp"

#include "glyphrelay/linux_capture.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace glyphrelay {
namespace {

class LinuxRecordFrameSource final : public RecordFrameSource {
public:
  RecordSourceResult select_window() override {
    auto opened = portal_.open_window({}, 300'000U);
    if (!opened.passed || !opened.grant) {
      return {false, opened.cancelled, opened.reason};
    }
    grant_ = std::move(opened.grant);
    selected_ = true;
    return {true, false, "portal_window_selected"};
  }

  RecordSourceResult start_capture() override {
    if (!selected_ || !grant_) {
      return {false, false, "portal_window_not_selected"};
    }
    const auto started = pipewire_.start(std::move(*grant_), pool_, [this](std::string event) {
      std::scoped_lock lock(event_mutex_);
      last_event_ = std::move(event);
    });
    grant_.reset();
    if (!started.passed) {
      return {false, started.cancelled, started.reason};
    }
    started_ = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pipewire_.running() && std::chrono::steady_clock::now() < deadline) {
      if (const auto terminal = portal_.poll_terminal()) {
        return {false, *terminal == CaptureState::cancelled,
                std::string("capture_") + std::string(capture_state_name(*terminal))};
      }
      if (pool_.diagnostics().terminal_state == CaptureState::disconnected) {
        return {false, false, last_event()};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!pipewire_.running()) {
      return {false, false, "pipewire_stream_start_timeout"};
    }
    return {true, false, "pipewire_capture_started"};
  }

  std::optional<CapturedFrameLease> take_latest() override { return pool_.take_latest(); }

  std::optional<CaptureState> poll_terminal() override {
    if (const auto portal_terminal = portal_.poll_terminal()) {
      return portal_terminal;
    }
    const auto capture = pool_.diagnostics();
    if (!capture.admission_open) {
      return capture.terminal_state;
    }
    if (started_ && !pipewire_.running()) {
      return CaptureState::disconnected;
    }
    return std::nullopt;
  }

  CapturePoolDiagnostics diagnostics() const override { return pool_.diagnostics(); }

  void stop(CaptureState terminal_state) override {
    if (started_ || pipewire_.running()) {
      static_cast<void>(pipewire_.stop(terminal_state));
      started_ = false;
    } else if (pool_.diagnostics().admission_open) {
      pool_.stop(terminal_state);
    }
    static_cast<void>(portal_.close());
    grant_.reset();
    selected_ = false;
  }

private:
  std::string last_event() const {
    std::scoped_lock lock(event_mutex_);
    return last_event_.empty() ? "pipewire_stream_disconnected" : last_event_;
  }

  LinuxPortalClient portal_;
  SharedMemoryCapturePool pool_{3U};
  LinuxPipeWireCapture pipewire_;
  std::optional<PortalWindowGrant> grant_;
  mutable std::mutex event_mutex_;
  std::string last_event_;
  bool selected_ = false;
  bool started_ = false;
};

} // namespace

RecordRunResult run_interactive_record(const RecordCommandOptions &options,
                                       RecordStopPredicate stop_requested,
                                       WindowSelectedCallback window_selected) {
  if (!linux_capture_backend_available() || !durable_recording_available()) {
    return {.exit_code = 3,
            .reason = "recording_backend_unavailable",
            .captured_frames = 0U,
            .encoded_access_units = 0U,
            .frame_rate_drops = 0U,
            .capture = {},
            .recorder = {}};
  }
  LinuxRecordFrameSource source;
  return run_record_pipeline(options, source, std::move(stop_requested),
                             std::move(window_selected));
}

} // namespace glyphrelay
