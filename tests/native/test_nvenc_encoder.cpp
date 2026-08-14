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

void test_uniform_aq_configuration_contract() {
  auto controlled = glyphrelay::NvencEncoderConfig{.mode = glyphrelay::NvencFrameMode::uniform};
  require(glyphrelay::valid_nvenc_encoder_configuration(controlled),
          "controlled uniform must use canonical disabled AQ fields");

  auto temporal_only = controlled;
  temporal_only.enable_temporal_aq = true;
  require(glyphrelay::valid_nvenc_encoder_configuration(temporal_only),
          "the frozen grid must admit temporal-only AQ");

  auto spatial = controlled;
  spatial.enable_aq = true;
  for (const auto strength : {1U, 4U, 8U, 12U, 15U}) {
    spatial.aq_strength = strength;
    require(glyphrelay::valid_nvenc_encoder_configuration(spatial),
            "every frozen spatial AQ strength must be representable");
  }
  spatial.aq_strength = 16U;
  require(!glyphrelay::valid_nvenc_encoder_configuration(spatial),
          "AQ strength above the documented NVENC range must fail");
  spatial.enable_aq = false;
  spatial.aq_strength = 1U;
  require(!glyphrelay::valid_nvenc_encoder_configuration(spatial),
          "disabled spatial AQ must use canonical strength zero");

  auto emphasis = glyphrelay::NvencEncoderConfig{
      .width = 16U,
      .height = 16U,
      .mode = glyphrelay::NvencFrameMode::fixed_emphasis,
      .enable_temporal_aq = true,
      .fixed_emphasis_map = {4},
  };
  require(!glyphrelay::valid_nvenc_encoder_configuration(emphasis),
          "every emphasis-map mode must reject temporal or spatial AQ");
  emphasis.enable_temporal_aq = false;
  require(glyphrelay::valid_nvenc_encoder_configuration(emphasis),
          "an AQ-disabled fixed emphasis map remains valid");
}

} // namespace

int main() {
  test_portable_stub_and_configuration_contract();
  test_mode_names_are_stable();
  test_uniform_aq_configuration_contract();
  return 0;
}
