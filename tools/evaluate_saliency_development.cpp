#include "glyphrelay/cuda_preprocess.hpp"
#include "glyphrelay/saliency_development.hpp"
#include "glyphrelay/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 16U> kBundleMagic = {'G', 'L', 'Y', 'P', 'H', '_', 'S', 'A',
                                                        'L', '_', 'D', 'E', 'V', '1', 0,   0};
constexpr std::array<std::uint8_t, 8U> kResultMagic = {'S', 'A', 'L', 'R', 'S', 'L', 'T', '1'};
constexpr std::uint32_t kBundleVersion = 1U;
constexpr std::size_t kWidth = 1920U;
constexpr std::size_t kHeight = 1080U;
constexpr std::size_t kSequenceCount = 64U;
constexpr std::size_t kFrameCount = 4U;
constexpr std::size_t kMacroblockCount = 120U * 68U;
constexpr std::size_t kFrameBytes = kWidth * kHeight * 4U;
constexpr std::size_t kStratumCount = 7U;
constexpr std::size_t kCandidateCount = 2511U;
constexpr std::uint64_t kFrameIntervalNs = 33'333'333U;
constexpr std::uint64_t kSampleIntervalNs = 2'000'000'000U;

constexpr std::array<std::string_view, kStratumCount> kStrata = {
    "animated_typing_scrolling",
    "browser_documentation",
    "code_editor",
    "mixed_video_text",
    "slide_diagram",
    "spreadsheet_table",
    "terminal",
};

struct FrameData {
  std::uint64_t source_frame_id = 0U;
  std::uint64_t source_pts_ns = 0U;
  std::array<std::uint8_t, 32U> png_sha256{};
  std::vector<std::uint8_t> glyph_truth;
  std::vector<std::uint8_t> small_glyph_truth;
  std::vector<std::uint8_t> ui_truth;
  glyphrelay::CapturedFrame captured;
};

struct SequenceData {
  std::string identifier;
  std::size_t stratum = 0U;
  bool static_sequence = false;
  std::array<FrameData, kFrameCount> frames;
};

struct MetricSet {
  double overall_glyph_recall = 0.0;
  double small_glyph_recall = 0.0;
  double protected_fraction = 0.0;
  double false_protected_fraction = 0.0;
  double false_discovery_fraction = 0.0;
  double static_map_change_fraction = 0.0;
};

struct CandidateMetrics {
  MetricSet aggregate;
  std::array<MetricSet, kStratumCount> per_stratum{};
  MetricSet p95_sequence;
  std::uint64_t processing_p95_ns = 0U;
};

struct CandidateResult {
  bool passed = false;
  std::string reason;
  glyphrelay::SaliencyConfiguration configuration;
  CandidateMetrics metrics;
};

struct Arguments {
  std::filesystem::path bundle;
  std::filesystem::path checkpoint;
  std::filesystem::path output;
  std::string bundle_sha256;
  std::string evaluation_identity;
  std::string source_bundle_id;
  std::string automatic_map_implementation_sha256;
  std::string processing_platform_sha256;
  std::string corpus_protocol_sha256;
  std::string development_manifest_sha256;
  std::string development_render_index_sha256;
  std::string grid_sha256;
};

class BinaryReader {
public:
  explicit BinaryReader(const std::filesystem::path &path) : input_(path, std::ios::binary) {
    if (!input_) {
      throw std::runtime_error("development_bundle_open_failed");
    }
  }

  void read(std::span<std::uint8_t> output) {
    if (output.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
      throw std::runtime_error("development_bundle_read_size_overflow");
    }
    input_.read(reinterpret_cast<char *>(output.data()),
                static_cast<std::streamsize>(output.size()));
    if (!input_) {
      throw std::runtime_error("development_bundle_truncated");
    }
  }

  std::uint8_t u8() {
    std::array<std::uint8_t, 1U> bytes{};
    read(bytes);
    return bytes[0];
  }

  std::uint16_t u16() {
    std::array<std::uint8_t, 2U> bytes{};
    read(bytes);
    return static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8U);
  }

  std::uint32_t u32() {
    std::array<std::uint8_t, 4U> bytes{};
    read(bytes);
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
    }
    return value;
  }

  std::uint64_t u64() {
    std::array<std::uint8_t, 8U> bytes{};
    read(bytes);
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
  }

  std::string text(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    read(bytes);
    return {bytes.begin(), bytes.end()};
  }

  bool exhausted() { return input_.peek() == std::char_traits<char>::eof(); }

private:
  std::ifstream input_;
};

bool hex_sha256(std::string_view value) {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

std::array<std::uint8_t, 32U> parse_sha256(std::string_view value) {
  if (!hex_sha256(value)) {
    throw std::runtime_error("development_sha256_argument_invalid");
  }
  std::array<std::uint8_t, 32U> output{};
  auto nibble = [](char character) {
    return static_cast<std::uint8_t>(character <= '9' ? character - '0' : character - 'a' + 10);
  };
  for (std::size_t index = 0U; index < output.size(); ++index) {
    output[index] = static_cast<std::uint8_t>((nibble(value[index * 2U]) << 4U) |
                                              nibble(value[index * 2U + 1U]));
  }
  return output;
}

void require_bundle_sha(BinaryReader &reader, std::string_view expected, const char *reason) {
  std::array<std::uint8_t, 32U> actual{};
  reader.read(actual);
  if (actual != parse_sha256(expected)) {
    throw std::runtime_error(reason);
  }
}

std::vector<SequenceData> load_bundle(const Arguments &arguments) {
  if (glyphrelay::sha256_file_hex(arguments.bundle) != arguments.bundle_sha256) {
    throw std::runtime_error("development_bundle_sha256_mismatch");
  }
  BinaryReader reader(arguments.bundle);
  std::array<std::uint8_t, kBundleMagic.size()> magic{};
  reader.read(magic);
  if (magic != kBundleMagic || reader.u32() != kBundleVersion || reader.u32() != kWidth ||
      reader.u32() != kHeight || reader.u32() != kSequenceCount || reader.u32() != kFrameCount ||
      reader.u32() != kMacroblockCount || reader.u32() != kFrameBytes) {
    throw std::runtime_error("development_bundle_header_invalid");
  }
  require_bundle_sha(reader, arguments.corpus_protocol_sha256,
                     "development_bundle_corpus_identity_mismatch");
  require_bundle_sha(reader, arguments.development_manifest_sha256,
                     "development_bundle_manifest_identity_mismatch");
  require_bundle_sha(reader, arguments.development_render_index_sha256,
                     "development_bundle_render_identity_mismatch");
  require_bundle_sha(reader, arguments.grid_sha256, "development_bundle_grid_identity_mismatch");

  std::vector<SequenceData> sequences(kSequenceCount);
  std::array<std::size_t, kStratumCount> stratum_counts{};
  for (std::size_t sequence_index = 0U; sequence_index < sequences.size(); ++sequence_index) {
    auto &sequence = sequences[sequence_index];
    const auto identifier_size = reader.u16();
    sequence.stratum = reader.u8();
    const auto static_value = reader.u8();
    if (identifier_size == 0U || identifier_size > 96U || sequence.stratum >= kStratumCount ||
        static_value > 1U) {
      throw std::runtime_error("development_bundle_sequence_header_invalid");
    }
    sequence.identifier = reader.text(identifier_size);
    sequence.static_sequence = static_value != 0U;
    ++stratum_counts[sequence.stratum];
    for (auto &frame : sequence.frames) {
      frame.source_frame_id = reader.u64();
      frame.source_pts_ns = reader.u64();
      reader.read(frame.png_sha256);
      frame.glyph_truth.resize(kMacroblockCount);
      frame.small_glyph_truth.resize(kMacroblockCount);
      frame.ui_truth.resize(kMacroblockCount);
      reader.read(frame.glyph_truth);
      reader.read(frame.small_glyph_truth);
      reader.read(frame.ui_truth);
      frame.captured.geometry.source_width = kWidth;
      frame.captured.geometry.source_height = kHeight;
      frame.captured.geometry.source_crop = {0U, 0U, kWidth, kHeight};
      frame.captured.geometry.visible_width = kWidth;
      frame.captured.geometry.visible_height = kHeight;
      frame.captured.geometry.coded_width = kWidth;
      frame.captured.geometry.coded_height = kHeight;
      frame.captured.pixel_order = glyphrelay::PackedPixelOrder::bgra;
      frame.captured.pitch = kWidth * 4U;
      frame.captured.pixels.resize(kFrameBytes);
      reader.read(frame.captured.pixels);
    }
  }
  if (!reader.exhausted() || std::any_of(stratum_counts.begin(), stratum_counts.end(),
                                         [](std::size_t count) { return count < 8U; })) {
    throw std::runtime_error("development_bundle_sequence_coverage_invalid");
  }
  return sequences;
}

std::vector<glyphrelay::SaliencyConfiguration> enumerate_configurations() {
  constexpr std::array weights = {0.15, 0.25, 0.35, 0.45};
  constexpr std::array entries = {0.50, 0.55, 0.60};
  constexpr std::array exits = {0.30, 0.35, 0.40};
  constexpr std::array coefficients = {0.40, 0.60, 0.80};
  constexpr std::array<std::size_t, 3U> radii = {0U, 1U, 2U};
  std::vector<glyphrelay::SaliencyConfiguration> configurations;
  for (const auto gradient : weights) {
    for (const auto contrast : weights) {
      for (const auto edge : weights) {
        for (const auto small : weights) {
          if (std::abs(gradient + contrast + edge + small - 1.0) > 1e-12) {
            continue;
          }
          for (const auto entry : entries) {
            for (const auto exit : exits) {
              if (exit >= entry) {
                continue;
              }
              for (const auto coefficient : coefficients) {
                for (const auto radius : radii) {
                  configurations.push_back(
                      {gradient, contrast, edge, small, entry, exit, coefficient, radius});
                }
              }
            }
          }
        }
      }
    }
  }
  auto key = [](const glyphrelay::SaliencyConfiguration &value) {
    return std::tuple(value.gradient_weight, value.contrast_weight, value.edge_pair_weight,
                      value.small_structure_weight, value.entry_threshold, value.exit_threshold,
                      value.previous_score_coefficient, value.dilation_radius_tiles);
  };
  std::sort(configurations.begin(), configurations.end(),
            [&](const auto &left, const auto &right) { return key(left) < key(right); });
  if (configurations.size() != kCandidateCount ||
      std::any_of(configurations.begin(), configurations.end(), [](const auto &configuration) {
        return !glyphrelay::valid_saliency_configuration(configuration);
      })) {
    throw std::runtime_error("development_native_grid_invalid");
  }
  return configurations;
}

bool complete_lifecycle(glyphrelay::CudaPreprocessor &pipeline,
                        const glyphrelay::CudaPreprocessTicket &ticket, std::string &reason) {
  const auto submitted = pipeline.mark_submitted(ticket);
  const auto released = pipeline.mark_encoder_input_released(ticket);
  const auto recycled = pipeline.release(ticket);
  if (!submitted.passed || !released.passed || !recycled.passed) {
    reason = "development_cuda_lifecycle_failed";
    return false;
  }
  return true;
}

double metric_value(const glyphrelay::SaliencyMapMetrics &metrics, std::size_t index) {
  switch (index) {
  case 0U:
    return metrics.overall_glyph_recall;
  case 1U:
    return metrics.small_glyph_recall;
  case 2U:
    return metrics.protected_fraction;
  case 3U:
    return metrics.false_protected_fraction;
  case 4U:
    return metrics.false_discovery_fraction;
  case 5U:
    return metrics.static_map_change_fraction;
  default:
    throw std::logic_error("development_metric_index_invalid");
  }
}

void set_metric(MetricSet &metrics, std::size_t index, double value) {
  switch (index) {
  case 0U:
    metrics.overall_glyph_recall = value;
    break;
  case 1U:
    metrics.small_glyph_recall = value;
    break;
  case 2U:
    metrics.protected_fraction = value;
    break;
  case 3U:
    metrics.false_protected_fraction = value;
    break;
  case 4U:
    metrics.false_discovery_fraction = value;
    break;
  case 5U:
    metrics.static_map_change_fraction = value;
    break;
  default:
    throw std::logic_error("development_metric_index_invalid");
  }
}

double mean(const std::vector<double> &values) {
  if (values.empty()) {
    throw std::runtime_error("development_metric_group_empty");
  }
  double sum = 0.0;
  for (const auto value : values) {
    sum += value;
  }
  return sum / static_cast<double>(values.size());
}

double nearest_rank_p95(std::vector<double> values) {
  if (values.empty()) {
    throw std::runtime_error("development_p95_group_empty");
  }
  std::sort(values.begin(), values.end());
  const auto rank = (95U * values.size() + 99U) / 100U;
  return values[rank - 1U];
}

std::uint64_t nearest_rank_p95(std::vector<std::uint64_t> values) {
  if (values.empty()) {
    throw std::runtime_error("development_processing_samples_empty");
  }
  std::sort(values.begin(), values.end());
  const auto rank = (95U * values.size() + 99U) / 100U;
  return values[rank - 1U];
}

CandidateMetrics aggregate_metrics(
    const std::vector<std::pair<std::size_t, glyphrelay::SaliencyMapMetrics>> &sequences,
    std::vector<std::uint64_t> processing_samples) {
  if (sequences.size() != kSequenceCount) {
    throw std::runtime_error("development_sequence_metric_count_invalid");
  }
  CandidateMetrics output;
  for (std::size_t metric = 0U; metric < 6U; ++metric) {
    std::vector<double> corpus_sequences;
    std::array<std::vector<double>, kStratumCount> per_stratum;
    for (const auto &[stratum, value] : sequences) {
      if (metric == 5U && !value.static_map_change_observed) {
        continue;
      }
      const auto number = metric_value(value, metric);
      corpus_sequences.push_back(number);
      per_stratum[stratum].push_back(number);
    }
    std::vector<double> stratum_means;
    for (std::size_t stratum = 0U; stratum < kStratumCount; ++stratum) {
      const auto value = mean(per_stratum[stratum]);
      set_metric(output.per_stratum[stratum], metric, value);
      stratum_means.push_back(value);
    }
    set_metric(output.aggregate, metric, mean(stratum_means));
    set_metric(output.p95_sequence, metric, nearest_rank_p95(std::move(corpus_sequences)));
  }
  output.processing_p95_ns = nearest_rank_p95(std::move(processing_samples));
  if (output.processing_p95_ns == 0U) {
    throw std::runtime_error("development_processing_p95_zero");
  }
  return output;
}

CandidateResult evaluate_candidate(glyphrelay::SaliencyConfiguration configuration,
                                   std::vector<SequenceData> &sequences) {
  CandidateResult result;
  result.configuration = configuration;
  glyphrelay::CudaPreprocessor pipeline(0, kWidth, kHeight, 3U, 3U, configuration);
  if (!pipeline.available()) {
    result.reason = pipeline.reason();
    return result;
  }
  std::vector<std::pair<std::size_t, glyphrelay::SaliencyMapMetrics>> sequence_metrics;
  std::vector<std::uint64_t> processing_samples;
  sequence_metrics.reserve(sequences.size());
  processing_samples.reserve(sequences.size() * kFrameCount);
  std::uint64_t runtime_frame_id = 1U;
  std::uint64_t runtime_timestamp_ns = kFrameIntervalNs;
  for (std::size_t sequence_index = 0U; sequence_index < sequences.size(); ++sequence_index) {
    auto &sequence = sequences[sequence_index];
    glyphrelay::SaliencySequenceMetrics metrics(kMacroblockCount, sequence.static_sequence);
    auto process = [&](FrameData &frame, bool measure) {
      frame.captured.frame_id = runtime_frame_id++;
      frame.captured.geometry.epoch = sequence_index + 1U;
      frame.captured.monotonic_timestamp_ns = runtime_timestamp_ns;
      const auto ticket = pipeline.enqueue(frame.captured, glyphrelay::ColorRange::full);
      if (!ticket.passed) {
        result.reason = ticket.reason;
        return false;
      }
      const auto completion = pipeline.wait(ticket);
      if (!completion.passed) {
        result.reason = completion.reason;
        return false;
      }
      if (measure) {
        const auto observed = metrics.observe({completion.emphasis_map.values, frame.glyph_truth,
                                               frame.small_glyph_truth, frame.ui_truth});
        if (!observed.passed) {
          result.reason = observed.reason;
          return false;
        }
        processing_samples.push_back(completion.timings.total_pipeline);
      }
      return complete_lifecycle(pipeline, ticket, result.reason);
    };
    for (std::size_t warmup = 0U; warmup < 5U; ++warmup) {
      if (!process(sequence.frames[0], warmup == 4U)) {
        pipeline.close_admission();
        return result;
      }
      runtime_timestamp_ns += kFrameIntervalNs;
    }
    for (std::size_t frame = 1U; frame < kFrameCount; ++frame) {
      runtime_timestamp_ns += kSampleIntervalNs;
      if (!process(sequence.frames[frame], true)) {
        pipeline.close_admission();
        return result;
      }
    }
    runtime_timestamp_ns += kSampleIntervalNs;
    if (metrics.observed_frames() != kFrameCount) {
      result.reason = "development_sequence_observation_count_invalid";
      pipeline.close_admission();
      return result;
    }
    sequence_metrics.emplace_back(sequence.stratum, metrics.finalize());
  }
  pipeline.close_admission();
  if (!pipeline.all_free()) {
    result.reason = "development_cuda_resources_not_drained";
    return result;
  }
  try {
    result.metrics = aggregate_metrics(sequence_metrics, std::move(processing_samples));
  } catch (const std::exception &error) {
    result.reason = error.what();
    return result;
  }
  result.passed = true;
  result.reason = "development_candidate_passed";
  return result;
}

void append_u16(std::vector<std::uint8_t> &output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t> &output, std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void append_u64(std::vector<std::uint8_t> &output, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void append_double(std::vector<std::uint8_t> &output, double value) {
  append_u64(output, std::bit_cast<std::uint64_t>(value));
}

void append_metric_set(std::vector<std::uint8_t> &output, const MetricSet &metrics) {
  append_double(output, metrics.overall_glyph_recall);
  append_double(output, metrics.small_glyph_recall);
  append_double(output, metrics.protected_fraction);
  append_double(output, metrics.false_protected_fraction);
  append_double(output, metrics.false_discovery_fraction);
  append_double(output, metrics.static_map_change_fraction);
}

std::vector<std::uint8_t> serialize_result(std::size_t index, const CandidateResult &result,
                                           std::string_view evaluation_identity) {
  if (result.reason.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("development_candidate_reason_too_long");
  }
  std::vector<std::uint8_t> output(kResultMagic.begin(), kResultMagic.end());
  const auto identity = parse_sha256(evaluation_identity);
  output.insert(output.end(), identity.begin(), identity.end());
  append_u32(output, static_cast<std::uint32_t>(index));
  output.push_back(result.passed ? 1U : 0U);
  append_u16(output, static_cast<std::uint16_t>(result.reason.size()));
  output.insert(output.end(), result.reason.begin(), result.reason.end());
  const auto &configuration = result.configuration;
  append_double(output, configuration.gradient_weight);
  append_double(output, configuration.contrast_weight);
  append_double(output, configuration.edge_pair_weight);
  append_double(output, configuration.small_structure_weight);
  append_double(output, configuration.entry_threshold);
  append_double(output, configuration.exit_threshold);
  append_double(output, configuration.previous_score_coefficient);
  append_u32(output, static_cast<std::uint32_t>(configuration.dilation_radius_tiles));
  if (result.passed) {
    append_metric_set(output, result.metrics.aggregate);
    for (const auto &metrics : result.metrics.per_stratum) {
      append_metric_set(output, metrics);
    }
    append_metric_set(output, result.metrics.p95_sequence);
    append_u64(output, result.metrics.processing_p95_ns);
  }
  const auto digest = glyphrelay::Sha256::digest(output);
  output.insert(output.end(), digest.begin(), digest.end());
  return output;
}

std::uint16_t take_u16(std::span<const std::uint8_t> input, std::size_t &offset) {
  if (offset + 2U > input.size()) {
    throw std::runtime_error("development_checkpoint_truncated");
  }
  const auto value = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(input[offset]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[offset + 1U]) << 8U));
  offset += 2U;
  return value;
}

std::uint32_t take_u32(std::span<const std::uint8_t> input, std::size_t &offset) {
  if (offset + 4U > input.size()) {
    throw std::runtime_error("development_checkpoint_truncated");
  }
  std::uint32_t value = 0U;
  for (std::size_t index = 0U; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(input[offset + index]) << (index * 8U);
  }
  offset += 4U;
  return value;
}

std::uint64_t take_u64(std::span<const std::uint8_t> input, std::size_t &offset) {
  if (offset + 8U > input.size()) {
    throw std::runtime_error("development_checkpoint_truncated");
  }
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(input[offset + index]) << (index * 8U);
  }
  offset += 8U;
  return value;
}

double take_double(std::span<const std::uint8_t> input, std::size_t &offset) {
  return std::bit_cast<double>(take_u64(input, offset));
}

MetricSet take_metric_set(std::span<const std::uint8_t> input, std::size_t &offset) {
  return {
      take_double(input, offset), take_double(input, offset), take_double(input, offset),
      take_double(input, offset), take_double(input, offset), take_double(input, offset),
  };
}

CandidateResult parse_result(const std::filesystem::path &path, std::size_t expected_index,
                             const glyphrelay::SaliencyConfiguration &expected_configuration,
                             std::string_view evaluation_identity) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() < 0 || input.tellg() > 4096) {
    throw std::runtime_error("development_checkpoint_size_invalid");
  }
  const auto size = static_cast<std::size_t>(input.tellg());
  input.seekg(0);
  std::vector<std::uint8_t> bytes(size);
  input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input || bytes.size() < kResultMagic.size() + 32U + 32U) {
    throw std::runtime_error("development_checkpoint_read_failed");
  }
  const std::span<const std::uint8_t> body(bytes.data(), bytes.size() - 32U);
  const auto digest = glyphrelay::Sha256::digest(body);
  if (!std::equal(digest.begin(), digest.end(), bytes.end() - 32)) {
    throw std::runtime_error("development_checkpoint_sha256_invalid");
  }
  std::size_t offset = 0U;
  if (!std::equal(kResultMagic.begin(), kResultMagic.end(), bytes.begin())) {
    throw std::runtime_error("development_checkpoint_magic_invalid");
  }
  offset += kResultMagic.size();
  const auto identity = parse_sha256(evaluation_identity);
  if (!std::equal(identity.begin(), identity.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
    throw std::runtime_error("development_checkpoint_identity_invalid");
  }
  offset += identity.size();
  if (take_u32(body, offset) != expected_index || offset >= body.size()) {
    throw std::runtime_error("development_checkpoint_index_invalid");
  }
  CandidateResult result;
  result.passed = body[offset++] != 0U;
  const auto reason_size = take_u16(body, offset);
  if (offset + reason_size > body.size()) {
    throw std::runtime_error("development_checkpoint_reason_invalid");
  }
  result.reason.assign(reinterpret_cast<const char *>(body.data() + offset), reason_size);
  offset += reason_size;
  result.configuration = {
      take_double(body, offset), take_double(body, offset), take_double(body, offset),
      take_double(body, offset), take_double(body, offset), take_double(body, offset),
      take_double(body, offset), take_u32(body, offset),
  };
  auto configuration_key = [](const glyphrelay::SaliencyConfiguration &value) {
    return std::tuple(value.gradient_weight, value.contrast_weight, value.edge_pair_weight,
                      value.small_structure_weight, value.entry_threshold, value.exit_threshold,
                      value.previous_score_coefficient, value.dilation_radius_tiles);
  };
  if (configuration_key(result.configuration) != configuration_key(expected_configuration)) {
    throw std::runtime_error("development_checkpoint_configuration_invalid");
  }
  if (result.passed) {
    result.metrics.aggregate = take_metric_set(body, offset);
    for (auto &metrics : result.metrics.per_stratum) {
      metrics = take_metric_set(body, offset);
    }
    result.metrics.p95_sequence = take_metric_set(body, offset);
    result.metrics.processing_p95_ns = take_u64(body, offset);
  }
  if (offset != body.size()) {
    throw std::runtime_error("development_checkpoint_trailing_data");
  }
  return result;
}

void fsync_directory(const std::filesystem::path &path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (descriptor < 0) {
    throw std::runtime_error("development_checkpoint_directory_open_failed");
  }
  const int result = ::fsync(descriptor);
  const int saved_errno = errno;
  ::close(descriptor);
  if (result != 0) {
    throw std::runtime_error(std::string("development_checkpoint_directory_fsync_failed:") +
                             std::strerror(saved_errno));
  }
}

void write_all(int descriptor, std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::runtime_error("development_checkpoint_write_failed");
    }
    offset += static_cast<std::size_t>(written);
  }
}

void durable_write(const std::filesystem::path &target, std::span<const std::uint8_t> bytes) {
  const auto temporary = target.parent_path() / ("." + target.filename().string() + "." +
                                                 std::to_string(::getpid()) + ".tmp");
  const int descriptor =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw std::runtime_error("development_checkpoint_temporary_open_failed");
  }
  try {
    write_all(descriptor, bytes);
    if (::fsync(descriptor) != 0 || ::close(descriptor) != 0) {
      throw std::runtime_error("development_checkpoint_file_fsync_failed");
    }
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
      throw std::runtime_error("development_checkpoint_publish_failed");
    }
    fsync_directory(target.parent_path());
  } catch (...) {
    ::close(descriptor);
    ::unlink(temporary.c_str());
    throw;
  }
}

std::filesystem::path checkpoint_path(const Arguments &arguments, std::size_t index) {
  std::ostringstream name;
  name << std::setw(4) << std::setfill('0') << index << ".result";
  return arguments.checkpoint / name.str();
}

std::vector<CandidateResult>
evaluate_or_resume(const Arguments &arguments,
                   const std::vector<glyphrelay::SaliencyConfiguration> &configurations,
                   std::vector<SequenceData> &sequences) {
  std::filesystem::create_directories(arguments.checkpoint);
  std::vector<CandidateResult> results;
  results.reserve(configurations.size());
  for (std::size_t index = 0U; index < configurations.size(); ++index) {
    const auto path = checkpoint_path(arguments, index);
    CandidateResult result;
    if (std::filesystem::exists(path)) {
      result = parse_result(path, index, configurations[index], arguments.evaluation_identity);
    } else {
      result = evaluate_candidate(configurations[index], sequences);
      const auto serialized = serialize_result(index, result, arguments.evaluation_identity);
      durable_write(path, serialized);
    }
    results.push_back(std::move(result));
    if ((index + 1U) % 25U == 0U || index + 1U == configurations.size()) {
      std::cout << "saliency development candidates complete: " << (index + 1U) << '/'
                << configurations.size() << '\n';
    }
  }
  return results;
}

void write_configuration(std::ostringstream &output,
                         const glyphrelay::SaliencyConfiguration &configuration) {
  output << std::setprecision(17) << "{\"contrastWeight\":" << configuration.contrast_weight
         << ",\"dilationRadiusTiles\":" << configuration.dilation_radius_tiles
         << ",\"edgePairWeight\":" << configuration.edge_pair_weight
         << ",\"entryThreshold\":" << configuration.entry_threshold
         << ",\"exitThreshold\":" << configuration.exit_threshold
         << ",\"gradientWeight\":" << configuration.gradient_weight
         << ",\"previousScoreCoefficient\":" << configuration.previous_score_coefficient
         << ",\"smallStructureWeight\":" << configuration.small_structure_weight << '}';
}

void write_metric_set(std::ostringstream &output, const MetricSet &metrics) {
  output << std::setprecision(17)
         << "{\"falseDiscoveryFraction\":" << metrics.false_discovery_fraction
         << ",\"falseProtectedFraction\":" << metrics.false_protected_fraction
         << ",\"overallGlyphRecall\":" << metrics.overall_glyph_recall
         << ",\"protectedFraction\":" << metrics.protected_fraction
         << ",\"smallGlyphRecall\":" << metrics.small_glyph_recall
         << ",\"staticMapChangeFraction\":" << metrics.static_map_change_fraction << '}';
}

void write_json_string(std::ostringstream &output, std::string_view value) {
  constexpr std::string_view hexadecimal = "0123456789abcdef";
  output << '"';
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte == static_cast<unsigned char>('"') || byte == static_cast<unsigned char>('\\')) {
      output << '\\' << static_cast<char>(byte);
    } else if (byte >= 0x20U && byte < 0x7fU) {
      output << static_cast<char>(byte);
    } else {
      output << "\\u00" << hexadecimal[(byte >> 4U) & 0x0fU] << hexadecimal[byte & 0x0fU];
    }
  }
  output << '"';
}

std::string evidence_json(const Arguments &arguments, const std::vector<CandidateResult> &results) {
  std::ostringstream output;
  output << "{\"automaticMapImplementationSha256\":\""
         << arguments.automatic_map_implementation_sha256 << "\",\"candidates\":[";
  for (std::size_t index = 0U; index < results.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    const auto &result = results[index];
    output << "{\"configuration\":";
    write_configuration(output, result.configuration);
    output << ",\"invalidReason\":";
    if (result.passed) {
      output << "null,\"metrics\":";
      output << '{';
      output << "\"falseDiscoveryFraction\":" << std::setprecision(17)
             << result.metrics.aggregate.false_discovery_fraction
             << ",\"falseProtectedFraction\":" << result.metrics.aggregate.false_protected_fraction
             << ",\"overallGlyphRecall\":" << result.metrics.aggregate.overall_glyph_recall
             << ",\"p95Sequence\":";
      write_metric_set(output, result.metrics.p95_sequence);
      output << ",\"perStratum\":{";
      for (std::size_t stratum = 0U; stratum < kStratumCount; ++stratum) {
        if (stratum != 0U) {
          output << ',';
        }
        output << '"' << kStrata[stratum] << "\":";
        write_metric_set(output, result.metrics.per_stratum[stratum]);
      }
      output << "},\"processingP95Ns\":" << result.metrics.processing_p95_ns
             << ",\"protectedFraction\":" << result.metrics.aggregate.protected_fraction
             << ",\"smallGlyphRecall\":" << result.metrics.aggregate.small_glyph_recall
             << ",\"staticMapChangeFraction\":"
             << result.metrics.aggregate.static_map_change_fraction << '}';
    } else {
      write_json_string(output, result.reason);
      output << ",\"metrics\":null";
    }
    output << ",\"status\":\"" << (result.passed ? "PASSED" : "INVALID") << "\"}";
  }
  output << "],\"corpusProtocolSha256\":\"" << arguments.corpus_protocol_sha256
         << "\",\"developmentManifestSha256\":\"" << arguments.development_manifest_sha256
         << "\",\"developmentRenderIndexSha256\":\"" << arguments.development_render_index_sha256
         << "\",\"gridSha256\":\"" << arguments.grid_sha256 << "\",\"processingPlatformSha256\":\""
         << arguments.processing_platform_sha256
         << "\",\"protocol\":\"saliency_v1\",\"schemaVersion\":1,\"sourceBundleId\":\""
         << arguments.source_bundle_id << "\",\"split\":\"development\"}\n";
  return output.str();
}

void write_output(const std::filesystem::path &path, std::string_view text) {
  if (!std::filesystem::is_directory(path.parent_path())) {
    throw std::runtime_error("development_output_parent_missing");
  }
  const int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw std::runtime_error(errno == EEXIST ? "development_output_exists"
                                             : "development_output_open_failed");
  }
  try {
    write_all(descriptor,
              std::span(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
    if (::fsync(descriptor) != 0 || ::close(descriptor) != 0) {
      throw std::runtime_error("development_output_fsync_failed");
    }
    fsync_directory(path.parent_path());
  } catch (...) {
    ::close(descriptor);
    throw;
  }
}

Arguments parse_arguments(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "usage: glyphrelay_saliency_development --bundle FILE --bundle-sha256 HEX "
                 "--checkpoint DIR --output FILE --evaluation-identity HEX "
                 "--source-bundle-id HEX --automatic-map-implementation-sha256 HEX "
                 "--processing-platform-sha256 HEX --corpus-protocol-sha256 HEX "
                 "--development-manifest-sha256 HEX --development-render-index-sha256 HEX "
                 "--grid-sha256 HEX\n";
    std::exit(0);
  }
  if (argc != 25) {
    throw std::runtime_error("development_arguments_invalid");
  }
  std::vector<std::pair<std::string_view, std::string_view>> values;
  for (int index = 1; index < argc; index += 2) {
    values.emplace_back(argv[index], argv[index + 1]);
  }
  auto take = [&](std::string_view name) {
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const auto &item) { return item.first == name; });
    if (found == values.end() || std::count_if(values.begin(), values.end(), [&](const auto &item) {
                                   return item.first == name;
                                 }) != 1) {
      throw std::runtime_error("development_arguments_invalid");
    }
    return std::string(found->second);
  };
  Arguments arguments{
      .bundle = take("--bundle"),
      .checkpoint = take("--checkpoint"),
      .output = take("--output"),
      .bundle_sha256 = take("--bundle-sha256"),
      .evaluation_identity = take("--evaluation-identity"),
      .source_bundle_id = take("--source-bundle-id"),
      .automatic_map_implementation_sha256 = take("--automatic-map-implementation-sha256"),
      .processing_platform_sha256 = take("--processing-platform-sha256"),
      .corpus_protocol_sha256 = take("--corpus-protocol-sha256"),
      .development_manifest_sha256 = take("--development-manifest-sha256"),
      .development_render_index_sha256 = take("--development-render-index-sha256"),
      .grid_sha256 = take("--grid-sha256"),
  };
  for (const auto *identity : {
           &arguments.bundle_sha256,
           &arguments.evaluation_identity,
           &arguments.source_bundle_id,
           &arguments.automatic_map_implementation_sha256,
           &arguments.processing_platform_sha256,
           &arguments.corpus_protocol_sha256,
           &arguments.development_manifest_sha256,
           &arguments.development_render_index_sha256,
           &arguments.grid_sha256,
       }) {
    if (!hex_sha256(*identity)) {
      throw std::runtime_error("development_identity_argument_invalid");
    }
  }
  if (!arguments.bundle.is_absolute() || !arguments.checkpoint.is_absolute() ||
      !arguments.output.is_absolute()) {
    throw std::runtime_error("development_paths_must_be_absolute");
  }
  return arguments;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    auto sequences = load_bundle(arguments);
    const auto configurations = enumerate_configurations();
    const auto results = evaluate_or_resume(arguments, configurations, sequences);
    write_output(arguments.output, evidence_json(arguments, results));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "saliency development evaluation failed: " << error.what() << '\n';
    return 8;
  }
}
