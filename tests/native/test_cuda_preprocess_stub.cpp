#include "glyphrelay/cuda_preprocess.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

} // namespace

int main() {
  glyphrelay::CudaPreprocessor pipeline(0, 640U, 480U, 2U, 2U);
  require(!pipeline.available() && pipeline.reason() == "cuda_preprocessor_not_built" &&
              !pipeline.context_identity().valid(),
          "the portable preset must report that CUDA preprocessing was not built");
  glyphrelay::CapturedFrame frame;
  require(!pipeline.enqueue(frame, glyphrelay::ColorRange::limited).passed &&
              !pipeline.wait({}).passed && !pipeline.mark_submitted({}).passed &&
              !pipeline.mark_encoder_input_released({}).passed && !pipeline.release({}).passed,
          "the portable CUDA adapter must fail closed without simulating work");
  pipeline.close_admission();
  require(!pipeline.diagnostics().admission_open && pipeline.all_free(),
          "the unsupported adapter must still preserve bounded shutdown diagnostics");
  return 0;
}
