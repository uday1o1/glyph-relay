#include "glyphrelay/recording.hpp"

#include <utility>

namespace glyphrelay {

struct DurableRecorder::Implementation {
  explicit Implementation(RecorderConfig input) : config(std::move(input)) {}
  RecorderConfig config;
};

DurableRecorder::DurableRecorder(RecorderConfig config)
    : implementation_(std::make_unique<Implementation>(std::move(config))) {}

DurableRecorder::~DurableRecorder() = default;
DurableRecorder::DurableRecorder(DurableRecorder &&) noexcept = default;
DurableRecorder &DurableRecorder::operator=(DurableRecorder &&) noexcept = default;

bool DurableRecorder::ready() const { return false; }

std::string DurableRecorder::initialization_reason() const {
  return "durable_recording_requires_linux";
}

RecorderEnqueueResult DurableRecorder::enqueue(RecordedAccessUnit) {
  return {false, true, "durable_recording_requires_linux"};
}

RecorderFinalizeResult DurableRecorder::finalize() {
  return {false, "durable_recording_requires_linux"};
}

RecorderDiagnostics DurableRecorder::diagnostics() const {
  return {.ready = false, .failed = true, .reason = "durable_recording_requires_linux"};
}

RecordingInspection inspect_recording(const std::filesystem::path &) {
  return {.passed = false,
          .state = RecordingInspectionState::absent,
          .reason = "durable_recording_requires_linux"};
}

bool durable_recording_available() { return false; }

} // namespace glyphrelay
