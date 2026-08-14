#pragma once

#include "glyphrelay/m0_protocol.hpp"

#include <filesystem>
#include <string>

namespace glyphrelay {

enum class M0BenchmarkStatus {
  passed,
  unsupported,
  failed,
};

struct M0BenchmarkRequest {
  M0ProtocolLock protocol;
  std::filesystem::path output_directory;
};

struct M0BenchmarkResult {
  M0BenchmarkStatus status = M0BenchmarkStatus::failed;
  std::string reason;
};

bool nvenc_benchmark_backend_available();
M0BenchmarkResult run_m0_nvenc_benchmark(const M0BenchmarkRequest &request);

} // namespace glyphrelay
