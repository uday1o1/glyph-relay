#include "glyphrelay/saliency.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kWidth = 640U;
constexpr std::size_t kHeight = 360U;

void usage() { std::cout << "usage: glyphrelay_saliency_preview --output FILE.ppm\n"; }

std::vector<std::uint8_t> source_fixture() {
  std::vector<std::uint8_t> luma(kWidth * kHeight, 28U);
  for (std::size_t y = 48U; y < 312U; ++y) {
    for (std::size_t x = 48U; x < 592U; ++x) {
      luma[y * kWidth + x] = 52U;
    }
  }
  for (std::size_t row = 0U; row < 8U; ++row) {
    const auto top = 72U + row * 28U;
    for (std::size_t glyph = 0U; glyph < 20U; ++glyph) {
      const auto left = 72U + glyph * 24U;
      const auto glyph_width = 4U + glyph % 4U;
      for (std::size_t y = top; y < top + 12U; ++y) {
        for (std::size_t x = left; x < left + glyph_width; ++x) {
          luma[y * kWidth + x] = (x + y + glyph) % 3U == 0U ? 220U : 236U;
        }
      }
    }
  }
  for (std::size_t y = 96U; y < 272U; ++y) {
    for (std::size_t x = 472U; x < 560U; ++x) {
      luma[y * kWidth + x] = ((x / 3U + y / 3U) & 1U) == 0U ? 16U : 240U;
    }
  }
  return luma;
}

bool write_ppm(const std::filesystem::path &path, std::span<const std::uint8_t> rgb) {
  errno = 0;
  auto *file = std::fopen(path.string().c_str(), "wbx");
  if (file == nullptr) {
    std::cerr << "preview output open failed: " << std::strerror(errno) << '\n';
    return false;
  }
  const auto header = "P6\n" + std::to_string(kWidth) + " " + std::to_string(kHeight) + "\n255\n";
  bool passed = std::fwrite(header.data(), 1U, header.size(), file) == header.size();
  passed = std::fwrite(rgb.data(), 1U, rgb.size(), file) == rgb.size() && passed;
  passed = std::fflush(file) == 0 && passed;
  passed = std::fclose(file) == 0 && passed;
  if (!passed) {
    std::cerr << "preview output write failed\n";
  }
  return passed;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    usage();
    return 0;
  }
  if (argc != 3 || std::string(argv[1]) != "--output") {
    usage();
    return 2;
  }
  const std::filesystem::path output_path(argv[2]);
  if (output_path.extension() != ".ppm" || !output_path.has_filename() ||
      !std::filesystem::is_directory(output_path.parent_path())) {
    std::cerr << "preview output must be a .ppm file in an existing directory\n";
    return 2;
  }

  const auto luma = source_fixture();
  glyphrelay::SaliencyReference saliency;
  const glyphrelay::LumaPlaneView frame{
      .codes = luma,
      .width = kWidth,
      .height = kHeight,
      .pitch = kWidth,
      .range = glyphrelay::ColorRange::full,
      .frame_id = 1U,
      .geometry_epoch = 1U,
      .monotonic_timestamp_ns = 1U,
  };
  glyphrelay::SaliencyProcessOptions options;
  options.generate_preview = true;
  const auto result = saliency.process(frame, options);
  if (!result.passed || result.output.protected_fraction <= 0.0 ||
      result.output.preview.rgba.size() != luma.size() * 4U) {
    std::cerr << "saliency preview generation failed: " << result.reason << '\n';
    return 8;
  }

  std::vector<std::uint8_t> rgb(luma.size() * 3U);
  for (std::size_t index = 0U; index < luma.size(); ++index) {
    const auto alpha = result.output.preview.rgba[index * 4U + 3U];
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
      const auto overlay = result.output.preview.rgba[index * 4U + channel];
      const auto blended = static_cast<unsigned int>(overlay) * alpha +
                           static_cast<unsigned int>(luma[index]) * (255U - alpha) + 127U;
      rgb[index * 3U + channel] = static_cast<std::uint8_t>(blended / 255U);
    }
  }
  if (!write_ppm(output_path, rgb)) {
    return errno == EEXIST ? 2 : 8;
  }

  std::cout << std::setprecision(17)
            << "{\"status\":\"PASSED\",\"protectedFraction\":" << result.output.protected_fraction
            << ",\"macroblockWidth\":" << result.output.macroblock_width
            << ",\"macroblockHeight\":" << result.output.macroblock_height
            << ",\"levelHistogram\":[";
  for (std::size_t index = 0U; index < result.output.level_histogram.size(); ++index) {
    if (index != 0U) {
      std::cout << ',';
    }
    std::cout << result.output.level_histogram[index];
  }
  std::cout << "]}\n";
  return 0;
}
