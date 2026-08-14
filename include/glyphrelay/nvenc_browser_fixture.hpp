#pragma once

#include "glyphrelay/m0_protocol.hpp"
#include "glyphrelay/nvenc_benchmark.hpp"

#include <filesystem>

namespace glyphrelay {

struct M0BrowserFixtureRequest {
  M0ProtocolLock protocol;
  std::filesystem::path output_directory;
};

M0BenchmarkResult run_m0_nvenc_browser_fixture(const M0BrowserFixtureRequest &request);

} // namespace glyphrelay
