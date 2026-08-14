#pragma once

#include "glyphrelay/capture.hpp"
#include "glyphrelay/color_conversion.hpp"
#include "glyphrelay/gpu_contracts.hpp"
#include "glyphrelay/preprocess_pool.hpp"
#include "glyphrelay/saliency.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace glyphrelay {

class CudaPrimaryContext;

struct CudaPreprocessTimingsNs {
  std::uint64_t input_upload = 0U;
  std::uint64_t color_conversion = 0U;
  std::uint64_t feature_extraction = 0U;
  std::uint64_t temporal_hysteresis = 0U;
  std::uint64_t morphology_and_overrides = 0U;
  std::uint64_t macroblock_reduction = 0U;
  std::uint64_t host_map_copy = 0U;
  std::uint64_t total_pipeline = 0U;

  std::uint64_t total() const;
};

struct CudaPreprocessTicket {
  bool passed = false;
  std::string reason;
  PreprocessSlotToken token;
};

struct CudaPreprocessCompletion {
  bool passed = false;
  std::string reason;
  Nv12SurfaceDescriptor surface;
  EmphasisMapDescriptor emphasis_map;
  CudaPreprocessTimingsNs timings;
  std::size_t visible_width = 0U;
  std::size_t visible_height = 0U;
  std::size_t tile_width = 0U;
  std::size_t tile_height = 0U;
  std::vector<TileFeatureVector> debug_tiles;
  std::vector<std::uint8_t> debug_nv12;
};

struct CudaPreprocessOperation {
  bool passed = false;
  std::string reason;
};

class CudaPreprocessor {
public:
  CudaPreprocessor(int device_ordinal, std::size_t maximum_visible_width,
                   std::size_t maximum_visible_height, std::size_t source_capacity = 3U,
                   std::size_t surface_capacity = 3U, SaliencyConfiguration configuration = {});
  CudaPreprocessor(std::shared_ptr<CudaPrimaryContext> context, std::size_t maximum_visible_width,
                   std::size_t maximum_visible_height, std::size_t source_capacity = 3U,
                   std::size_t surface_capacity = 3U, SaliencyConfiguration configuration = {});
  ~CudaPreprocessor();
  CudaPreprocessor(CudaPreprocessor &&) noexcept;
  CudaPreprocessor &operator=(CudaPreprocessor &&) noexcept = delete;
  CudaPreprocessor(const CudaPreprocessor &) = delete;
  CudaPreprocessor &operator=(const CudaPreprocessor &) = delete;

  bool available() const;
  const std::string &reason() const;
  CudaContextIdentity context_identity() const;
  CudaPreprocessTicket enqueue(const CapturedFrame &frame, ColorRange range,
                               const SaliencyProcessOptions &options = {},
                               bool capture_debug_output = false);
  CudaPreprocessCompletion wait(const CudaPreprocessTicket &ticket);
  CudaPreprocessOperation mark_submitted(const CudaPreprocessTicket &ticket);
  CudaPreprocessOperation mark_encoder_input_released(const CudaPreprocessTicket &ticket);
  CudaPreprocessOperation release(const CudaPreprocessTicket &ticket);
  CudaPreprocessOperation abort(const CudaPreprocessTicket &ticket);
  void close_admission();
  PreprocessPoolDiagnostics diagnostics() const;
  bool all_free() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace glyphrelay
