#include "glyphrelay/benchmark_gate.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace glyphrelay {
namespace {

double quarter_pending_mean(std::span<const M0BenchmarkTimingSample> samples, bool last) {
  if (samples.size() < 4U) {
    throw std::invalid_argument("benchmark pending trend requires at least four samples");
  }
  const auto count = samples.size() / 4U;
  const auto begin = last ? samples.size() - count : 0U;
  return std::accumulate(samples.begin() + static_cast<std::ptrdiff_t>(begin),
                         samples.begin() + static_cast<std::ptrdiff_t>(begin + count), 0.0,
                         [](double sum, const M0BenchmarkTimingSample &sample) {
                           return sum + static_cast<double>(sample.pending_count);
                         }) /
         static_cast<double>(count);
}

} // namespace

double nearest_rank_percentile(std::span<const double> values, double probability) {
  if (values.empty() || probability <= 0.0 || probability > 1.0 ||
      !std::all_of(values.begin(), values.end(),
                   [](double value) { return std::isfinite(value) && value >= 0.0; })) {
    throw std::invalid_argument("benchmark percentile input is invalid");
  }
  std::vector<double> ordered(values.begin(), values.end());
  std::sort(ordered.begin(), ordered.end());
  const auto rank =
      static_cast<std::size_t>(std::ceil(probability * static_cast<double>(ordered.size())));
  return ordered[std::max<std::size_t>(1U, rank) - 1U];
}

double m0_payload_mean(std::span<const double> payload_bps) {
  if (payload_bps.empty() || !std::all_of(payload_bps.begin(), payload_bps.end(), [](double value) {
        return std::isfinite(value) && value >= 0.0;
      })) {
    throw std::invalid_argument("benchmark payload input is invalid");
  }
  return std::accumulate(payload_bps.begin(), payload_bps.end(), 0.0) /
         static_cast<double>(payload_bps.size());
}

bool m0_payload_mean_within_window(std::span<const double> payload_bps) {
  const auto mean = m0_payload_mean(payload_bps);
  return mean >= 980'000.0 && mean <= 1'020'000.0;
}

M0BenchmarkRunGate
evaluate_m0_benchmark_run_gate(double payload_bps,
                               std::span<const M0BenchmarkTimingSample> measurement_samples) {
  if (!std::isfinite(payload_bps) || payload_bps < 0.0 ||
      measurement_samples.size() != M0SourceGeometry::measurement_frames) {
    throw std::invalid_argument("benchmark run gate input is invalid");
  }
  std::vector<double> latencies;
  latencies.reserve(measurement_samples.size());
  M0BenchmarkRunGate result;
  result.payload_bps = payload_bps;
  for (const auto &sample : measurement_samples) {
    if (!std::isfinite(sample.latency_ms) || sample.latency_ms < 0.0 ||
        !std::isfinite(sample.oldest_pending_ms) || sample.oldest_pending_ms < 0.0) {
      throw std::invalid_argument("benchmark timing sample is invalid");
    }
    latencies.push_back(sample.latency_ms);
    result.maximum_pending_age_ms =
        std::max(result.maximum_pending_age_ms, sample.oldest_pending_ms);
  }
  result.latency_p95_ms = nearest_rank_percentile(latencies, 0.95);
  result.latency_p99_ms = nearest_rank_percentile(latencies, 0.99);
  result.first_quarter_pending_mean = quarter_pending_mean(measurement_samples, false);
  result.last_quarter_pending_mean = quarter_pending_mean(measurement_samples, true);
  if (result.latency_p95_ms > 10.0) {
    result.failures.emplace_back("encode_latency_p95_exceeded");
  }
  if (result.latency_p99_ms > 16.0) {
    result.failures.emplace_back("encode_latency_p99_exceeded");
  }
  if (result.maximum_pending_age_ms > 33.34) {
    result.failures.emplace_back("pending_age_exceeded");
  }
  if (result.last_quarter_pending_mean > result.first_quarter_pending_mean) {
    result.failures.emplace_back("pending_count_positive_trend");
  }
  result.passed = result.failures.empty();
  return result;
}

} // namespace glyphrelay
