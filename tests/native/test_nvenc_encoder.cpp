#include "glyphrelay/cuda_context.hpp"
#include "glyphrelay/cuda_preprocess.hpp"
#include "glyphrelay/nvenc_encoder.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

void test_portable_stub_and_configuration_contract() {
  auto context = std::make_shared<glyphrelay::CudaPrimaryContext>(-1);
  glyphrelay::CudaPreprocessor preprocessor(context, 1920U, 1080U);

  glyphrelay::NvencEncoder uniform(
      context, preprocessor,
      glyphrelay::NvencEncoderConfig{.mode = glyphrelay::NvencFrameMode::uniform}, {});
#if GLYPHRELAY_HAS_NVENC
  require(!uniform.available() && uniform.reason() == "nvenc_shared_cuda_context_invalid",
          "an unavailable shared context must fail before opening a real NVENC session");
#else
  require(!uniform.available() && uniform.reason() == "nvenc_encoder_not_built",
          "the portable build must report that NVENC is not built");
#endif
  require(!uniform.submit({}).passed, "the unavailable encoder must reject frame submission");
  require(uniform.close().passed, "the unavailable encoder must close idempotently");
  require(uniform.close().passed, "repeated unavailable encoder close must remain safe");

  glyphrelay::NvencEncoder invalid_fixed(context, preprocessor,
                                         glyphrelay::NvencEncoderConfig{
                                             .mode = glyphrelay::NvencFrameMode::fixed_emphasis,
                                             .fixed_emphasis_map = {4},
                                         },
                                         {});
  require(!invalid_fixed.available() &&
              invalid_fixed.reason() == "nvenc_encoder_configuration_invalid",
          "a fixed map with the wrong macroblock count must fail before driver access");
}

void test_mode_names_are_stable() {
  require(glyphrelay::nvenc_frame_mode_name(glyphrelay::NvencFrameMode::uniform) == "uniform" &&
              glyphrelay::nvenc_frame_mode_name(glyphrelay::NvencFrameMode::fixed_emphasis) ==
                  "fixed_emphasis" &&
              glyphrelay::nvenc_frame_mode_name(glyphrelay::NvencFrameMode::automatic_emphasis) ==
                  "automatic_emphasis",
          "public NVENC mode names must remain stable for diagnostics and evidence");
}

} // namespace

int main() {
  test_portable_stub_and_configuration_contract();
  test_mode_names_are_stable();
  return 0;
}
