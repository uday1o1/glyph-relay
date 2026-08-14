#include "glyphrelay/color_conversion.hpp"
#include "glyphrelay/cuda_preprocess.hpp"
#include "glyphrelay/saliency.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct QualifiedFrame {
  std::vector<std::uint8_t> nv12;
  std::size_t nv12_pitch = 0U;
  std::vector<std::int8_t> map;
  std::vector<glyphrelay::TileFeatureVector> features;
};

struct CorrectnessEvidence {
  bool passed = false;
  std::string reason;
  std::size_t frame_count = 0U;
  std::size_t compared_luma_chroma_codes = 0U;
  std::size_t compared_features = 0U;
  unsigned int maximum_code_error = 0U;
  double maximum_feature_error = 0.0;
  bool deterministic = false;
};

struct PerformanceEvidence {
  bool passed = false;
  std::string reason;
  std::size_t warmup_frames = 0U;
  std::size_t measured_frames = 0U;
  std::uint64_t measured_wall_duration_ns = 0U;
  glyphrelay::CudaPreprocessTimingsNs p95;
};

void usage() {
  std::cout << "usage: glyphrelay_cuda_saliency_qualify "
               "(--correctness|--performance) --output FILE.json\n";
}

glyphrelay::CapturedFrame make_frame(std::size_t width, std::size_t height, std::size_t extra_pitch,
                                     std::uint64_t frame_id, std::uint64_t geometry_epoch,
                                     std::uint64_t timestamp_ns, glyphrelay::PackedPixelOrder order,
                                     std::uint32_t seed) {
  glyphrelay::CapturedFrame frame;
  frame.frame_id = frame_id;
  frame.monotonic_timestamp_ns = timestamp_ns;
  frame.geometry.epoch = geometry_epoch;
  frame.geometry.source_width = width;
  frame.geometry.source_height = height;
  frame.geometry.source_crop = {0U, 0U, width, height};
  frame.geometry.visible_width = width;
  frame.geometry.visible_height = height;
  frame.geometry.coded_width = (width + 1U) & ~std::size_t{1U};
  frame.geometry.coded_height = (height + 1U) & ~std::size_t{1U};
  frame.pixel_order = order;
  frame.pitch = width * 4U + extra_pitch;
  frame.pixels.assign(frame.pitch * height, 0xCDU);
  std::mt19937 random(seed);
  std::uniform_int_distribution<unsigned int> byte(0U, 255U);
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      const auto offset = y * frame.pitch + x * 4U;
      frame.pixels[offset] = static_cast<std::uint8_t>(byte(random));
      frame.pixels[offset + 1U] = static_cast<std::uint8_t>(byte(random));
      frame.pixels[offset + 2U] = static_cast<std::uint8_t>(byte(random));
      frame.pixels[offset + 3U] = static_cast<std::uint8_t>(byte(random));
    }
  }
  return frame;
}

bool complete_lifecycle(glyphrelay::CudaPreprocessor &pipeline,
                        const glyphrelay::CudaPreprocessTicket &ticket, std::string &reason) {
  const auto submitted = pipeline.mark_submitted(ticket);
  const auto released = pipeline.mark_encoder_input_released(ticket);
  const auto recycled = pipeline.release(ticket);
  if (!submitted.passed || !released.passed || !recycled.passed) {
    reason = "cuda_qualification_lifecycle_failed";
    return false;
  }
  return true;
}

double feature_error(const glyphrelay::TileFeatureVector &left,
                     const glyphrelay::TileFeatureVector &right) {
  const std::array errors = {
      std::abs(left.gradient_density - right.gradient_density),
      std::abs(left.local_contrast - right.local_contrast),
      std::abs(left.edge_pair_density - right.edge_pair_density),
      std::abs(left.small_structure_density - right.small_structure_density),
      std::abs(left.raw_score - right.raw_score),
      std::abs(left.temporal_stability - right.temporal_stability),
      std::abs(left.current_score - right.current_score),
      std::abs(left.filtered_score - right.filtered_score),
  };
  return *std::max_element(errors.begin(), errors.end());
}

bool compare_nv12(const glyphrelay::Nv12Image &reference,
                  const glyphrelay::CudaPreprocessCompletion &candidate,
                  unsigned int &maximum_error, std::size_t &compared_codes) {
  if (candidate.surface.coded_width != reference.coded_width ||
      candidate.surface.coded_height != reference.coded_height ||
      candidate.debug_nv12.size() <
          candidate.surface.pitch * candidate.surface.coded_height * 3U / 2U) {
    return false;
  }
  const auto gpu_chroma = candidate.surface.pitch * candidate.surface.coded_height;
  for (std::size_t y = 0U; y < reference.coded_height; ++y) {
    for (std::size_t x = 0U; x < reference.coded_width; ++x) {
      const auto left = reference.bytes[y * reference.pitch + x];
      const auto right = candidate.debug_nv12[y * candidate.surface.pitch + x];
      const auto error = static_cast<unsigned int>(left > right ? left - right : right - left);
      maximum_error = std::max(maximum_error, error);
      ++compared_codes;
    }
  }
  for (std::size_t y = 0U; y < reference.coded_height / 2U; ++y) {
    for (std::size_t x = 0U; x < reference.coded_width; ++x) {
      const auto left = reference.bytes[reference.chroma_offset + y * reference.pitch + x];
      const auto right = candidate.debug_nv12[gpu_chroma + y * candidate.surface.pitch + x];
      const auto error = static_cast<unsigned int>(left > right ? left - right : right - left);
      maximum_error = std::max(maximum_error, error);
      ++compared_codes;
    }
  }
  return true;
}

std::vector<std::pair<glyphrelay::CapturedFrame, glyphrelay::ColorRange>> correctness_frames() {
  std::vector<std::pair<glyphrelay::CapturedFrame, glyphrelay::ColorRange>> frames;
  constexpr std::array<std::pair<std::size_t, std::size_t>, 8U> boundaries = {
      std::pair{1U, 1U},   std::pair{7U, 9U},   std::pair{8U, 8U},   std::pair{15U, 17U},
      std::pair{16U, 16U}, std::pair{17U, 17U}, std::pair{31U, 18U}, std::pair{63U, 37U},
  };
  std::uint64_t frame_id = 1U;
  std::uint64_t epoch = 1U;
  for (std::size_t index = 0U; index < boundaries.size(); ++index) {
    const auto [width, height] = boundaries[index];
    const auto timestamp = frame_id * 33'333'333U;
    frames.emplace_back(make_frame(width, height, index % 5U, frame_id++, epoch++, timestamp,
                                   index % 2U == 0U ? glyphrelay::PackedPixelOrder::bgra
                                                    : glyphrelay::PackedPixelOrder::rgba,
                                   static_cast<std::uint32_t>(0x43554441U + index)),
                        index % 2U == 0U ? glyphrelay::ColorRange::limited
                                         : glyphrelay::ColorRange::full);
  }
  const auto temporal_epoch = epoch;
  for (std::size_t index = 0U; index < 20U; ++index) {
    const auto gap = index == 11U ? 250'000'000U : 33'333'333U;
    const auto timestamp = frames.back().first.monotonic_timestamp_ns + gap;
    frames.emplace_back(make_frame(63U, 37U, 7U, frame_id++, temporal_epoch, timestamp,
                                   glyphrelay::PackedPixelOrder::bgra,
                                   static_cast<std::uint32_t>(0x53414C31U + index)),
                        glyphrelay::ColorRange::limited);
  }
  frames.emplace_back(make_frame(1920U, 1080U, 0U, frame_id++, epoch + 1U,
                                 frames.back().first.monotonic_timestamp_ns + 33'333'333U,
                                 glyphrelay::PackedPixelOrder::rgba, 0x108030U),
                      glyphrelay::ColorRange::full);
  return frames;
}

bool qualify_sequence(
    const std::vector<std::pair<glyphrelay::CapturedFrame, glyphrelay::ColorRange>> &frames,
    std::vector<QualifiedFrame> &qualified, CorrectnessEvidence &evidence) {
  glyphrelay::CudaPreprocessor pipeline(0, 1920U, 1080U);
  if (!pipeline.available()) {
    evidence.reason = pipeline.reason();
    return false;
  }
  glyphrelay::SaliencyReference reference;
  qualified.clear();
  qualified.reserve(frames.size());
  for (std::size_t index = 0U; index < frames.size(); ++index) {
    const auto &[frame, range] = frames[index];
    glyphrelay::SaliencyProcessOptions options;
    if (index % 7U == 3U) {
      options.overrides.pins = {{1U, 1U, 9U, 9U}};
      options.overrides.cursor_halos = {
          {frame.geometry.visible_width / 2U, frame.geometry.visible_height / 2U, 3U, 3U}};
    }
    if (index % 11U == 5U) {
      options.overrides.exclusions = {{0U, 0U, 4U, 4U}};
    }
    const auto cpu_color = glyphrelay::convert_bgra_or_rgba_to_nv12(
        frame, range, glyphrelay::ColorConversionBackend::scalar);
    if (!cpu_color.passed) {
      evidence.reason = cpu_color.reason;
      return false;
    }
    const glyphrelay::LumaPlaneView luma{
        .codes = cpu_color.image.bytes,
        .width = frame.geometry.visible_width,
        .height = frame.geometry.visible_height,
        .pitch = cpu_color.image.pitch,
        .range = range,
        .frame_id = frame.frame_id,
        .geometry_epoch = frame.geometry.epoch,
        .monotonic_timestamp_ns = frame.monotonic_timestamp_ns,
    };
    const auto cpu_saliency = reference.process(luma, options);
    const auto ticket = pipeline.enqueue(frame, range, options, true);
    if (!cpu_saliency.passed || !ticket.passed) {
      evidence.reason = cpu_saliency.passed ? ticket.reason : cpu_saliency.reason;
      return false;
    }
    const auto gpu = pipeline.wait(ticket);
    if (!gpu.passed) {
      evidence.reason = gpu.reason;
      return false;
    }
    if (!compare_nv12(cpu_color.image, gpu, evidence.maximum_code_error,
                      evidence.compared_luma_chroma_codes) ||
        evidence.maximum_code_error > 1U ||
        gpu.emphasis_map.values.size() != cpu_saliency.output.macroblock_levels.size() ||
        !std::equal(gpu.emphasis_map.values.begin(), gpu.emphasis_map.values.end(),
                    cpu_saliency.output.macroblock_levels.begin()) ||
        gpu.debug_tiles.size() != cpu_saliency.output.tiles.size()) {
      evidence.reason = "cuda_qualification_output_mismatch";
      return false;
    }
    for (std::size_t tile = 0U; tile < gpu.debug_tiles.size(); ++tile) {
      evidence.maximum_feature_error =
          std::max(evidence.maximum_feature_error,
                   feature_error(gpu.debug_tiles[tile], cpu_saliency.output.tiles[tile]));
      ++evidence.compared_features;
      if (gpu.debug_tiles[tile].active != cpu_saliency.output.tiles[tile].active ||
          gpu.debug_tiles[tile].automatic_level !=
              cpu_saliency.output.tiles[tile].automatic_level ||
          gpu.debug_tiles[tile].final_level != cpu_saliency.output.tiles[tile].final_level) {
        evidence.reason = "cuda_qualification_tile_state_mismatch";
        return false;
      }
    }
    if (evidence.maximum_feature_error > 1e-10) {
      evidence.reason = "cuda_qualification_feature_error_exceeded";
      return false;
    }
    const glyphrelay::NvencSubmissionRequest validation_request{
        .submission_slot_id = ticket.token.surface_slot,
        .submission_sequence = frame.frame_id,
        .output_bitstream = 1U,
        .surface = gpu.surface,
        .emphasis_map = gpu.emphasis_map,
    };
    if (!glyphrelay::validate_nvenc_submission(validation_request).passed) {
      evidence.reason = "cuda_qualification_submission_descriptor_invalid";
      return false;
    }
    QualifiedFrame captured;
    captured.nv12 = gpu.debug_nv12;
    captured.nv12_pitch = gpu.surface.pitch;
    captured.map.assign(gpu.emphasis_map.values.begin(), gpu.emphasis_map.values.end());
    captured.features = gpu.debug_tiles;
    qualified.push_back(std::move(captured));
    if (!complete_lifecycle(pipeline, ticket, evidence.reason)) {
      return false;
    }
    ++evidence.frame_count;
  }
  pipeline.close_admission();
  if (!pipeline.all_free()) {
    evidence.reason = "cuda_qualification_resources_not_drained";
    return false;
  }
  return true;
}

CorrectnessEvidence run_correctness() {
  CorrectnessEvidence evidence;
  const auto frames = correctness_frames();
  std::vector<QualifiedFrame> first;
  std::vector<QualifiedFrame> second;
  if (!qualify_sequence(frames, first, evidence)) {
    return evidence;
  }
  CorrectnessEvidence replay_evidence;
  if (!qualify_sequence(frames, second, replay_evidence)) {
    evidence.reason = "cuda_qualification_replay_failed:" + replay_evidence.reason;
    return evidence;
  }
  if (first.size() != second.size()) {
    evidence.reason = "cuda_qualification_replay_count_mismatch";
    return evidence;
  }
  for (std::size_t frame = 0U; frame < first.size(); ++frame) {
    if (first[frame].nv12 != second[frame].nv12 || first[frame].map != second[frame].map ||
        first[frame].features.size() != second[frame].features.size()) {
      evidence.reason = "cuda_qualification_replay_bytes_mismatch";
      return evidence;
    }
    for (std::size_t tile = 0U; tile < first[frame].features.size(); ++tile) {
      if (feature_error(first[frame].features[tile], second[frame].features[tile]) != 0.0 ||
          first[frame].features[tile].active != second[frame].features[tile].active ||
          first[frame].features[tile].automatic_level !=
              second[frame].features[tile].automatic_level ||
          first[frame].features[tile].final_level != second[frame].features[tile].final_level) {
        evidence.reason = "cuda_qualification_replay_feature_mismatch";
        return evidence;
      }
    }
  }
  evidence.deterministic = true;
  evidence.passed = true;
  evidence.reason = "cuda_saliency_correctness_passed";
  return evidence;
}

template <typename Field>
std::uint64_t p95(const std::vector<glyphrelay::CudaPreprocessTimingsNs> &samples, Field field) {
  std::vector<std::uint64_t> values;
  values.reserve(samples.size());
  for (const auto &sample : samples) {
    values.push_back(sample.*field);
  }
  std::sort(values.begin(), values.end());
  const auto rank = (95U * values.size() + 99U) / 100U;
  return values[rank - 1U];
}

PerformanceEvidence run_performance() {
  PerformanceEvidence evidence;
  evidence.warmup_frames = 300U;
  constexpr std::size_t minimum_measured_frames = 1'800U;
  constexpr auto minimum_measured_duration = std::chrono::seconds(10);
  glyphrelay::CudaPreprocessor pipeline(0, 1920U, 1080U);
  if (!pipeline.available()) {
    evidence.reason = pipeline.reason();
    return evidence;
  }
  auto frame = make_frame(1920U, 1080U, 0U, 1U, 1U, 33'333'333U, glyphrelay::PackedPixelOrder::bgra,
                          0x50455246U);
  std::vector<glyphrelay::CudaPreprocessTimingsNs> samples;
  samples.reserve(minimum_measured_frames);
  auto process_frame = [&](std::size_t index, bool measured) {
    frame.frame_id = index + 1U;
    frame.monotonic_timestamp_ns = (index + 1U) * 33'333'333U;
    const auto ticket = pipeline.enqueue(frame, glyphrelay::ColorRange::limited);
    if (!ticket.passed) {
      evidence.reason = ticket.reason;
      return false;
    }
    const auto completion = pipeline.wait(ticket);
    if (!completion.passed) {
      evidence.reason = completion.reason;
      return false;
    }
    if (measured) {
      samples.push_back(completion.timings);
    }
    if (!complete_lifecycle(pipeline, ticket, evidence.reason)) {
      return false;
    }
    return true;
  };
  for (std::size_t index = 0U; index < evidence.warmup_frames; ++index) {
    if (!process_frame(index, false)) {
      return evidence;
    }
  }
  const auto measured_started = std::chrono::steady_clock::now();
  while (evidence.measured_frames < minimum_measured_frames ||
         std::chrono::steady_clock::now() - measured_started < minimum_measured_duration) {
    if (!process_frame(evidence.warmup_frames + evidence.measured_frames, true)) {
      return evidence;
    }
    ++evidence.measured_frames;
  }
  evidence.measured_wall_duration_ns =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - measured_started)
                                     .count());
  pipeline.close_admission();
  if (samples.empty() || !pipeline.all_free()) {
    evidence.reason = "cuda_performance_samples_or_cleanup_invalid";
    return evidence;
  }
  evidence.p95 = {
      .input_upload = p95(samples, &glyphrelay::CudaPreprocessTimingsNs::input_upload),
      .color_conversion = p95(samples, &glyphrelay::CudaPreprocessTimingsNs::color_conversion),
      .feature_extraction = p95(samples, &glyphrelay::CudaPreprocessTimingsNs::feature_extraction),
      .temporal_hysteresis =
          p95(samples, &glyphrelay::CudaPreprocessTimingsNs::temporal_hysteresis),
      .morphology_and_overrides =
          p95(samples, &glyphrelay::CudaPreprocessTimingsNs::morphology_and_overrides),
      .macroblock_reduction =
          p95(samples, &glyphrelay::CudaPreprocessTimingsNs::macroblock_reduction),
      .host_map_copy = p95(samples, &glyphrelay::CudaPreprocessTimingsNs::host_map_copy),
      .total_pipeline = p95(samples, &glyphrelay::CudaPreprocessTimingsNs::total_pipeline),
  };
  evidence.passed = evidence.p95.total_pipeline <= 5'000'000U;
  evidence.reason =
      evidence.passed ? "cuda_saliency_performance_passed" : "cuda_saliency_p95_exceeded_5ms";
  return evidence;
}

bool write_open_file(std::FILE *file, std::string_view text) {
  bool passed = std::fwrite(text.data(), 1U, text.size(), file) == text.size();
  passed = std::fflush(file) == 0 && passed;
  passed = std::fclose(file) == 0 && passed;
  return passed;
}

std::string correctness_json(const CorrectnessEvidence &evidence) {
  std::ostringstream output;
  output << std::setprecision(17) << "{\"schemaVersion\":1,\"protocol\":\"saliency_v1\","
         << "\"mode\":\"correctness\",\"status\":\"" << (evidence.passed ? "PASSED" : "FAILED")
         << "\",\"reason\":\"" << evidence.reason << "\",\"frameCount\":" << evidence.frame_count
         << ",\"comparedLumaChromaCodes\":" << evidence.compared_luma_chroma_codes
         << ",\"comparedFeatures\":" << evidence.compared_features
         << ",\"maximumCodeError\":" << evidence.maximum_code_error
         << ",\"maximumFeatureError\":" << evidence.maximum_feature_error
         << ",\"deterministic\":" << (evidence.deterministic ? "true" : "false") << "}\n";
  return output.str();
}

std::string performance_json(const PerformanceEvidence &evidence) {
  const auto &timings = evidence.p95;
  std::ostringstream output;
  output << "{\"schemaVersion\":1,\"protocol\":\"saliency_v1\","
         << "\"mode\":\"performance\",\"status\":\"" << (evidence.passed ? "PASSED" : "FAILED")
         << "\",\"reason\":\"" << evidence.reason
         << "\",\"warmupFrames\":" << evidence.warmup_frames
         << ",\"measuredFrames\":" << evidence.measured_frames
         << ",\"measuredWallDurationNs\":" << evidence.measured_wall_duration_ns << ",\"p95Ns\":{"
         << "\"inputUpload\":" << timings.input_upload
         << ",\"colorConversion\":" << timings.color_conversion
         << ",\"featureExtraction\":" << timings.feature_extraction
         << ",\"temporalHysteresis\":" << timings.temporal_hysteresis
         << ",\"morphologyAndOverrides\":" << timings.morphology_and_overrides
         << ",\"macroblockReduction\":" << timings.macroblock_reduction
         << ",\"hostMapCopy\":" << timings.host_map_copy
         << ",\"totalPipeline\":" << timings.total_pipeline << "}}\n";
  return output.str();
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage();
    return 0;
  }
  if (argc != 4 ||
      (std::string_view(argv[1]) != "--correctness" &&
       std::string_view(argv[1]) != "--performance") ||
      std::string_view(argv[2]) != "--output") {
    usage();
    return 2;
  }
  const std::filesystem::path output_path(argv[3]);
  if (output_path.extension() != ".json" || !output_path.has_filename() ||
      !std::filesystem::is_directory(output_path.parent_path())) {
    std::cerr << "qualification output must be a .json file in an existing directory\n";
    return 2;
  }
  errno = 0;
  auto *output_file = std::fopen(output_path.string().c_str(), "wbx");
  if (output_file == nullptr) {
    std::cerr << "qualification output open failed: " << std::strerror(errno) << '\n';
    return errno == EEXIST ? 2 : 8;
  }
  const bool correctness = std::string_view(argv[1]) == "--correctness";
  const auto correctness_evidence = correctness ? run_correctness() : CorrectnessEvidence{};
  const auto performance_evidence = correctness ? PerformanceEvidence{} : run_performance();
  const auto passed = correctness ? correctness_evidence.passed : performance_evidence.passed;
  const auto json =
      correctness ? correctness_json(correctness_evidence) : performance_json(performance_evidence);
  if (!write_open_file(output_file, json)) {
    std::cerr << "qualification output write failed\n";
    return 8;
  }
  std::cout << json;
  return passed ? 0 : 8;
}
