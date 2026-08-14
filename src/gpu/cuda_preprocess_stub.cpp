#include "glyphrelay/cuda_preprocess.hpp"

#include <utility>

namespace glyphrelay {

std::uint64_t CudaPreprocessTimingsNs::total() const { return total_pipeline; }

struct CudaPreprocessor::Implementation {
  std::string reason = "cuda_preprocessor_not_built";
  PreprocessOwnershipRing ownership;

  Implementation(std::size_t source_capacity, std::size_t surface_capacity)
      : ownership(source_capacity, surface_capacity) {}
};

CudaPreprocessor::CudaPreprocessor(int, std::size_t, std::size_t, std::size_t source_capacity,
                                   std::size_t surface_capacity, SaliencyConfiguration)
    : implementation_(std::make_unique<Implementation>(source_capacity, surface_capacity)) {}

CudaPreprocessor::~CudaPreprocessor() = default;
CudaPreprocessor::CudaPreprocessor(CudaPreprocessor &&) noexcept = default;

bool CudaPreprocessor::available() const { return false; }
const std::string &CudaPreprocessor::reason() const { return implementation_->reason; }
CudaContextIdentity CudaPreprocessor::context_identity() const { return {}; }

CudaPreprocessTicket CudaPreprocessor::enqueue(const CapturedFrame &, ColorRange,
                                               const SaliencyProcessOptions &, bool) {
  return {false, implementation_->reason, {}};
}

CudaPreprocessCompletion CudaPreprocessor::wait(const CudaPreprocessTicket &) {
  CudaPreprocessCompletion result;
  result.reason = implementation_->reason;
  return result;
}

CudaPreprocessOperation CudaPreprocessor::mark_submitted(const CudaPreprocessTicket &) {
  return {false, implementation_->reason};
}

CudaPreprocessOperation
CudaPreprocessor::mark_encoder_input_released(const CudaPreprocessTicket &) {
  return {false, implementation_->reason};
}

CudaPreprocessOperation CudaPreprocessor::release(const CudaPreprocessTicket &) {
  return {false, implementation_->reason};
}

void CudaPreprocessor::close_admission() { implementation_->ownership.close_admission(); }

PreprocessPoolDiagnostics CudaPreprocessor::diagnostics() const {
  return implementation_->ownership.diagnostics();
}

bool CudaPreprocessor::all_free() const { return implementation_->ownership.all_free(); }

} // namespace glyphrelay
