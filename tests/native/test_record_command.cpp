#include "glyphrelay/nv12_scaler.hpp"
#include "glyphrelay/record_command.hpp"
#include "glyphrelay/recording.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

class SyntheticRecordSource final : public glyphrelay::RecordFrameSource {
public:
  explicit SyntheticRecordSource(std::filesystem::path output) : output_(std::move(output)) {}

  glyphrelay::RecordSourceResult select_window() override {
    selected_ = true;
    return {true, false, "synthetic_window_selected"};
  }

  glyphrelay::RecordSourceResult start_capture() override {
    if (!selected_) {
      return {false, false, "synthetic_window_not_selected"};
    }
    const auto prepared = glyphrelay::inspect_recording(output_);
    require(prepared.passed &&
                prepared.state == glyphrelay::RecordingInspectionState::prepared_incomplete,
            "durable prepared barrier must precede capture start");
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
        pixels_[offset] = static_cast<std::uint8_t>((x + frame_index_ * 5U) & 0xFFU);
        pixels_[offset + 1U] = static_cast<std::uint8_t>((y * 2U) & 0xFFU);
        pixels_[offset + 2U] = static_cast<std::uint8_t>((x + y) & 0xFFU);
        pixels_[offset + 3U] = 255U;
      }
    }
    const auto visible_width = frame_index_ < 4U ? width : width - 2U;
    glyphrelay::SharedMemoryBufferView view = {
        .bytes = pixels_,
        .width = width,
        .height = height,
        .pitch = width * 4U,
        .crop = {0U, 0U, visible_width, height},
        .pixel_order = glyphrelay::PackedPixelOrder::bgra,
        .orientation = glyphrelay::CaptureOrientation::upright,
        .cursor_mode = glyphrelay::CursorMode::hidden,
        .cursor = std::nullopt,
        .damage = {},
    };
    const auto relative_timestamp =
        frame_index_ == 1U
            ? 10'000'000ULL
            : static_cast<std::uint64_t>(frame_index_ == 0U ? 0U : frame_index_ - 1U) *
                  33'333'334ULL;
    const auto timestamp = 1'000'000'000ULL + relative_timestamp;
    const auto ingested = pool_.ingest(view, timestamp, []() {});
    require(ingested.accepted && ingested.requeued,
            "synthetic record frame must enter the bounded capture pool");
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

private:
  static constexpr std::size_t frame_count_ = 7U;
  std::filesystem::path output_;
  glyphrelay::SharedMemoryCapturePool pool_{3U};
  std::vector<std::uint8_t> pixels_;
  std::size_t frame_index_ = 0U;
  bool selected_ = false;
  bool started_ = false;
};

void test_nv12_scaler() {
  glyphrelay::Nv12Image source = {
      .visible_width = 2U,
      .visible_height = 2U,
      .coded_width = 2U,
      .coded_height = 2U,
      .pitch = 2U,
      .chroma_offset = 4U,
      .bytes = {16U, 32U, 48U, 64U, 100U, 150U},
  };
  const auto scaled = glyphrelay::scale_nv12_letterbox(source, 4U, 4U);
  require(scaled.passed && scaled.transform.scaled_width == 4U &&
              scaled.transform.scaled_height == 4U && scaled.transform.offset_x == 0U &&
              scaled.transform.offset_y == 0U,
          "square NV12 scaling must fill a square output without padding");
  constexpr std::array<std::uint8_t, 16U> expected_luma = {
      16U, 16U, 32U, 32U, 16U, 16U, 32U, 32U, 48U, 48U, 64U, 64U, 48U, 48U, 64U, 64U,
  };
  require(std::equal(expected_luma.begin(), expected_luma.end(), scaled.image.bytes.begin()),
          "nearest-neighbor luma scaling must use pixel-center coordinates");
  constexpr std::array<std::uint8_t, 8U> expected_chroma = {100U, 150U, 100U, 150U,
                                                            100U, 150U, 100U, 150U};
  require(
      std::equal(expected_chroma.begin(), expected_chroma.end(), scaled.image.bytes.begin() + 16),
      "NV12 scaling must preserve interleaved chroma order");
  require(!glyphrelay::scale_nv12_letterbox(source, 3U, 4U).passed,
          "NV12 scaling must reject odd encoder dimensions");
}

void test_record_pipeline(const std::filesystem::path &output) {
  std::filesystem::remove(output);
  std::filesystem::remove(output.string() + ".json");
  std::filesystem::remove(output.string() + ".complete");
  std::filesystem::remove(output.string() + ".journal");
  SyntheticRecordSource source(output);
  std::string displayed_label;
  const auto result = glyphrelay::run_record_pipeline(
      {.output_path = output, .window_label = "Local test label", .bitrate_profile = "2m"}, source,
      {}, [&displayed_label](std::string_view label) { displayed_label = label; });
  require(
      result.exit_code == 0 && result.reason == "capture_closed" && result.captured_frames == 7U &&
          result.encoded_access_units == 6U && result.frame_rate_drops == 1U &&
          result.capture.capacity == 3U && result.capture.leased == 0U &&
          result.recorder.completed && result.recorder.maximum_queue_bytes == 64U * 1024U * 1024U,
      "public record service must complete through bounded capture, encode, and recorder stages");
  require(displayed_label == "Local test label",
          "window label must be exposed only through the local selection callback");
  const auto inspected = glyphrelay::inspect_recording(output);
  require(inspected.passed && inspected.state == glyphrelay::RecordingInspectionState::complete &&
              inspected.committed_access_units == 6U,
          "record service output must pass the public recording inspector");
  const auto inspection_json = glyphrelay::recording_inspection_json(inspected);
  require(inspection_json.find("\"schemaVersion\":1") != std::string::npos &&
              inspection_json.find("\"state\":\"COMPLETE\"") != std::string::npos &&
              inspection_json.find("\"committedAccessUnits\":6") != std::string::npos,
          "public inspection JSON must expose its version, state, and durable count");
  std::ifstream sidecar(output.string() + ".json", std::ios::binary);
  const std::string sidecar_text((std::istreambuf_iterator<char>(sidecar)),
                                 std::istreambuf_iterator<char>());
  require(sidecar_text.find("Local test label") == std::string::npos,
          "ephemeral window label must never enter recording metadata");
  require(sidecar_text.find("\"dependencyEpoch\":2") != std::string::npos,
          "capture geometry change must advance dependency epoch and force recovery");
  require(glyphrelay::record_bitrate_bps("500k") == 500'000U &&
              glyphrelay::record_bitrate_bps("4m") == 4'000'000U &&
              !glyphrelay::record_bitrate_bps("unbounded") &&
              glyphrelay::valid_window_label("Local") &&
              !glyphrelay::valid_window_label("bad\nlabel"),
          "record CLI profiles and local labels must be strictly bounded");
}

} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "record command test requires one output path");
  require(glyphrelay::durable_recording_available(),
          "record command integration requires Linux durable recording");
  test_nv12_scaler();
  test_record_pipeline(argv[1]);
  return 0;
}
