#pragma once

#include "glyphrelay/capture.hpp"
#include "glyphrelay/recording.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace glyphrelay {

struct RecordSourceResult {
  bool passed = false;
  bool cancelled = false;
  std::string reason;
};

class RecordFrameSource {
public:
  virtual ~RecordFrameSource() = default;
  virtual RecordSourceResult select_window() = 0;
  virtual RecordSourceResult start_capture() = 0;
  virtual std::optional<CapturedFrameLease> take_latest() = 0;
  virtual std::optional<CaptureState> poll_terminal() = 0;
  virtual CapturePoolDiagnostics diagnostics() const = 0;
  virtual void stop(CaptureState terminal_state) = 0;
};

std::unique_ptr<RecordFrameSource> make_interactive_frame_source();

struct RecordCommandOptions {
  std::filesystem::path output_path;
  std::string window_label;
  std::string bitrate_profile = "2m";
};

struct RecordRunResult {
  int exit_code = 8;
  std::string reason;
  std::uint64_t captured_frames = 0;
  std::uint64_t encoded_access_units = 0;
  std::uint64_t frame_rate_drops = 0;
  CapturePoolDiagnostics capture;
  RecorderDiagnostics recorder;
};

using RecordStopPredicate = std::function<bool()>;
using WindowSelectedCallback = std::function<void(std::string_view)>;

std::optional<unsigned int> record_bitrate_bps(std::string_view profile);
bool valid_window_label(std::string_view label);
RecordRunResult run_record_pipeline(const RecordCommandOptions &options, RecordFrameSource &source,
                                    RecordStopPredicate stop_requested = {},
                                    WindowSelectedCallback window_selected = {});
RecordRunResult run_interactive_record(const RecordCommandOptions &options,
                                       RecordStopPredicate stop_requested = {},
                                       WindowSelectedCallback window_selected = {});

} // namespace glyphrelay
