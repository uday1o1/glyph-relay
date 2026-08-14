#pragma once

#include "glyphrelay/synthetic_source.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace glyphrelay {

struct M0BenchmarkTimingSample {
  double latency_ms = 0.0;
  std::size_t pending_count = 0U;
  double oldest_pending_ms = 0.0;
};

struct M0BenchmarkRunGate {
  bool passed = false;
  double payload_bps = 0.0;
  double latency_p95_ms = 0.0;
  double latency_p99_ms = 0.0;
  double maximum_pending_age_ms = 0.0;
  double first_quarter_pending_mean = 0.0;
  double last_quarter_pending_mean = 0.0;
  std::vector<std::string> failures;
};

double nearest_rank_percentile(std::span<const double> values, double probability);
double m0_payload_mean(std::span<const double> payload_bps);
bool m0_payload_mean_within_window(std::span<const double> payload_bps);
M0BenchmarkRunGate
evaluate_m0_benchmark_run_gate(double payload_bps,
                               std::span<const M0BenchmarkTimingSample> measurement_samples);

} // namespace glyphrelay
