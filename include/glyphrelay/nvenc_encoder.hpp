#pragma once

#include "glyphrelay/cuda_preprocess.hpp"
#include "glyphrelay/gpu_contracts.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace glyphrelay {

class CudaPrimaryContext;

struct NvencEncoderConfig {
  std::size_t width = 1920U;
  std::size_t height = 1080U;
  std::uint32_t frames_per_second = 30U;
  std::uint32_t target_bitrate_bps = 2'000'000U;
  std::uint32_t maximum_bitrate_bps = 2'000'000U;
  std::uint32_t gop_frames = 60U;
  std::uint32_t level_idc = 40U;
  std::size_t capacity = 3U;
  std::size_t maximum_busy_retries = 100U;
  NvencFrameMode mode = NvencFrameMode::automatic_emphasis;
  bool enable_aq = false;
  std::uint32_t aq_strength = 0U;
  bool enable_temporal_aq = false;
  std::vector<std::int8_t> fixed_emphasis_map;
};

struct NvencEncodeInput {
  CudaPreprocessTicket ticket;
  CudaPreprocessCompletion completion;
  std::uint64_t submission_sequence = 0U;
  std::uint64_t presentation_timestamp_ns = 0U;
  std::uint64_t duration_ns = 0U;
  std::uint64_t dependency_epoch = 0U;
  bool force_idr = false;
};

struct NvencEncodedFrame {
  std::vector<std::uint8_t> annex_b;
  std::uint64_t frame_id = 0U;
  std::uint64_t submission_sequence = 0U;
  std::uint64_t presentation_timestamp_ns = 0U;
  std::uint64_t dependency_epoch = 0U;
  bool keyframe = false;
  bool parameter_sets_present = false;
};

struct NvencEncoderOperation {
  bool passed = false;
  std::string reason;
};

struct NvencEncoderDiagnostics {
  bool available = false;
  bool admission_open = false;
  bool end_of_stream = false;
  bool fatal = false;
  std::size_t capacity = 0U;
  std::size_t active_submissions = 0U;
  std::uint64_t accepted_frames = 0U;
  std::uint64_t completed_frames = 0U;
  std::uint64_t busy_retries = 0U;
  std::string reason;
};

using NvencOutputCallback = std::function<void(NvencEncodedFrame)>;

bool valid_nvenc_encoder_configuration(const NvencEncoderConfig &configuration);

class NvencEncoder {
public:
  NvencEncoder(std::shared_ptr<CudaPrimaryContext> context, CudaPreprocessor &preprocessor,
               NvencEncoderConfig configuration, NvencOutputCallback output);
  ~NvencEncoder();

  NvencEncoder(const NvencEncoder &) = delete;
  NvencEncoder &operator=(const NvencEncoder &) = delete;
  NvencEncoder(NvencEncoder &&) noexcept;
  NvencEncoder &operator=(NvencEncoder &&) noexcept = delete;

  bool available() const;
  std::string reason() const;
  NvencEncoderOperation submit(NvencEncodeInput input);
  NvencEncoderOperation flush();
  NvencEncoderOperation close();
  NvencEncoderDiagnostics diagnostics() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace glyphrelay
