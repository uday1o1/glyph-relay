#include "glyphrelay/capture.hpp"
#include "glyphrelay/cuda_context.hpp"
#include "glyphrelay/cuda_preprocess.hpp"
#include "glyphrelay/nvenc_encoder.hpp"
#include "glyphrelay/sha256.hpp"
#include "glyphrelay/synthetic_source.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kWidth = 1920U;
constexpr std::size_t kHeight = 1080U;
constexpr std::size_t kCapacity = 4U;
constexpr std::size_t kFrames = 300U;
constexpr std::size_t kTeardownCycles = 10U;
constexpr std::uint64_t kFrameDurationNs = 33'333'333U;

struct Arguments {
  std::filesystem::path output;
  glyphrelay::SaliencyConfiguration saliency;
  std::string selection_sha256;
  std::string configuration_sha256;
};

struct SessionObservation {
  std::string mode;
  std::string stream_sha256;
  std::uint64_t stream_bytes = 0U;
  std::size_t frames = 0U;
  std::size_t peak_in_flight = 0U;
  bool output_thread_dedicated = false;
  bool first_access_unit_keyframe = false;
  bool first_access_unit_parameter_sets = false;
  bool resources_released = false;
};

glyphrelay::CapturedFrame make_frame(std::size_t frame_index,
                                     const std::vector<std::uint8_t> &base) {
  glyphrelay::CapturedFrame frame;
  frame.frame_id = frame_index + 1U;
  frame.geometry = {
      .epoch = 1U,
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
  frame.monotonic_timestamp_ns = frame.frame_id * kFrameDurationNs;
  frame.pixels = base;
  const auto cursor_x = 32U + (frame_index * 19U) % (kWidth - 64U);
  for (std::size_t y = 100U; y < 164U; ++y) {
    for (std::size_t x = cursor_x; x < cursor_x + 4U; ++x) {
      const auto offset = (y * kWidth + x) * 4U;
      frame.pixels[offset] = 0x20U;
      frame.pixels[offset + 1U] = 0xE0U;
      frame.pixels[offset + 2U] = 0xF0U;
      frame.pixels[offset + 3U] = 0xFFU;
    }
  }
  return frame;
}

std::vector<std::uint8_t> make_base_frame() {
  std::vector<std::uint8_t> pixels(kWidth * kHeight * 4U, 0xFFU);
  for (std::size_t y = 0U; y < kHeight; ++y) {
    for (std::size_t x = 0U; x < kWidth; ++x) {
      const auto offset = (y * kWidth + x) * 4U;
      const auto checker = ((x / 96U) + (y / 64U)) & 1U;
      const auto background = static_cast<std::uint8_t>(checker == 0U ? 0xF4U : 0xE8U);
      pixels[offset] = background;
      pixels[offset + 1U] = background;
      pixels[offset + 2U] = background;
      if (x >= 96U && x < 1536U && y >= 96U && y < 920U && ((y - 96U) % 32U) < 3U &&
          ((x - 96U) % 18U) < 12U) {
        pixels[offset] = 0x24U;
        pixels[offset + 1U] = 0x24U;
        pixels[offset + 2U] = 0x24U;
      }
    }
  }
  return pixels;
}

glyphrelay::NvencEncoderConfig configuration(glyphrelay::NvencFrameMode mode) {
  glyphrelay::NvencEncoderConfig output{
      .width = kWidth,
      .height = kHeight,
      .frames_per_second = 30U,
      .target_bitrate_bps = 2'000'000U,
      .maximum_bitrate_bps = 2'000'000U,
      .gop_frames = 60U,
      .level_idc = 40U,
      .capacity = kCapacity,
      .maximum_busy_retries = 100U,
      .mode = mode,
      .fixed_emphasis_map = {},
  };
  if (mode == glyphrelay::NvencFrameMode::fixed_emphasis) {
    output.fixed_emphasis_map = glyphrelay::m0_fixed_emphasis_map();
  }
  return output;
}

SessionObservation run_session(const std::shared_ptr<glyphrelay::CudaPrimaryContext> &context,
                               glyphrelay::NvencFrameMode mode,
                               const std::filesystem::path &output_directory,
                               const std::vector<std::uint8_t> &base_frame,
                               glyphrelay::SaliencyConfiguration saliency) {
  const auto mode_name = glyphrelay::nvenc_frame_mode_name(mode);
  const auto stream_path = output_directory / (mode_name + ".h264");
  std::ofstream stream(stream_path, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("nvenc_qualification_stream_open_failed");
  }
  glyphrelay::CudaPreprocessor preprocessor(context, kWidth, kHeight, kCapacity, kCapacity,
                                            saliency);
  if (!preprocessor.available()) {
    throw std::runtime_error("nvenc_qualification_preprocessor_unavailable:" +
                             preprocessor.reason());
  }
  std::mutex output_mutex;
  std::mutex callback_gate_mutex;
  std::condition_variable callback_gate_condition;
  std::vector<glyphrelay::NvencEncodedFrame> outputs;
  outputs.reserve(kFrames);
  const auto producer_thread = std::this_thread::get_id();
  bool dedicated_output = true;
  bool first_callback_released = false;
  glyphrelay::NvencEncoder encoder(
      context, preprocessor, configuration(mode), [&](glyphrelay::NvencEncodedFrame frame) {
        if (frame.submission_sequence == 1U) {
          std::unique_lock gate_lock(callback_gate_mutex);
          if (!callback_gate_condition.wait_for(gate_lock, std::chrono::seconds(5),
                                                [&] { return first_callback_released; })) {
            throw std::runtime_error("nvenc_qualification_multiflight_gate_timeout");
          }
        }
        std::scoped_lock lock(output_mutex);
        dedicated_output = dedicated_output && std::this_thread::get_id() != producer_thread;
        const auto expected = outputs.size() + 1U;
        if (frame.submission_sequence != expected || frame.frame_id != expected ||
            frame.presentation_timestamp_ns != expected * kFrameDurationNs ||
            frame.dependency_epoch != 1U || frame.annex_b.empty()) {
          throw std::runtime_error("nvenc_qualification_output_identity_invalid");
        }
        stream.write(reinterpret_cast<const char *>(frame.annex_b.data()),
                     static_cast<std::streamsize>(frame.annex_b.size()));
        if (!stream) {
          throw std::runtime_error("nvenc_qualification_stream_write_failed");
        }
        outputs.push_back(std::move(frame));
      });
  if (!encoder.available()) {
    throw std::runtime_error("nvenc_qualification_encoder_unavailable:" + encoder.reason());
  }

  std::vector<glyphrelay::CudaPreprocessTicket> batch;
  batch.reserve(kCapacity);
  std::size_t peak_in_flight = 0U;
  try {
    for (std::size_t frame_index = 0U; frame_index < kFrames; ++frame_index) {
      auto frame = make_frame(frame_index, base_frame);
      const auto ticket = preprocessor.enqueue(frame, glyphrelay::ColorRange::limited);
      if (!ticket.passed) {
        throw std::runtime_error("nvenc_qualification_enqueue_failed:" + ticket.reason);
      }
      batch.push_back(ticket);
      if (batch.size() != kCapacity && frame_index + 1U != kFrames) {
        continue;
      }
      for (const auto &pending : batch) {
        auto completion = preprocessor.wait(pending);
        if (!completion.passed) {
          throw std::runtime_error("nvenc_qualification_wait_failed:" + completion.reason);
        }
        const auto submitted = encoder.submit({
            .ticket = pending,
            .completion = std::move(completion),
            .submission_sequence = pending.token.frame_id,
            .presentation_timestamp_ns = pending.token.frame_id * kFrameDurationNs,
            .duration_ns = kFrameDurationNs,
            .dependency_epoch = 1U,
            .force_idr = pending.token.frame_id == 1U,
        });
        if (!submitted.passed) {
          throw std::runtime_error("nvenc_qualification_submit_failed:" + submitted.reason);
        }
        peak_in_flight = std::max(peak_in_flight, encoder.diagnostics().active_submissions);
        if (peak_in_flight >= 2U) {
          {
            std::scoped_lock gate_lock(callback_gate_mutex);
            first_callback_released = true;
          }
          callback_gate_condition.notify_all();
        }
      }
      batch.clear();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  } catch (...) {
    {
      std::scoped_lock gate_lock(callback_gate_mutex);
      first_callback_released = true;
    }
    callback_gate_condition.notify_all();
    static_cast<void>(encoder.close());
    throw;
  }
  const auto flushed = encoder.flush();
  if (!flushed.passed) {
    throw std::runtime_error("nvenc_qualification_flush_failed:" + flushed.reason);
  }
  const auto before_close = encoder.diagnostics();
  if (before_close.accepted_frames != kFrames || before_close.completed_frames != kFrames ||
      before_close.active_submissions != 0U) {
    throw std::runtime_error("nvenc_qualification_completion_count_invalid");
  }
  if (!encoder.close().passed || !preprocessor.all_free()) {
    throw std::runtime_error("nvenc_qualification_teardown_failed");
  }
  stream.flush();
  stream.close();
  if (outputs.size() != kFrames || peak_in_flight < 2U || !dedicated_output ||
      !outputs.front().keyframe || !outputs.front().parameter_sets_present) {
    throw std::runtime_error("nvenc_qualification_output_contract_failed");
  }
  return {
      .mode = mode_name,
      .stream_sha256 = glyphrelay::sha256_file_hex(stream_path),
      .stream_bytes = std::filesystem::file_size(stream_path),
      .frames = outputs.size(),
      .peak_in_flight = peak_in_flight,
      .output_thread_dedicated = dedicated_output,
      .first_access_unit_keyframe = outputs.front().keyframe,
      .first_access_unit_parameter_sets = outputs.front().parameter_sets_present,
      .resources_released = preprocessor.all_free(),
  };
}

void run_teardown_cycle(const std::shared_ptr<glyphrelay::CudaPrimaryContext> &context,
                        const std::vector<std::uint8_t> &base_frame,
                        glyphrelay::SaliencyConfiguration saliency) {
  constexpr std::size_t capacity = 1U;
  glyphrelay::CudaPreprocessor preprocessor(context, kWidth, kHeight, capacity, capacity, saliency);
  if (!preprocessor.available()) {
    throw std::runtime_error("nvenc_teardown_preprocessor_unavailable:" + preprocessor.reason());
  }
  std::atomic_size_t output_count{0U};
  glyphrelay::NvencEncoderConfig config =
      configuration(glyphrelay::NvencFrameMode::automatic_emphasis);
  config.capacity = capacity;
  glyphrelay::NvencEncoder encoder(
      context, preprocessor, std::move(config), [&](glyphrelay::NvencEncodedFrame frame) {
        if (frame.frame_id != 1U || frame.submission_sequence != 1U || !frame.keyframe ||
            !frame.parameter_sets_present || frame.annex_b.empty()) {
          throw std::runtime_error("nvenc_teardown_output_invalid");
        }
        output_count.fetch_add(1U, std::memory_order_relaxed);
      });
  if (!encoder.available()) {
    throw std::runtime_error("nvenc_teardown_encoder_unavailable:" + encoder.reason());
  }
  const auto ticket =
      preprocessor.enqueue(make_frame(0U, base_frame), glyphrelay::ColorRange::limited);
  if (!ticket.passed) {
    throw std::runtime_error("nvenc_teardown_enqueue_failed:" + ticket.reason);
  }
  const auto completion = preprocessor.wait(ticket);
  if (!completion.passed) {
    throw std::runtime_error("nvenc_teardown_wait_failed:" + completion.reason);
  }
  auto invalid_completion = completion;
  ++invalid_completion.surface.context.generation;
  const auto invalid = encoder.submit({
      .ticket = ticket,
      .completion = std::move(invalid_completion),
      .submission_sequence = 1U,
      .presentation_timestamp_ns = kFrameDurationNs,
      .duration_ns = kFrameDurationNs,
      .dependency_epoch = 1U,
      .force_idr = true,
  });
  if (invalid.passed || invalid.reason != "nvenc_encode_surface_contract_mismatch") {
    throw std::runtime_error("nvenc_teardown_invalid_context_not_rejected");
  }
  const auto submitted = encoder.submit({
      .ticket = ticket,
      .completion = completion,
      .submission_sequence = 1U,
      .presentation_timestamp_ns = kFrameDurationNs,
      .duration_ns = kFrameDurationNs,
      .dependency_epoch = 1U,
      .force_idr = true,
  });
  const auto flushed = encoder.flush();
  const auto closed = encoder.close();
  if (!submitted.passed || !flushed.passed || !closed.passed ||
      output_count.load(std::memory_order_relaxed) != 1U || !preprocessor.all_free()) {
    throw std::runtime_error("nvenc_teardown_cycle_failed");
  }
}

void write_evidence(const std::filesystem::path &path,
                    const std::vector<SessionObservation> &sessions, const Arguments &arguments) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("nvenc_qualification_evidence_open_failed");
  }
  output << std::setprecision(17) << "{\"height\":" << kHeight
         << ",\"saliencyConfiguration\":{\"contrastWeight\":" << arguments.saliency.contrast_weight
         << ",\"dilationRadiusTiles\":" << arguments.saliency.dilation_radius_tiles
         << ",\"edgePairWeight\":" << arguments.saliency.edge_pair_weight
         << ",\"entryThreshold\":" << arguments.saliency.entry_threshold
         << ",\"exitThreshold\":" << arguments.saliency.exit_threshold
         << ",\"gradientWeight\":" << arguments.saliency.gradient_weight
         << ",\"previousScoreCoefficient\":" << arguments.saliency.previous_score_coefficient
         << ",\"smallStructureWeight\":" << arguments.saliency.small_structure_weight
         << "},\"saliencyConfigurationSha256\":\"" << arguments.configuration_sha256
         << "\",\"saliencySelectionSha256\":\"" << arguments.selection_sha256
         << "\",\"schemaVersion\":1,\"sessions\":[";
  for (std::size_t index = 0U; index < sessions.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    const auto &session = sessions[index];
    output << "{\"firstAccessUnitKeyframe\":"
           << (session.first_access_unit_keyframe ? "true" : "false")
           << ",\"firstAccessUnitParameterSets\":"
           << (session.first_access_unit_parameter_sets ? "true" : "false")
           << ",\"frames\":" << session.frames << ",\"mode\":\"" << session.mode
           << "\",\"outputThreadDedicated\":"
           << (session.output_thread_dedicated ? "true" : "false")
           << ",\"peakInFlight\":" << session.peak_in_flight
           << ",\"resourcesReleased\":" << (session.resources_released ? "true" : "false")
           << ",\"streamBytes\":" << session.stream_bytes << ",\"streamSha256\":\""
           << session.stream_sha256 << "\"}";
  }
  output << "],\"status\":\"PASSED\",\"teardownCycles\":" << kTeardownCycles
         << ",\"width\":" << kWidth << "}\n";
  output.flush();
  if (!output) {
    throw std::runtime_error("nvenc_qualification_evidence_write_failed");
  }
}

bool sha256_hex(std::string_view value) {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

double parse_double(std::string_view value) {
  double parsed = 0.0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      !std::isfinite(parsed)) {
    throw std::runtime_error("nvenc_qualification_decimal_invalid");
  }
  return parsed;
}

std::size_t parse_size(std::string_view value) {
  std::size_t parsed = 0U;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    throw std::runtime_error("nvenc_qualification_integer_invalid");
  }
  return parsed;
}

Arguments parse_arguments(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "usage: glyphrelay_nvenc_encoder_qualify --output DIRECTORY "
                 "--selection-sha256 SHA256 --configuration-sha256 SHA256 "
                 "--gradient-weight NUMBER --contrast-weight NUMBER "
                 "--edge-pair-weight NUMBER --small-structure-weight NUMBER "
                 "--entry-threshold NUMBER --exit-threshold NUMBER "
                 "--previous-score-coefficient NUMBER --dilation-radius-tiles INTEGER\n";
    std::exit(0);
  }
  if (argc != 23) {
    throw std::runtime_error("nvenc_qualification_arguments_invalid");
  }
  std::vector<std::pair<std::string_view, std::string_view>> values;
  values.reserve(11U);
  for (int index = 1; index < argc; index += 2) {
    values.emplace_back(argv[index], argv[index + 1]);
  }
  const auto take = [&](std::string_view name) {
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const auto &entry) { return entry.first == name; });
    if (found == values.end() ||
        std::count_if(values.begin(), values.end(),
                      [&](const auto &entry) { return entry.first == name; }) != 1) {
      throw std::runtime_error("nvenc_qualification_arguments_invalid");
    }
    return found->second;
  };
  Arguments arguments{
      .output = std::filesystem::absolute(take("--output")),
      .saliency =
          {
              .gradient_weight = parse_double(take("--gradient-weight")),
              .contrast_weight = parse_double(take("--contrast-weight")),
              .edge_pair_weight = parse_double(take("--edge-pair-weight")),
              .small_structure_weight = parse_double(take("--small-structure-weight")),
              .entry_threshold = parse_double(take("--entry-threshold")),
              .exit_threshold = parse_double(take("--exit-threshold")),
              .previous_score_coefficient = parse_double(take("--previous-score-coefficient")),
              .dilation_radius_tiles = parse_size(take("--dilation-radius-tiles")),
          },
      .selection_sha256 = std::string(take("--selection-sha256")),
      .configuration_sha256 = std::string(take("--configuration-sha256")),
  };
  if (!sha256_hex(arguments.selection_sha256) || !sha256_hex(arguments.configuration_sha256) ||
      !glyphrelay::valid_saliency_configuration(arguments.saliency)) {
    throw std::runtime_error("nvenc_qualification_selection_invalid");
  }
  return arguments;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto &output_directory = arguments.output;
    if (std::filesystem::exists(output_directory) ||
        !std::filesystem::create_directory(output_directory)) {
      throw std::runtime_error("nvenc_qualification_output_exists_or_unavailable");
    }
    auto context = std::make_shared<glyphrelay::CudaPrimaryContext>(0);
    if (!context->available()) {
      throw std::runtime_error("nvenc_qualification_cuda_context_unavailable:" + context->reason());
    }
    const auto identity = context->identity();
    const auto base_frame = make_base_frame();
    std::vector<SessionObservation> sessions;
    for (const auto mode :
         {glyphrelay::NvencFrameMode::uniform, glyphrelay::NvencFrameMode::fixed_emphasis,
          glyphrelay::NvencFrameMode::automatic_emphasis}) {
      sessions.push_back(
          run_session(context, mode, output_directory, base_frame, arguments.saliency));
      if (context->identity() != identity) {
        throw std::runtime_error("nvenc_qualification_context_identity_changed");
      }
    }
    for (std::size_t cycle = 0U; cycle < kTeardownCycles; ++cycle) {
      run_teardown_cycle(context, base_frame, arguments.saliency);
      if (context->identity() != identity) {
        throw std::runtime_error("nvenc_teardown_context_identity_changed");
      }
    }
    write_evidence(output_directory / "evidence.json", sessions, arguments);
    if (!context->shutdown()) {
      throw std::runtime_error("nvenc_qualification_context_shutdown_failed");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "NVENC encoder qualification failed: " << error.what() << '\n';
    return 8;
  }
}
