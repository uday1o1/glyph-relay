#include "glyphrelay/capture.hpp"
#include "glyphrelay/cuda_context.hpp"
#include "glyphrelay/cuda_preprocess.hpp"
#include "glyphrelay/nvenc_encoder.hpp"
#include "glyphrelay/sha256.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 16U> kBundleMagic = {'G', 'L', 'Y', 'P', 'H', '_', 'S',  'A',
                                                        'L', '_', 'D', 'E', 'V', '1', '\0', '\0'};
constexpr std::uint32_t kBundleVersion = 1U;
constexpr std::size_t kWidth = 1920U;
constexpr std::size_t kHeight = 1080U;
constexpr std::size_t kSequenceCount = 64U;
constexpr std::size_t kSampleFrameCount = 4U;
constexpr std::size_t kFramesPerSequence = 240U;
constexpr std::size_t kSubmittedFrameCount = kSequenceCount * kFramesPerSequence;
constexpr std::size_t kMacroblockCount = 120U * 68U;
constexpr std::size_t kFrameBytes = kWidth * kHeight * 4U;
constexpr std::size_t kStratumCount = 7U;
constexpr std::size_t kCapacity = 4U;
constexpr std::size_t kWarmupFrames = 300U;
constexpr std::size_t kMeasurementFrames = 300U;
constexpr std::size_t kMeasurementEndFrame = kWarmupFrames + kMeasurementFrames;
constexpr std::size_t kPendingBuckets = 8U;
constexpr std::uint64_t kFrameIntervalNs = 33'333'333U;

struct Arguments {
  std::filesystem::path bundle;
  std::filesystem::path output;
  std::string bundle_sha256;
  std::string condition_id;
  std::string effective_encoder_fields_sha256;
  std::string corpus_protocol_sha256;
  std::string development_manifest_sha256;
  std::string development_render_index_sha256;
  std::string saliency_grid_sha256;
  std::uint32_t requested_payload_bps = 0U;
  bool enable_aq = false;
  std::uint32_t aq_strength = 0U;
  bool enable_temporal_aq = false;
};

struct SampleFrame {
  std::uint64_t source_frame_id = 0U;
  std::uint64_t source_pts_ns = 0U;
  glyphrelay::CapturedFrame captured;
};

struct Sequence {
  std::string identifier;
  std::size_t stratum = 0U;
  std::array<SampleFrame, kSampleFrameCount> frames;
};

class BinaryReader {
public:
  explicit BinaryReader(const std::filesystem::path &path) : input_(path, std::ios::binary) {
    if (!input_) {
      throw std::runtime_error("uniform_aq_bundle_open_failed");
    }
  }

  void read(std::span<std::uint8_t> output) {
    if (output.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
      throw std::runtime_error("uniform_aq_bundle_read_size_overflow");
    }
    input_.read(reinterpret_cast<char *>(output.data()),
                static_cast<std::streamsize>(output.size()));
    if (!input_) {
      throw std::runtime_error("uniform_aq_bundle_truncated");
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
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                      (static_cast<std::uint16_t>(bytes[1]) << 8U));
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

bool sha256_hex(std::string_view value) {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

bool safe_identifier(std::string_view value) {
  return !value.empty() && value.size() <= 96U &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '_' || character == '-';
         });
}

std::array<std::uint8_t, 32U> parse_sha256(std::string_view value) {
  if (!sha256_hex(value)) {
    throw std::runtime_error("uniform_aq_sha256_argument_invalid");
  }
  std::array<std::uint8_t, 32U> output{};
  const auto nibble = [](char character) {
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

std::vector<Sequence> load_bundle(const Arguments &arguments) {
  if (glyphrelay::sha256_file_hex(arguments.bundle) != arguments.bundle_sha256) {
    throw std::runtime_error("uniform_aq_bundle_sha256_mismatch");
  }
  BinaryReader reader(arguments.bundle);
  std::array<std::uint8_t, kBundleMagic.size()> magic{};
  reader.read(magic);
  if (magic != kBundleMagic || reader.u32() != kBundleVersion || reader.u32() != kWidth ||
      reader.u32() != kHeight || reader.u32() != kSequenceCount ||
      reader.u32() != kSampleFrameCount || reader.u32() != kMacroblockCount ||
      reader.u32() != kFrameBytes) {
    throw std::runtime_error("uniform_aq_bundle_header_invalid");
  }
  require_bundle_sha(reader, arguments.corpus_protocol_sha256,
                     "uniform_aq_bundle_corpus_identity_mismatch");
  require_bundle_sha(reader, arguments.development_manifest_sha256,
                     "uniform_aq_bundle_manifest_identity_mismatch");
  require_bundle_sha(reader, arguments.development_render_index_sha256,
                     "uniform_aq_bundle_render_identity_mismatch");
  require_bundle_sha(reader, arguments.saliency_grid_sha256,
                     "uniform_aq_bundle_saliency_grid_identity_mismatch");

  std::vector<Sequence> sequences(kSequenceCount);
  std::array<std::size_t, kStratumCount> stratum_counts{};
  std::array<std::uint8_t, kMacroblockCount> discarded_mask{};
  for (auto &sequence : sequences) {
    const auto identifier_size = reader.u16();
    sequence.stratum = reader.u8();
    const auto static_value = reader.u8();
    if (identifier_size == 0U || identifier_size > 96U || sequence.stratum >= kStratumCount ||
        static_value > 1U) {
      throw std::runtime_error("uniform_aq_bundle_sequence_header_invalid");
    }
    sequence.identifier = reader.text(identifier_size);
    if (!safe_identifier(sequence.identifier)) {
      throw std::runtime_error("uniform_aq_bundle_sequence_identifier_invalid");
    }
    ++stratum_counts[sequence.stratum];
    for (std::size_t frame_index = 0U; frame_index < sequence.frames.size(); ++frame_index) {
      auto &frame = sequence.frames[frame_index];
      frame.source_frame_id = reader.u64();
      frame.source_pts_ns = reader.u64();
      if (frame.source_frame_id != frame_index * 60U ||
          frame.source_pts_ns != frame.source_frame_id * kFrameIntervalNs) {
        throw std::runtime_error("uniform_aq_bundle_sample_identity_invalid");
      }
      std::array<std::uint8_t, 32U> discarded_png_sha256{};
      reader.read(discarded_png_sha256);
      reader.read(discarded_mask);
      reader.read(discarded_mask);
      reader.read(discarded_mask);
      frame.captured.pixels.resize(kFrameBytes);
      reader.read(frame.captured.pixels);
    }
  }
  if (!reader.exhausted() || std::any_of(stratum_counts.begin(), stratum_counts.end(),
                                         [](std::size_t count) { return count < 8U; })) {
    throw std::runtime_error("uniform_aq_bundle_sequence_coverage_invalid");
  }
  return sequences;
}

std::uint32_t parse_u32(std::string_view value, const char *reason) {
  std::uint32_t parsed = 0U;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    throw std::runtime_error(reason);
  }
  return parsed;
}

bool parse_bool(std::string_view value, const char *reason) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  throw std::runtime_error(reason);
}

std::string effective_encoder_fields_sha256(const Arguments &arguments) {
  std::ostringstream value;
  value << "{\"aqFields\":{\"aqStrength\":" << arguments.aq_strength
        << ",\"enableAQ\":" << (arguments.enable_aq ? "true" : "false")
        << ",\"enableTemporalAQ\":" << (arguments.enable_temporal_aq ? "true" : "false")
        << "},\"bFrames\":0,\"capacity\":4,\"chromaFormatIdc\":1,"
           "\"entropyCoding\":\"cavlc\",\"fillerDataInsertion\":true,\"frameRate\":30,"
           "\"gopFrames\":60,\"height\":1080,\"inputBitDepth\":8,\"levelIdc\":40,"
           "\"lookahead\":false,\"maximumBusyRetries\":100,\"maximumReferenceFrames\":1,"
           "\"mode\":\"uniform\",\"multipass\":\"disabled\",\"outputBitDepth\":8,"
           "\"preset\":\"p4\",\"profile\":\"baseline\","
           "\"rateControl\":\"cbr_development_search_variable\",\"repeatSpsPps\":true,"
           "\"temporalLayers\":1,\"tuning\":\"low_latency\",\"width\":1920,"
           "\"zeroReorderDelay\":true}\n";
  return glyphrelay::sha256_hex(value.str());
}

Arguments parse_arguments(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "usage: glyphrelay_uniform_aq_evaluate --bundle FILE --bundle-sha256 SHA256 "
                 "--output DIRECTORY --condition-id ID --requested-payload-bps INTEGER "
                 "--enable-aq BOOL --aq-strength INTEGER --enable-temporal-aq BOOL "
                 "--effective-encoder-fields-sha256 SHA256 --corpus-protocol-sha256 SHA256 "
                 "--development-manifest-sha256 SHA256 --development-render-index-sha256 SHA256 "
                 "--saliency-grid-sha256 SHA256\n";
    std::exit(0);
  }
  if (argc != 27) {
    throw std::runtime_error("uniform_aq_arguments_invalid");
  }
  std::vector<std::pair<std::string_view, std::string_view>> values;
  values.reserve(13U);
  for (int index = 1; index < argc; index += 2) {
    values.emplace_back(argv[index], argv[index + 1]);
  }
  const auto take = [&](std::string_view name) {
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const auto &entry) { return entry.first == name; });
    if (found == values.end() ||
        std::count_if(values.begin(), values.end(),
                      [&](const auto &entry) { return entry.first == name; }) != 1) {
      throw std::runtime_error("uniform_aq_arguments_invalid");
    }
    return found->second;
  };
  Arguments arguments{
      .bundle = std::filesystem::absolute(take("--bundle")),
      .output = std::filesystem::absolute(take("--output")),
      .bundle_sha256 = std::string(take("--bundle-sha256")),
      .condition_id = std::string(take("--condition-id")),
      .effective_encoder_fields_sha256 = std::string(take("--effective-encoder-fields-sha256")),
      .corpus_protocol_sha256 = std::string(take("--corpus-protocol-sha256")),
      .development_manifest_sha256 = std::string(take("--development-manifest-sha256")),
      .development_render_index_sha256 = std::string(take("--development-render-index-sha256")),
      .saliency_grid_sha256 = std::string(take("--saliency-grid-sha256")),
      .requested_payload_bps =
          parse_u32(take("--requested-payload-bps"), "uniform_aq_payload_rate_invalid"),
      .enable_aq = parse_bool(take("--enable-aq"), "uniform_aq_enable_aq_invalid"),
      .aq_strength = parse_u32(take("--aq-strength"), "uniform_aq_strength_invalid"),
      .enable_temporal_aq =
          parse_bool(take("--enable-temporal-aq"), "uniform_aq_enable_temporal_aq_invalid"),
  };
  if (!safe_identifier(arguments.condition_id) || arguments.requested_payload_bps == 0U ||
      !sha256_hex(arguments.bundle_sha256) ||
      !sha256_hex(arguments.effective_encoder_fields_sha256) ||
      !sha256_hex(arguments.corpus_protocol_sha256) ||
      !sha256_hex(arguments.development_manifest_sha256) ||
      !sha256_hex(arguments.development_render_index_sha256) ||
      !sha256_hex(arguments.saliency_grid_sha256) ||
      effective_encoder_fields_sha256(arguments) != arguments.effective_encoder_fields_sha256) {
    throw std::runtime_error("uniform_aq_arguments_invalid");
  }
  return arguments;
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty() || fraction < 0.0 || fraction > 1.0) {
    throw std::runtime_error("uniform_aq_percentile_input_invalid");
  }
  std::sort(values.begin(), values.end());
  const auto rank =
      static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size())) - 1.0);
  return values[std::min(rank, values.size() - 1U)];
}

glyphrelay::CapturedFrame &captured_frame(SampleFrame &sample, std::size_t frame_index,
                                          std::size_t sequence_index) {
  auto &frame = sample.captured;
  frame.frame_id = frame_index + 1U;
  frame.geometry = {
      .epoch = sequence_index + 1U,
      .source_width = kWidth,
      .source_height = kHeight,
      .source_crop = {0U, 0U, kWidth, kHeight},
      .visible_width = kWidth,
      .visible_height = kHeight,
      .coded_width = kWidth,
      .coded_height = kHeight,
  };
  frame.pixel_order = glyphrelay::PackedPixelOrder::bgra;
  frame.pitch = kWidth * 4U;
  frame.monotonic_timestamp_ns = frame.frame_id * kFrameIntervalNs;
  return frame;
}

glyphrelay::NvencEncoderConfig encoder_configuration(const Arguments &arguments) {
  return {
      .width = kWidth,
      .height = kHeight,
      .frames_per_second = 30U,
      .target_bitrate_bps = arguments.requested_payload_bps,
      .maximum_bitrate_bps = arguments.requested_payload_bps,
      .gop_frames = 60U,
      .level_idc = 40U,
      .capacity = kCapacity,
      .maximum_busy_retries = 100U,
      .mode = glyphrelay::NvencFrameMode::uniform,
      .enable_aq = arguments.enable_aq,
      .aq_strength = arguments.aq_strength,
      .enable_temporal_aq = arguments.enable_temporal_aq,
      .fixed_emphasis_map = {},
  };
}

struct Observation {
  std::vector<double> preprocess_ms;
  std::vector<double> encode_ms;
  std::vector<double> combined_ms;
  std::array<std::size_t, kPendingBuckets> pending_bucket_maxima{};
  std::size_t encoded_frames = 0U;
  std::size_t stream_bytes = 0U;
  bool first_access_unit_keyframe = false;
  bool first_access_unit_parameter_sets = false;
  double maximum_pending_age_ms = 0.0;
  double mean_sender_cpu_percent = 0.0;
};

Observation encode(const Arguments &arguments, std::vector<Sequence> &sequences,
                   const std::filesystem::path &temporary_stream) {
  auto context = std::make_shared<glyphrelay::CudaPrimaryContext>(0);
  if (!context->available()) {
    throw std::runtime_error("uniform_aq_cuda_context_unavailable:" + context->reason());
  }
  glyphrelay::CudaPreprocessor preprocessor(context, kWidth, kHeight, kCapacity, kCapacity);
  if (!preprocessor.available()) {
    throw std::runtime_error("uniform_aq_preprocessor_unavailable:" + preprocessor.reason());
  }
  auto config = encoder_configuration(arguments);
  if (!glyphrelay::valid_nvenc_encoder_configuration(config)) {
    throw std::runtime_error("uniform_aq_encoder_configuration_invalid");
  }
  std::ofstream stream(temporary_stream, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("uniform_aq_stream_open_failed");
  }

  using Clock = std::chrono::steady_clock;
  std::vector<Clock::time_point> submit_times(kSubmittedFrameCount);
  std::vector<double> encode_ms;
  encode_ms.reserve(kMeasurementFrames);
  std::mutex output_mutex;
  std::condition_variable output_condition;
  std::string callback_failure;
  std::size_t encoded_frames = 0U;
  std::size_t stream_bytes = 0U;
  bool first_keyframe = false;
  bool first_parameter_sets = false;
  glyphrelay::NvencEncoder encoder(
      context, preprocessor, std::move(config), [&](glyphrelay::NvencEncodedFrame frame) {
        const auto callback_time = Clock::now();
        std::scoped_lock lock(output_mutex);
        if (!callback_failure.empty()) {
          output_condition.notify_all();
          return;
        }
        const auto expected = encoded_frames + 1U;
        if (frame.submission_sequence != expected || frame.frame_id != expected ||
            frame.annex_b.empty()) {
          callback_failure = "uniform_aq_output_identity_invalid";
          output_condition.notify_all();
          return;
        }
        if (expected == 1U) {
          first_keyframe = frame.keyframe;
          first_parameter_sets = frame.parameter_sets_present;
        }
        stream.write(reinterpret_cast<const char *>(frame.annex_b.data()),
                     static_cast<std::streamsize>(frame.annex_b.size()));
        if (!stream) {
          callback_failure = "uniform_aq_stream_write_failed";
          output_condition.notify_all();
          return;
        }
        if (expected > kWarmupFrames && expected <= kMeasurementEndFrame) {
          encode_ms.push_back(
              std::chrono::duration<double, std::milli>(callback_time - submit_times[expected - 1U])
                  .count());
        }
        stream_bytes += frame.annex_b.size();
        ++encoded_frames;
        output_condition.notify_all();
      });
  if (!encoder.available()) {
    throw std::runtime_error("uniform_aq_encoder_unavailable:" + encoder.reason());
  }

  std::vector<double> preprocess_ms;
  std::vector<double> combined_ms;
  preprocess_ms.reserve(kMeasurementFrames);
  combined_ms.reserve(kMeasurementFrames);
  std::vector<double> per_frame_preprocess(kMeasurementEndFrame);
  std::array<std::size_t, kPendingBuckets> pending_bucket_maxima{};
  const auto wall_start = Clock::now();
  Clock::time_point measurement_wall_start{};
  Clock::time_point measurement_wall_end{};
  std::clock_t measurement_cpu_start = 0;
  std::clock_t measurement_cpu_end = 0;
  std::size_t global_frame = 0U;
  try {
    for (std::size_t sequence_index = 0U; sequence_index < sequences.size(); ++sequence_index) {
      auto &sequence = sequences[sequence_index];
      for (std::size_t local_frame = 0U; local_frame < kFramesPerSequence;
           ++local_frame, ++global_frame) {
        if (global_frame == kWarmupFrames) {
          measurement_wall_start = Clock::now();
          measurement_cpu_start = std::clock();
        }
        auto &frame =
            captured_frame(sequence.frames[local_frame / 60U], global_frame, sequence_index);
        const auto ticket = preprocessor.enqueue(frame, glyphrelay::ColorRange::limited);
        if (!ticket.passed) {
          throw std::runtime_error("uniform_aq_enqueue_failed:" + ticket.reason);
        }
        auto completion = preprocessor.wait(ticket);
        if (!completion.passed) {
          throw std::runtime_error("uniform_aq_wait_failed:" + completion.reason);
        }
        const auto preprocess = static_cast<double>(completion.timings.total()) / 1'000'000.0;
        if (global_frame < kMeasurementEndFrame) {
          per_frame_preprocess[global_frame] = preprocess;
        }
        if (global_frame >= kWarmupFrames && global_frame < kMeasurementEndFrame) {
          preprocess_ms.push_back(preprocess);
        }
        submit_times[global_frame] = Clock::now();
        const auto submitted = encoder.submit({
            .ticket = ticket,
            .completion = std::move(completion),
            .submission_sequence = global_frame + 1U,
            .presentation_timestamp_ns = (global_frame + 1U) * kFrameIntervalNs,
            .duration_ns = kFrameIntervalNs,
            .dependency_epoch = sequence_index + 1U,
            .force_idr = local_frame == 0U,
        });
        if (!submitted.passed) {
          throw std::runtime_error("uniform_aq_submit_failed:" + submitted.reason);
        }
        if (global_frame >= kWarmupFrames && global_frame < kMeasurementEndFrame) {
          const auto diagnostics = encoder.diagnostics();
          const auto bucket =
              std::min(kPendingBuckets - 1U,
                       (global_frame - kWarmupFrames) * kPendingBuckets / kMeasurementFrames);
          pending_bucket_maxima[bucket] =
              std::max(pending_bucket_maxima[bucket], diagnostics.active_submissions);
        }
        {
          std::scoped_lock lock(output_mutex);
          if (!callback_failure.empty()) {
            throw std::runtime_error(callback_failure);
          }
        }
        if (global_frame < kMeasurementEndFrame) {
          const auto next_deadline =
              wall_start + std::chrono::nanoseconds((global_frame + 1U) * kFrameIntervalNs);
          std::this_thread::sleep_until(next_deadline);
          if (global_frame + 1U == kMeasurementEndFrame) {
            measurement_cpu_end = std::clock();
            measurement_wall_end = Clock::now();
          }
        } else {
          std::unique_lock lock(output_mutex);
          if (!output_condition.wait_for(lock, std::chrono::seconds(5), [&] {
                return encoded_frames >= global_frame + 1U || !callback_failure.empty();
              })) {
            throw std::runtime_error("uniform_aq_fast_path_output_timeout");
          }
          if (!callback_failure.empty()) {
            throw std::runtime_error(callback_failure);
          }
        }
      }
    }
    const auto flushed = encoder.flush();
    if (!flushed.passed) {
      throw std::runtime_error("uniform_aq_flush_failed:" + flushed.reason);
    }
  } catch (...) {
    static_cast<void>(encoder.close());
    throw;
  }

  {
    std::unique_lock lock(output_mutex);
    if (!output_condition.wait_for(lock, std::chrono::seconds(30), [&] {
          return encoded_frames == kSubmittedFrameCount || !callback_failure.empty();
        })) {
      throw std::runtime_error("uniform_aq_output_completion_timeout");
    }
    if (!callback_failure.empty()) {
      throw std::runtime_error(callback_failure);
    }
  }
  const auto before_close = encoder.diagnostics();
  if (before_close.accepted_frames != kSubmittedFrameCount ||
      before_close.completed_frames != kSubmittedFrameCount ||
      before_close.active_submissions != 0U) {
    throw std::runtime_error("uniform_aq_completion_count_invalid");
  }
  if (!encoder.close().passed || !preprocessor.all_free()) {
    throw std::runtime_error("uniform_aq_teardown_failed");
  }
  stream.flush();
  stream.close();
  if (!stream || encoded_frames != kSubmittedFrameCount ||
      encode_ms.size() != preprocess_ms.size() || !first_keyframe || !first_parameter_sets) {
    throw std::runtime_error("uniform_aq_output_contract_failed");
  }
  for (std::size_t index = kWarmupFrames; index < kMeasurementEndFrame; ++index) {
    combined_ms.push_back(per_frame_preprocess[index] + encode_ms[index - kWarmupFrames]);
  }
  if (!context->shutdown()) {
    throw std::runtime_error("uniform_aq_context_shutdown_failed");
  }
  const auto wall_seconds =
      std::chrono::duration<double>(measurement_wall_end - measurement_wall_start).count();
  const auto cpu_seconds =
      static_cast<double>(measurement_cpu_end - measurement_cpu_start) / CLOCKS_PER_SEC;
  const auto maximum_pending_age_ms = *std::max_element(encode_ms.begin(), encode_ms.end());
  return {
      .preprocess_ms = std::move(preprocess_ms),
      .encode_ms = std::move(encode_ms),
      .combined_ms = std::move(combined_ms),
      .pending_bucket_maxima = pending_bucket_maxima,
      .encoded_frames = encoded_frames,
      .stream_bytes = stream_bytes,
      .first_access_unit_keyframe = first_keyframe,
      .first_access_unit_parameter_sets = first_parameter_sets,
      .maximum_pending_age_ms = maximum_pending_age_ms,
      .mean_sender_cpu_percent = wall_seconds > 0.0 ? 100.0 * cpu_seconds / wall_seconds : 0.0,
  };
}

void write_evidence(const Arguments &arguments, const Observation &observation,
                    const std::filesystem::path &stream_path,
                    const std::filesystem::path &evidence_path) {
  std::ofstream output(evidence_path, std::ios::out | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("uniform_aq_evidence_open_failed");
  }
  const auto measured_mbps = static_cast<double>(observation.stream_bytes) * 8.0 /
                             (static_cast<double>(kSubmittedFrameCount) / 30.0) / 1'000'000.0;
  const bool pending_positive_trend =
      observation.pending_bucket_maxima.back() > observation.pending_bucket_maxima.front();
  output << std::setprecision(17) << "{\"aqFields\":{\"aqStrength\":" << arguments.aq_strength
         << ",\"enableAQ\":" << (arguments.enable_aq ? "true" : "false")
         << ",\"enableTemporalAQ\":" << (arguments.enable_temporal_aq ? "true" : "false")
         << "},\"bundleSha256\":\"" << arguments.bundle_sha256 << "\",\"conditionId\":\""
         << arguments.condition_id << "\",\"corpusProtocolSha256\":\""
         << arguments.corpus_protocol_sha256 << "\",\"developmentManifestSha256\":\""
         << arguments.development_manifest_sha256 << "\",\"developmentRenderIndexSha256\":\""
         << arguments.development_render_index_sha256 << "\",\"effectiveEncoderFieldsSha256\":\""
         << arguments.effective_encoder_fields_sha256
         << "\",\"encodedFrames\":" << observation.encoded_frames << ",\"firstAccessUnitKeyframe\":"
         << (observation.first_access_unit_keyframe ? "true" : "false")
         << ",\"firstAccessUnitParameterSets\":"
         << (observation.first_access_unit_parameter_sets ? "true" : "false")
         << ",\"frameDurationNs\":" << kFrameIntervalNs << ",\"height\":" << kHeight
         << ",\"maximumPendingAgeMs\":" << observation.maximum_pending_age_ms
         << ",\"meanSenderCpuPercent\":" << observation.mean_sender_cpu_percent
         << ",\"measuredPayloadMbps\":" << measured_mbps
         << ",\"measurementFrames\":" << kMeasurementFrames
         << ",\"p95EncodeMs\":" << percentile(observation.encode_ms, 0.95)
         << ",\"p95PreprocessEncodeMs\":" << percentile(observation.combined_ms, 0.95)
         << ",\"p95PreprocessMs\":" << percentile(observation.preprocess_ms, 0.95)
         << ",\"p99EncodeMs\":" << percentile(observation.encode_ms, 0.99)
         << ",\"pendingBucketMaxima\":[";
  for (std::size_t index = 0U; index < observation.pending_bucket_maxima.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    output << observation.pending_bucket_maxima[index];
  }
  output << "],\"pendingPositiveTrend\":" << (pending_positive_trend ? "true" : "false")
         << ",\"requestedPayloadBps\":" << arguments.requested_payload_bps
         << ",\"saliencyGridSha256\":\"" << arguments.saliency_grid_sha256
         << "\",\"schemaVersion\":1,\"status\":\"PASSED\",\"streamBytes\":"
         << observation.stream_bytes << ",\"streamSha256\":\""
         << glyphrelay::sha256_file_hex(stream_path)
         << "\",\"submittedFrames\":" << kSubmittedFrameCount
         << ",\"warmupFrames\":" << kWarmupFrames << ",\"width\":" << kWidth << "}\n";
  output.flush();
  if (!output) {
    throw std::runtime_error("uniform_aq_evidence_write_failed");
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    if (std::filesystem::exists(arguments.output) ||
        !std::filesystem::create_directory(arguments.output)) {
      throw std::runtime_error("uniform_aq_output_exists_or_unavailable");
    }
    const auto temporary_stream = arguments.output / ".trial.h264.tmp";
    const auto stream_path = arguments.output / "trial.h264";
    const auto evidence_path = arguments.output / "native-evidence.json";
    auto sequences = load_bundle(arguments);
    const auto observation = encode(arguments, sequences, temporary_stream);
    std::filesystem::rename(temporary_stream, stream_path);
    write_evidence(arguments, observation, stream_path, evidence_path);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "uniform AQ evaluation failed: " << error.what() << '\n';
    return 8;
  }
}
