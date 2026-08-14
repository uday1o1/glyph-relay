#include "glyphrelay/openh264_encoder.hpp"

#include "glyphrelay/h264_sps.hpp"
#include "glyphrelay/recording_profile.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#if GLYPHRELAY_HAS_OPENH264
#include <wels/codec_api.h>
#endif

namespace glyphrelay {

struct OpenH264Encoder::Implementation {
  OpenH264EncoderConfig config;
  std::string reason = "system_openh264_not_built";
  std::uint64_t next_frame_index = 0;
#if GLYPHRELAY_HAS_OPENH264
  ISVCEncoder *encoder = nullptr;
#endif

  ~Implementation() {
#if GLYPHRELAY_HAS_OPENH264
    if (encoder != nullptr) {
      static_cast<void>(encoder->Uninitialize());
      WelsDestroySVCEncoder(encoder);
    }
#endif
  }
};

namespace {

#if GLYPHRELAY_HAS_OPENH264
constexpr std::size_t kMaximumAccessUnitBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumNalBytes = 8U * 1024U * 1024U;
#endif

bool valid_config(const OpenH264EncoderConfig &config) {
  return config.width >= 16U && config.height >= 16U && (config.width & 1U) == 0U &&
         (config.height & 1U) == 0U && config.width <= 4096U && config.height <= 4096U &&
         config.frames_per_second > 0U && config.frames_per_second <= 60U &&
         config.target_bitrate_bps > 0U &&
         config.maximum_bitrate_bps <= static_cast<unsigned int>(std::numeric_limits<int>::max()) &&
         config.maximum_bitrate_bps >= config.target_bitrate_bps && config.gop_frames > 0U &&
         config.gop_frames <= 600U && config.level_idc == 40U;
}

#if GLYPHRELAY_HAS_OPENH264
bool checked_int(std::size_t value, int &destination) {
  if (value > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  destination = static_cast<int>(value);
  return true;
}

bool configure_encoder(auto &implementation) {
  if (WelsCreateSVCEncoder(&implementation.encoder) != cmResultSuccess ||
      implementation.encoder == nullptr) {
    implementation.reason = "openh264_create_failed";
    return false;
  }
  SEncParamExt parameters{};
  if (implementation.encoder->GetDefaultParams(&parameters) != cmResultSuccess) {
    implementation.reason = "openh264_default_parameters_failed";
    return false;
  }
  int width = 0;
  int height = 0;
  if (!checked_int(implementation.config.width, width) ||
      !checked_int(implementation.config.height, height)) {
    implementation.reason = "openh264_geometry_out_of_range";
    return false;
  }
  parameters.iUsageType = SCREEN_CONTENT_REAL_TIME;
  parameters.iPicWidth = width;
  parameters.iPicHeight = height;
  parameters.iTargetBitrate = static_cast<int>(implementation.config.target_bitrate_bps);
  parameters.iMaxBitrate = static_cast<int>(implementation.config.maximum_bitrate_bps);
  parameters.iRCMode = RC_BITRATE_MODE;
  parameters.fMaxFrameRate = static_cast<float>(implementation.config.frames_per_second);
  parameters.iTemporalLayerNum = 1;
  parameters.iSpatialLayerNum = 1;
  parameters.iComplexityMode = HIGH_COMPLEXITY;
  parameters.uiIntraPeriod = implementation.config.gop_frames;
  parameters.iNumRefFrame = 1;
  parameters.eSpsPpsIdStrategy = CONSTANT_ID;
  parameters.bPrefixNalAddingCtrl = false;
  parameters.bEnableSSEI = false;
  parameters.bSimulcastAVC = false;
  parameters.iPaddingFlag = 0;
  parameters.iEntropyCodingModeFlag = 0;
  parameters.bEnableFrameSkip = true;
  parameters.bEnableLongTermReference = false;
  parameters.iMultipleThreadIdc = 1;
  parameters.bUseLoadBalancing = false;
  parameters.bEnableDenoise = false;
  parameters.bEnableBackgroundDetection = false;
  parameters.bEnableAdaptiveQuant = false;
  parameters.bEnableFrameCroppingFlag = true;
  parameters.bEnableSceneChangeDetect = true;
  parameters.bIsLosslessLink = false;
  parameters.bFixRCOverShoot = false;
  parameters.iIdrBitrateRatio = 100;

  auto &layer = parameters.sSpatialLayers[0];
  layer.iVideoWidth = width;
  layer.iVideoHeight = height;
  layer.fFrameRate = parameters.fMaxFrameRate;
  layer.iSpatialBitrate = parameters.iTargetBitrate;
  layer.iMaxSpatialBitrate = parameters.iMaxBitrate;
  layer.uiProfileIdc = PRO_BASELINE;
  layer.uiLevelIdc = LEVEL_4_0;
  layer.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
  layer.bVideoSignalTypePresent = true;
  layer.uiVideoFormat = 5U;
  layer.bFullRange = false;
  layer.bColorDescriptionPresent = true;
  layer.uiColorPrimaries = 1U;
  layer.uiTransferCharacteristics = 1U;
  layer.uiColorMatrix = 1U;
  layer.bAspectRatioPresent = true;
  layer.eAspectRatio = ASP_1x1;

  if (implementation.encoder->InitializeExt(&parameters) != cmResultSuccess) {
    implementation.reason = "openh264_initialize_failed";
    return false;
  }
  int input_format = videoFormatI420;
  if (implementation.encoder->SetOption(ENCODER_OPTION_DATAFORMAT, &input_format) !=
      cmResultSuccess) {
    implementation.reason = "openh264_i420_configuration_failed";
    return false;
  }
  implementation.reason = "openh264_initialized";
  return true;
}
#endif

} // namespace

OpenH264Encoder::OpenH264Encoder(const OpenH264EncoderConfig &config)
    : implementation_(std::make_unique<Implementation>()) {
  implementation_->config = config;
  if (!valid_config(config)) {
    implementation_->reason = "openh264_configuration_invalid";
    return;
  }
#if GLYPHRELAY_HAS_OPENH264
  static_cast<void>(configure_encoder(*implementation_));
#endif
}

OpenH264Encoder::~OpenH264Encoder() = default;
OpenH264Encoder::OpenH264Encoder(OpenH264Encoder &&) noexcept = default;
OpenH264Encoder &OpenH264Encoder::operator=(OpenH264Encoder &&) noexcept = default;

bool OpenH264Encoder::available() const {
#if GLYPHRELAY_HAS_OPENH264
  return implementation_->encoder != nullptr && implementation_->reason == "openh264_initialized";
#else
  return false;
#endif
}

const std::string &OpenH264Encoder::initialization_reason() const {
  return implementation_->reason;
}

OpenH264EncodeResult OpenH264Encoder::encode(const I420Frame &frame, bool force_idr) {
#if !GLYPHRELAY_HAS_OPENH264
  static_cast<void>(force_idr);
#endif
  OpenH264EncodeResult result;
  result.frame_index = implementation_->next_frame_index;
  if (!available()) {
    result.reason = implementation_->reason;
    return result;
  }
  if (frame.width != implementation_->config.width ||
      frame.height != implementation_->config.height || frame.y_stride != frame.width ||
      frame.u_stride != frame.width / 2U || frame.v_stride != frame.width / 2U ||
      frame.bytes.size() != frame.width * frame.height * 3U / 2U) {
    result.reason = "openh264_i420_frame_contract_mismatch";
    return result;
  }
#if GLYPHRELAY_HAS_OPENH264
  if ((implementation_->next_frame_index == 0U || force_idr) &&
      implementation_->encoder->ForceIntraFrame(true) != cmResultSuccess) {
    result.reason = "openh264_force_idr_failed";
    return result;
  }
  SSourcePicture picture{};
  picture.iColorFormat = videoFormatI420;
  picture.iStride[0] = static_cast<int>(frame.y_stride);
  picture.iStride[1] = static_cast<int>(frame.u_stride);
  picture.iStride[2] = static_cast<int>(frame.v_stride);
  picture.pData[0] = const_cast<unsigned char *>(frame.y_plane().data());
  picture.pData[1] = const_cast<unsigned char *>(frame.u_plane().data());
  picture.pData[2] = const_cast<unsigned char *>(frame.v_plane().data());
  picture.iPicWidth = static_cast<int>(frame.width);
  picture.iPicHeight = static_cast<int>(frame.height);
  picture.uiTimeStamp = static_cast<long long>(implementation_->next_frame_index * 1000U /
                                               implementation_->config.frames_per_second);
  SFrameBSInfo bitstream{};
  if (implementation_->encoder->EncodeFrame(&picture, &bitstream) != cmResultSuccess) {
    result.reason = "openh264_encode_failed";
    return result;
  }
  if (bitstream.eFrameType == videoFrameTypeSkip) {
    result.passed = true;
    result.skipped = true;
    result.reason = "openh264_frame_skipped";
    ++implementation_->next_frame_index;
    return result;
  }
  if (bitstream.iLayerNum <= 0 || bitstream.iLayerNum > MAX_LAYER_NUM_OF_FRAME) {
    result.reason = "openh264_invalid_layer_count";
    return result;
  }
  if (bitstream.iFrameSizeInBytes <= 0 ||
      static_cast<std::size_t>(bitstream.iFrameSizeInBytes) > kMaximumAccessUnitBytes) {
    result.reason = "openh264_invalid_frame_size";
    return result;
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(static_cast<std::size_t>(bitstream.iFrameSizeInBytes));
  for (int layer_index = 0; layer_index < bitstream.iLayerNum; ++layer_index) {
    const auto &layer = bitstream.sLayerInfo[layer_index];
    if (layer.iNalCount <= 0 || layer.pNalLengthInByte == nullptr || layer.pBsBuf == nullptr) {
      result.reason = "openh264_invalid_layer_output";
      return result;
    }
    std::size_t offset = 0;
    for (int nal_index = 0; nal_index < layer.iNalCount; ++nal_index) {
      const int nal_size = layer.pNalLengthInByte[nal_index];
      if (nal_size <= 0 || offset > kMaximumNalBytes ||
          static_cast<std::size_t>(nal_size) > kMaximumNalBytes - offset ||
          bytes.size() > kMaximumAccessUnitBytes - static_cast<std::size_t>(nal_size)) {
        result.reason = "openh264_invalid_nal_size";
        return result;
      }
      bytes.insert(bytes.end(), layer.pBsBuf + offset, layer.pBsBuf + offset + nal_size);
      offset += static_cast<std::size_t>(nal_size);
    }
  }
  if (bytes.size() != static_cast<std::size_t>(bitstream.iFrameSizeInBytes)) {
    result.reason = "openh264_frame_size_mismatch";
    return result;
  }
  const auto parsed = parse_annex_b_access_unit(bytes);
  if (!parsed.passed) {
    result.reason = "openh264_output_" + parsed.reason;
    return result;
  }
  if (implementation_->next_frame_index == 0U &&
      !parsed.access_unit.starts_with_parameter_sets_and_idr()) {
    result.reason = "openh264_first_access_unit_not_sps_pps_idr";
    return result;
  }
  if (parsed.access_unit.contains(5U)) {
    if (!parsed.access_unit.starts_with_parameter_sets_and_idr()) {
      result.reason = "openh264_idr_not_sps_pps_idr";
      return result;
    }
    const auto sps =
        std::find_if(parsed.access_unit.nal_units.begin(), parsed.access_unit.nal_units.end(),
                     [](const AnnexBNalUnit &unit) { return unit.unit_type == 7U; });
    if (sps == parsed.access_unit.nal_units.end()) {
      result.reason = "openh264_idr_without_sps";
      return result;
    }
    const auto parsed_sps = parse_h264_sps(parsed.access_unit.payload(*sps));
    if (!parsed_sps.passed) {
      result.reason = "openh264_" + parsed_sps.reason;
      return result;
    }
    const auto compatibility = validate_recording_profile_sps(
        parsed_sps.info, implementation_->config.width, implementation_->config.height);
    if (!compatibility.compatible) {
      result.reason = "openh264_" + compatibility.reason;
      return result;
    }
  }
  result.keyframe = parsed.access_unit.contains(5U);
  result.access_unit = parsed.access_unit;
  result.passed = true;
  result.reason = "openh264_access_unit_encoded";
  ++implementation_->next_frame_index;
#endif
  return result;
}

} // namespace glyphrelay
