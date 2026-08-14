#include "glyphrelay/i420.hpp"
#include "glyphrelay/openh264_encoder.hpp"
#include "glyphrelay/synthetic_source.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "the integration test requires one output path");
  const std::filesystem::path output_path(argv[1]);

  glyphrelay::OpenH264Encoder encoder({
      .width = glyphrelay::M0SourceGeometry::visible_width,
      .height = glyphrelay::M0SourceGeometry::visible_height,
      .frames_per_second = glyphrelay::M0SourceGeometry::frames_per_second,
      .target_bitrate_bps = 4'000'000U,
      .maximum_bitrate_bps = 4'000'000U,
      .gop_frames = 60U,
      .level_idc = 40U,
  });
  require(encoder.available(), encoder.initialization_reason().c_str());

  glyphrelay::M0SyntheticSource source;
  std::vector<std::uint8_t> stream;
  std::size_t emitted_frames = 0U;
  for (std::size_t frame_index = 0U; frame_index < 30U && emitted_frames < 3U; ++frame_index) {
    const auto nv12 = source.generate(frame_index);
    const auto i420 = glyphrelay::nv12_to_i420(
        nv12, glyphrelay::M0SourceGeometry::coded_width, glyphrelay::M0SourceGeometry::coded_height,
        glyphrelay::M0SourceGeometry::coded_width, glyphrelay::M0SourceGeometry::coded_width,
        glyphrelay::M0SourceGeometry::visible_width, glyphrelay::M0SourceGeometry::visible_height);
    const auto encoded = encoder.encode(i420, emitted_frames == 2U);
    require(encoded.passed, encoded.reason.c_str());
    require(encoded.frame_index == frame_index, "encoded frame identity must be monotonic");
    if (encoded.skipped) {
      continue;
    }
    if (emitted_frames == 0U || emitted_frames == 2U) {
      require(encoded.keyframe, "startup and forced recovery frames must be IDRs");
      require(encoded.access_unit.starts_with_parameter_sets_and_idr(),
              "every IDR must be preceded by SPS and PPS");
    }
    stream.insert(stream.end(), encoded.access_unit.bytes.begin(), encoded.access_unit.bytes.end());
    ++emitted_frames;
  }
  require(emitted_frames == 3U, "the bounded fixture must emit three complete access units");
  require(!stream.empty(), "the system OpenH264 stream must contain encoded bytes");

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  require(output.good(), "the integration stream output must open");
  output.write(reinterpret_cast<const char *>(stream.data()),
               static_cast<std::streamsize>(stream.size()));
  output.close();
  require(output.good(), "the integration stream output must close cleanly");
  return 0;
}
