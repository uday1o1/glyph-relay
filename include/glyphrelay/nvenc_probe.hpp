#pragma once

#include "glyphrelay/cuda_context.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace glyphrelay {

struct NvencCapabilityReport {
  bool passed = false;
  std::string reason = "nvenc_probe_not_run";
  std::string compiled_header_version = "13.1";
  std::string maximum_driver_api_version = "unavailable";
  bool api_compatible = false;
  bool h264 = false;
  bool emphasis_map = false;
  bool nv12 = false;
  std::size_t maximum_width = 0;
  std::size_t maximum_height = 0;
  CudaContextIdentity context;
};

bool nvenc_api_version_compatible(std::uint32_t maximum_supported, std::uint32_t compiled_version);
NvencCapabilityReport probe_nvenc_capabilities(CudaPrimaryContext &context);
std::string nvenc_capability_report_json(const NvencCapabilityReport &report);

} // namespace glyphrelay
