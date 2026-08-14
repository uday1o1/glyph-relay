#include "glyphrelay/cuda_context.hpp"
#include "glyphrelay/nvenc_probe.hpp"

#include <iostream>

int main() {
  glyphrelay::CudaPrimaryContext context(0);
  const auto report = glyphrelay::probe_nvenc_capabilities(context);
  std::cout << glyphrelay::nvenc_capability_report_json(report) << '\n';
  if (!context.shutdown() && context.available()) {
    return 8;
  }
  return report.passed ? 0 : 3;
}
