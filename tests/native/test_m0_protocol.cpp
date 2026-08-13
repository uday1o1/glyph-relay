#include "glyphrelay/m0_protocol.hpp"
#include "glyphrelay/quality_metrics.hpp"
#include "glyphrelay/sha256.hpp"
#include "glyphrelay/synthetic_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::mt19937_64 random(
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto parent = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 32; ++attempt) {
      path_ = parent / ("glyphrelay-m0-protocol-test-" + std::to_string(random()));
      if (std::filesystem::create_directory(path_)) {
        return;
      }
    }
    throw std::runtime_error("cannot create a unique protocol test directory");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace

int main() {
  require(glyphrelay::sha256_hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 must match the empty-string known answer");
  require(glyphrelay::sha256_hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 must match the abc known answer");
  const std::string million_a(1'000'000U, 'a');
  require(glyphrelay::sha256_hex(million_a) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "SHA-256 must match the million-byte multi-block known answer");

  glyphrelay::M0SyntheticSource source;
  const auto frame_zero = source.generate(0);
  const auto frame_one = source.generate(1);
  require(frame_zero.size() == glyphrelay::M0SourceGeometry::frame_bytes,
          "the coded NV12 frame size must match its geometry");
  require(glyphrelay::sha256_hex(frame_zero) ==
              "04f446fe31ed05794f6c24607f470db6ccfc79324c18b92720a83494011e7c3d",
          "the first frozen frame must match its committed hash");
  require(frame_zero != frame_one, "the deterministic source must contain temporal motion");
  require(std::all_of(frame_zero.begin() +
                          static_cast<std::ptrdiff_t>(glyphrelay::M0SourceGeometry::luma_bytes),
                      frame_zero.end(), [](std::uint8_t value) { return value == 128U; }),
          "the synthetic NV12 source must use neutral chroma");

  const auto emphasis_map = glyphrelay::m0_fixed_emphasis_map();
  require(emphasis_map.size() == glyphrelay::M0SourceGeometry::map_entries,
          "the emphasis map must contain one byte per coded macroblock");
  require(std::count(emphasis_map.begin(), emphasis_map.end(), 4) == 800,
          "the fixed center must contain exactly 40 by 20 protected macroblocks");
  require(std::all_of(emphasis_map.begin(), emphasis_map.end(),
                      [](std::int8_t level) { return level == 0 || level == 4; }),
          "the frozen map must contain only declared emphasis levels");

  const std::vector<std::uint8_t> reference = {10, 20, 30, 40};
  const std::vector<std::uint8_t> decoded = {10, 22, 27, 40};
  const auto metric = glyphrelay::compute_luma_metric(reference, 2, decoded, 2, {0, 0, 2, 2});
  require(metric.squared_error == 13U && metric.sample_count == 4U,
          "luma squared error must use exact integer accumulation");
  require(std::abs(metric.mean_squared_error - 3.25) < 1e-12,
          "luma MSE must use the declared sample denominator");
  require(std::abs(metric.psnr_db - 43.01196999889036) < 1e-9,
          "luma PSNR must use peak 255 and the frozen formula");

  const auto manifest =
      std::filesystem::path(GLYPHRELAY_SOURCE_DIR) / "protocols/m0_fixed_map_v1/manifest.lock";
  const auto verification = glyphrelay::verify_m0_protocol(manifest);
  require(verification.passed, "the committed M0 protocol must pass complete verification");
  require(verification.lock.manifest_sha256 == GLYPHRELAY_M0_PROTOCOL_SHA256,
          "the M0 protocol identity must match the compiled immutable lock");

  TemporaryDirectory temporary;
  for (const auto &component : verification.lock.components) {
    const auto destination = temporary.path() / component.relative_path;
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy_file(verification.lock.repository_root / component.relative_path,
                               destination);
  }
  const auto copied_manifest = temporary.path() / "protocols/m0_fixed_map_v1/manifest.lock";
  std::filesystem::copy_file(manifest, copied_manifest);
  std::ofstream tamper(temporary.path() / "protocols/m0_fixed_map_v1/source.json",
                       std::ios::binary | std::ios::app);
  tamper << ' ';
  tamper.close();
  const auto rejected = glyphrelay::verify_m0_protocol(copied_manifest);
  require(!rejected.passed && rejected.reason.find("component hash mismatch") != std::string::npos,
          "a modified frozen source component must fail for a hash mismatch");

  return 0;
}
