#include "glyphrelay/cuda_preprocess.hpp"
#include "glyphrelay/saliency_development.hpp"
#include "glyphrelay/sha256.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 16U> kBundleMagic = {'G', 'L', 'Y', 'P', 'H', '_', 'S', 'A',
                                                        'L', '_', 'V', 'A', 'L', '1', 0,   0};
constexpr std::uint32_t kBundleVersion = 1U;
constexpr std::size_t kWidth = 1920U;
constexpr std::size_t kHeight = 1080U;
constexpr std::size_t kSequenceCount = 64U;
constexpr std::size_t kFrameCount = 4U;
constexpr std::size_t kMacroblockCount = 120U * 68U;
constexpr std::size_t kFrameBytes = kWidth * kHeight * 4U;
constexpr std::size_t kStratumCount = 7U;
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

struct Evaluation {
  MetricSet aggregate;
  std::array<MetricSet, kStratumCount> per_stratum{};
  MetricSet p95_sequence;
  std::vector<MetricSet> per_sequence;
  std::uint64_t processing_p95_ns = 0U;
};

struct Arguments {
  std::filesystem::path bundle;
  std::filesystem::path output;
  std::string bundle_sha256;
  std::string source_bundle_id;
  std::string automatic_map_implementation_sha256;
  std::string processing_platform_sha256;
  std::string corpus_protocol_sha256;
  std::string validation_manifest_sha256;
  std::string validation_render_index_sha256;
  std::string configuration_sha256;
  glyphrelay::SaliencyConfiguration configuration;
};

class BinaryReader {
public:
  explicit BinaryReader(const std::filesystem::path &path) : input_(path, std::ios::binary) {
    if (!input_) {
      throw std::runtime_error("validation_bundle_open_failed");
    }
  }

  void read(std::span<std::uint8_t> output) {
    input_.read(reinterpret_cast<char *>(output.data()),
                static_cast<std::streamsize>(output.size()));
    if (!input_) {
      throw std::runtime_error("validation_bundle_truncated");
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
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
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
    throw std::runtime_error("validation_sha256_argument_invalid");
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
    throw std::runtime_error("validation_bundle_sha256_mismatch");
  }
  BinaryReader reader(arguments.bundle);
  std::array<std::uint8_t, kBundleMagic.size()> magic{};
  reader.read(magic);
  if (magic != kBundleMagic || reader.u32() != kBundleVersion || reader.u32() != kWidth ||
      reader.u32() != kHeight || reader.u32() != kSequenceCount || reader.u32() != kFrameCount ||
      reader.u32() != kMacroblockCount || reader.u32() != kFrameBytes) {
    throw std::runtime_error("validation_bundle_header_invalid");
  }
  require_bundle_sha(reader, arguments.corpus_protocol_sha256,
                     "validation_bundle_corpus_identity_mismatch");
  require_bundle_sha(reader, arguments.validation_manifest_sha256,
                     "validation_bundle_manifest_identity_mismatch");
  require_bundle_sha(reader, arguments.validation_render_index_sha256,
                     "validation_bundle_render_identity_mismatch");
  require_bundle_sha(reader, arguments.configuration_sha256,
                     "validation_bundle_configuration_identity_mismatch");
  std::vector<SequenceData> sequences(kSequenceCount);
  std::array<std::size_t, kStratumCount> stratum_counts{};
  for (auto &sequence : sequences) {
    const auto identifier_size = reader.u16();
    sequence.stratum = reader.u8();
    const auto static_value = reader.u8();
    if (identifier_size == 0U || identifier_size > 96U || sequence.stratum >= kStratumCount ||
        static_value > 1U) {
      throw std::runtime_error("validation_bundle_sequence_header_invalid");
    }
    sequence.identifier = reader.text(identifier_size);
    sequence.static_sequence = static_value != 0U;
    ++stratum_counts[sequence.stratum];
    for (auto &frame : sequence.frames) {
      static_cast<void>(reader.u64());
      static_cast<void>(reader.u64());
      std::array<std::uint8_t, 32U> png_sha256{};
      reader.read(png_sha256);
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
    throw std::runtime_error("validation_bundle_sequence_coverage_invalid");
  }
  return sequences;
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
    throw std::logic_error("validation_metric_index_invalid");
  }
}

void set_metric(MetricSet &metrics, std::size_t index, double value) {
  auto values = std::array{&metrics.overall_glyph_recall,     &metrics.small_glyph_recall,
                           &metrics.protected_fraction,       &metrics.false_protected_fraction,
                           &metrics.false_discovery_fraction, &metrics.static_map_change_fraction};
  if (index >= values.size()) {
    throw std::logic_error("validation_metric_index_invalid");
  }
  *values[index] = value;
}

double mean(const std::vector<double> &values) {
  if (values.empty()) {
    throw std::runtime_error("validation_metric_group_empty");
  }
  double sum = 0.0;
  for (const auto value : values) {
    sum += value;
  }
  return sum / static_cast<double>(values.size());
}

template <typename Value> Value nearest_rank_p95(std::vector<Value> values) {
  if (values.empty()) {
    throw std::runtime_error("validation_p95_group_empty");
  }
  std::sort(values.begin(), values.end());
  const auto rank = (95U * values.size() + 99U) / 100U;
  return values[rank - 1U];
}

bool complete_lifecycle(glyphrelay::CudaPreprocessor &pipeline,
                        const glyphrelay::CudaPreprocessTicket &ticket) {
  return pipeline.mark_submitted(ticket).passed &&
         pipeline.mark_encoder_input_released(ticket).passed && pipeline.release(ticket).passed;
}

Evaluation evaluate(const Arguments &arguments, std::vector<SequenceData> &sequences) {
  glyphrelay::CudaPreprocessor pipeline(0, kWidth, kHeight, 3U, 3U, arguments.configuration);
  if (!pipeline.available()) {
    throw std::runtime_error(pipeline.reason());
  }
  std::vector<std::pair<std::size_t, glyphrelay::SaliencyMapMetrics>> sequence_metrics;
  std::vector<std::uint64_t> processing_samples;
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
        throw std::runtime_error(ticket.reason);
      }
      const auto completion = pipeline.wait(ticket);
      if (!completion.passed) {
        throw std::runtime_error(completion.reason);
      }
      if (measure) {
        const auto observed = metrics.observe({completion.emphasis_map.values, frame.glyph_truth,
                                               frame.small_glyph_truth, frame.ui_truth});
        if (!observed.passed) {
          throw std::runtime_error(observed.reason);
        }
        processing_samples.push_back(completion.timings.total_pipeline);
      }
      if (!complete_lifecycle(pipeline, ticket)) {
        throw std::runtime_error("validation_cuda_lifecycle_failed");
      }
    };
    for (std::size_t warmup = 0U; warmup < 5U; ++warmup) {
      process(sequence.frames[0], warmup == 4U);
      runtime_timestamp_ns += kFrameIntervalNs;
    }
    for (std::size_t frame = 1U; frame < kFrameCount; ++frame) {
      runtime_timestamp_ns += kSampleIntervalNs;
      process(sequence.frames[frame], true);
    }
    runtime_timestamp_ns += kSampleIntervalNs;
    if (metrics.observed_frames() != kFrameCount) {
      throw std::runtime_error("validation_sequence_observation_count_invalid");
    }
    sequence_metrics.emplace_back(sequence.stratum, metrics.finalize());
  }
  pipeline.close_admission();
  if (!pipeline.all_free()) {
    throw std::runtime_error("validation_cuda_resources_not_drained");
  }
  Evaluation output;
  output.per_sequence.reserve(sequence_metrics.size());
  for (const auto &[unused, metrics] : sequence_metrics) {
    static_cast<void>(unused);
    output.per_sequence.push_back({metrics.overall_glyph_recall, metrics.small_glyph_recall,
                                   metrics.protected_fraction, metrics.false_protected_fraction,
                                   metrics.false_discovery_fraction,
                                   metrics.static_map_change_fraction});
  }
  for (std::size_t metric = 0U; metric < 6U; ++metric) {
    std::vector<double> corpus_sequences;
    std::array<std::vector<double>, kStratumCount> per_stratum;
    for (const auto &[stratum, value] : sequence_metrics) {
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
    throw std::runtime_error("validation_processing_p95_zero");
  }
  return output;
}

void write_metric_set(std::ostringstream &output, const MetricSet &metrics) {
  output << std::setprecision(17)
         << "{\"falseDiscoveryFraction\":" << metrics.false_discovery_fraction
         << ",\"falseProtectedFraction\":" << metrics.false_protected_fraction
         << ",\"overallGlyphRecall\":" << metrics.overall_glyph_recall
         << ",\"protectedFraction\":" << metrics.protected_fraction
         << ",\"protectedTruthPrecision\":" << 1.0 - metrics.false_discovery_fraction
         << ",\"smallGlyphRecall\":" << metrics.small_glyph_recall
         << ",\"staticMapChangeFraction\":" << metrics.static_map_change_fraction << '}';
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

std::string evidence_json(const Arguments &arguments, const std::vector<SequenceData> &sequences,
                          const Evaluation &evaluation) {
  const bool passed = evaluation.aggregate.overall_glyph_recall >= 0.90 &&
                      evaluation.aggregate.small_glyph_recall >= 0.80 &&
                      evaluation.aggregate.protected_fraction <= 0.35 &&
                      evaluation.aggregate.false_protected_fraction <= 0.15 &&
                      evaluation.aggregate.static_map_change_fraction <= 0.02;
  std::ostringstream output;
  output << "{\"automaticMapImplementationSha256\":\""
         << arguments.automatic_map_implementation_sha256 << "\",\"bundleSha256\":\""
         << arguments.bundle_sha256 << "\",\"configuration\":";
  write_configuration(output, arguments.configuration);
  output << ",\"configurationSha256\":\"" << arguments.configuration_sha256
         << "\",\"corpusProtocolSha256\":\"" << arguments.corpus_protocol_sha256
         << "\",\"metrics\":";
  output << '{';
  output << "\"falseDiscoveryFraction\":" << std::setprecision(17)
         << evaluation.aggregate.false_discovery_fraction
         << ",\"falseProtectedFraction\":" << evaluation.aggregate.false_protected_fraction
         << ",\"overallGlyphRecall\":" << evaluation.aggregate.overall_glyph_recall
         << ",\"p95Sequence\":";
  write_metric_set(output, evaluation.p95_sequence);
  output << ",\"perSequence\":[";
  for (std::size_t index = 0U; index < sequences.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    output << "{\"metrics\":";
    write_metric_set(output, evaluation.per_sequence[index]);
    output << ",\"sequenceId\":\"" << sequences[index].identifier << "\",\"stratum\":\""
           << kStrata[sequences[index].stratum] << "\"}";
  }
  output << "],\"perStratum\":{";
  for (std::size_t stratum = 0U; stratum < kStratumCount; ++stratum) {
    if (stratum != 0U) {
      output << ',';
    }
    output << '\"' << kStrata[stratum] << "\":";
    write_metric_set(output, evaluation.per_stratum[stratum]);
  }
  output << "},\"processingP95Ns\":" << evaluation.processing_p95_ns
         << ",\"protectedFraction\":" << evaluation.aggregate.protected_fraction
         << ",\"protectedTruthPrecision\":" << 1.0 - evaluation.aggregate.false_discovery_fraction
         << ",\"smallGlyphRecall\":" << evaluation.aggregate.small_glyph_recall
         << ",\"staticMapChangeFraction\":" << evaluation.aggregate.static_map_change_fraction
         << "},\"processingPlatformSha256\":\"" << arguments.processing_platform_sha256
         << "\",\"protocol\":\"saliency_validation_v1\",\"schemaVersion\":1,\"sourceBundleId\":\""
         << arguments.source_bundle_id << "\",\"split\":\"validation\",\"status\":\""
         << (passed ? "PASSED" : "INSUFFICIENT_EVIDENCE")
         << "\",\"thresholds\":{\"falseProtectedFractionMaximum\":0.15,"
            "\"overallGlyphRecallMinimum\":0.9,\"protectedFractionMaximum\":0.35,"
            "\"smallGlyphRecallMinimum\":0.8,\"staticMapChangeFractionMaximum\":0.02},"
            "\"validationManifestSha256\":\""
         << arguments.validation_manifest_sha256 << "\",\"validationRenderIndexSha256\":\""
         << arguments.validation_render_index_sha256 << "\"}\n";
  return output.str();
}

double parse_double(std::string_view text, const char *label) {
  std::size_t consumed = 0U;
  const auto value = std::stod(std::string(text), &consumed);
  if (consumed != text.size() || !std::isfinite(value)) {
    throw std::runtime_error(std::string(label) + "_invalid");
  }
  return value;
}

std::size_t parse_size(std::string_view text, const char *label) {
  std::size_t consumed = 0U;
  const auto value = std::stoull(std::string(text), &consumed);
  if (consumed != text.size() || value > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error(std::string(label) + "_invalid");
  }
  return static_cast<std::size_t>(value);
}

Arguments parse_arguments(int argc, char **argv) {
  Arguments arguments;
  std::vector<std::string> seen;
  auto value = [&](int &index) -> std::string_view {
    if (++index >= argc) {
      throw std::runtime_error("validation_argument_value_missing");
    }
    return argv[index];
  };
  for (int index = 1; index < argc; ++index) {
    const std::string_view name = argv[index];
    if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
      throw std::runtime_error("validation_argument_duplicate");
    }
    seen.emplace_back(name);
    if (name == "--bundle") {
      arguments.bundle = value(index);
    } else if (name == "--bundle-sha256") {
      arguments.bundle_sha256 = value(index);
    } else if (name == "--output") {
      arguments.output = value(index);
    } else if (name == "--source-bundle-id") {
      arguments.source_bundle_id = value(index);
    } else if (name == "--automatic-map-implementation-sha256") {
      arguments.automatic_map_implementation_sha256 = value(index);
    } else if (name == "--processing-platform-sha256") {
      arguments.processing_platform_sha256 = value(index);
    } else if (name == "--corpus-protocol-sha256") {
      arguments.corpus_protocol_sha256 = value(index);
    } else if (name == "--validation-manifest-sha256") {
      arguments.validation_manifest_sha256 = value(index);
    } else if (name == "--validation-render-index-sha256") {
      arguments.validation_render_index_sha256 = value(index);
    } else if (name == "--configuration-sha256") {
      arguments.configuration_sha256 = value(index);
    } else if (name == "--gradient-weight") {
      arguments.configuration.gradient_weight = parse_double(value(index), "gradient_weight");
    } else if (name == "--contrast-weight") {
      arguments.configuration.contrast_weight = parse_double(value(index), "contrast_weight");
    } else if (name == "--edge-pair-weight") {
      arguments.configuration.edge_pair_weight = parse_double(value(index), "edge_pair_weight");
    } else if (name == "--small-structure-weight") {
      arguments.configuration.small_structure_weight =
          parse_double(value(index), "small_structure_weight");
    } else if (name == "--entry-threshold") {
      arguments.configuration.entry_threshold = parse_double(value(index), "entry_threshold");
    } else if (name == "--exit-threshold") {
      arguments.configuration.exit_threshold = parse_double(value(index), "exit_threshold");
    } else if (name == "--previous-score-coefficient") {
      arguments.configuration.previous_score_coefficient =
          parse_double(value(index), "previous_score_coefficient");
    } else if (name == "--dilation-radius-tiles") {
      arguments.configuration.dilation_radius_tiles =
          parse_size(value(index), "dilation_radius_tiles");
    } else {
      throw std::runtime_error("validation_argument_unknown");
    }
  }
  constexpr std::array required = {
      "--bundle",
      "--bundle-sha256",
      "--output",
      "--source-bundle-id",
      "--automatic-map-implementation-sha256",
      "--processing-platform-sha256",
      "--corpus-protocol-sha256",
      "--validation-manifest-sha256",
      "--validation-render-index-sha256",
      "--configuration-sha256",
      "--gradient-weight",
      "--contrast-weight",
      "--edge-pair-weight",
      "--small-structure-weight",
      "--entry-threshold",
      "--exit-threshold",
      "--previous-score-coefficient",
      "--dilation-radius-tiles",
  };
  if (std::any_of(required.begin(), required.end(), [&](std::string_view name) {
        return std::find(seen.begin(), seen.end(), name) == seen.end();
      })) {
    throw std::runtime_error("validation_argument_required_missing");
  }
  for (const auto &hash :
       {arguments.bundle_sha256, arguments.source_bundle_id,
        arguments.automatic_map_implementation_sha256, arguments.processing_platform_sha256,
        arguments.corpus_protocol_sha256, arguments.validation_manifest_sha256,
        arguments.validation_render_index_sha256, arguments.configuration_sha256}) {
    if (!hex_sha256(hash)) {
      throw std::runtime_error("validation_identity_invalid");
    }
  }
  if (arguments.bundle.empty() || arguments.output.empty() ||
      !glyphrelay::valid_saliency_configuration(arguments.configuration)) {
    throw std::runtime_error("validation_arguments_invalid");
  }
  return arguments;
}

void write_exclusive(const std::filesystem::path &path, std::string_view value) {
  const auto temporary = path.parent_path() / ("." + path.filename().string() + "." +
                                               std::to_string(::getpid()) + ".tmp");
  const int descriptor =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw std::runtime_error(std::string("validation_output_open_failed:") + std::strerror(errno));
  }
  std::size_t offset = 0U;
  while (offset < value.size()) {
    const auto written = ::write(descriptor, value.data() + offset, value.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      ::close(descriptor);
      ::unlink(temporary.c_str());
      throw std::runtime_error("validation_output_write_failed");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0 || ::close(descriptor) != 0) {
    ::unlink(temporary.c_str());
    throw std::runtime_error("validation_output_sync_failed");
  }
  if (::link(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    throw std::runtime_error(std::string("validation_output_publish_failed:") +
                             std::strerror(errno));
  }
  if (::unlink(temporary.c_str()) != 0) {
    throw std::runtime_error("validation_output_temporary_cleanup_failed");
  }
  const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory < 0 || ::fsync(directory) != 0 || ::close(directory) != 0) {
    throw std::runtime_error("validation_output_directory_sync_failed");
  }
}

void print_usage() {
  std::cout << "usage: glyphrelay_saliency_validation --bundle FILE --bundle-sha256 HEX "
               "--output FILE --source-bundle-id HEX --automatic-map-implementation-sha256 HEX "
               "--processing-platform-sha256 HEX --corpus-protocol-sha256 HEX "
               "--validation-manifest-sha256 HEX --validation-render-index-sha256 HEX "
               "--configuration-sha256 HEX --gradient-weight N --contrast-weight N "
               "--edge-pair-weight N --small-structure-weight N --entry-threshold N "
               "--exit-threshold N --previous-score-coefficient N --dilation-radius-tiles N\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    print_usage();
    return 0;
  }
  try {
    const auto arguments = parse_arguments(argc, argv);
    auto sequences = load_bundle(arguments);
    const auto evaluation = evaluate(arguments, sequences);
    write_exclusive(arguments.output, evidence_json(arguments, sequences, evaluation));
    std::cout << "saliency validation map gate: "
              << (evaluation.aggregate.overall_glyph_recall >= 0.90 &&
                          evaluation.aggregate.small_glyph_recall >= 0.80 &&
                          evaluation.aggregate.protected_fraction <= 0.35 &&
                          evaluation.aggregate.false_protected_fraction <= 0.15 &&
                          evaluation.aggregate.static_map_change_fraction <= 0.02
                      ? "PASSED"
                      : "INSUFFICIENT_EVIDENCE")
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "saliency validation failed: " << error.what() << '\n';
    return 1;
  }
}
