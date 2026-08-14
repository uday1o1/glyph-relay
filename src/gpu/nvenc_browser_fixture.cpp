#include "glyphrelay/nvenc_browser_fixture.hpp"

#include "glyphrelay/cuda_context.hpp"
#include "glyphrelay/m0_browser_source.hpp"
#include "glyphrelay/nvenc_probe.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if GLYPHRELAY_HAS_NVENC
#include "glyphrelay/annex_b.hpp"
#include "glyphrelay/h264_sps.hpp"
#include "glyphrelay/recording_profile.hpp"
#include "glyphrelay/sha256.hpp"

#include <cuda.h>
#include <dlfcn.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace glyphrelay {

#if GLYPHRELAY_HAS_NVENC
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kRequestedBitrate = 2'000'000U;
constexpr std::size_t kSurfaceCount = 4U;
constexpr std::size_t kMaximumBusyRetries = 100U;
constexpr std::uint32_t kGopFrames = 60U;

class FixtureError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

std::string json_quote(std::string_view value) {
  std::ostringstream output;
  output << '"';
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
    case '\\':
      output << "\\\\";
      break;
    case '"':
      output << "\\\"";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20U) {
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned int>(character) << std::dec;
      } else {
        output << static_cast<char>(character);
      }
    }
  }
  output << '"';
  return output.str();
}

std::string hexadecimal(std::span<const std::byte> bytes) {
  static constexpr std::array digits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result(bytes.size() * 2U, '0');
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    const auto value = std::to_integer<unsigned int>(bytes[index]);
    result[index * 2U] = digits[value >> 4U];
    result[index * 2U + 1U] = digits[value & 0x0FU];
  }
  return result;
}

void require_cuda(CUresult result, std::string_view operation) {
  if (result == CUDA_SUCCESS) {
    return;
  }
  const char *name = nullptr;
  const char *description = nullptr;
  static_cast<void>(cuGetErrorName(result, &name));
  static_cast<void>(cuGetErrorString(result, &description));
  throw FixtureError(std::string(operation) + ":" + (name == nullptr ? "CUDA_ERROR" : name) + ":" +
                     (description == nullptr ? "unavailable" : description));
}

template <typename Function> Function load_function(void *library, const char *name) {
  void *symbol = dlsym(library, name);
  Function function = nullptr;
  static_assert(sizeof(function) == sizeof(symbol));
  std::memcpy(&function, &symbol, sizeof(function));
  return function;
}

class NvencLibrary {
public:
  NvencLibrary() : handle_(dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL)) {
    if (handle_ == nullptr) {
      throw FixtureError("nvenc_driver_library_unavailable");
    }
    try {
      using GetMaximumVersion = NVENCSTATUS(NVENCAPI *)(std::uint32_t *);
      using CreateInstance = NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
      const auto get_maximum =
          load_function<GetMaximumVersion>(handle_, "NvEncodeAPIGetMaxSupportedVersion");
      const auto create_instance =
          load_function<CreateInstance>(handle_, "NvEncodeAPICreateInstance");
      if (get_maximum == nullptr || create_instance == nullptr) {
        throw FixtureError("nvenc_driver_entry_point_missing");
      }
      std::uint32_t maximum = 0U;
      if (get_maximum(&maximum) != NV_ENC_SUCCESS ||
          !nvenc_api_version_compatible(maximum, NVENCAPI_VERSION)) {
        throw FixtureError("nvenc_compiled_api_too_new");
      }
      functions_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
      if (create_instance(&functions_) != NV_ENC_SUCCESS) {
        throw FixtureError("nvenc_function_table_creation_failed");
      }
    } catch (...) {
      static_cast<void>(dlclose(handle_));
      handle_ = nullptr;
      throw;
    }
  }

  ~NvencLibrary() {
    if (handle_ != nullptr) {
      static_cast<void>(dlclose(handle_));
    }
  }

  NvencLibrary(const NvencLibrary &) = delete;
  NvencLibrary &operator=(const NvencLibrary &) = delete;

  NV_ENCODE_API_FUNCTION_LIST &functions() { return functions_; }

private:
  void *handle_ = nullptr;
  NV_ENCODE_API_FUNCTION_LIST functions_{};
};

void require_nvenc(NV_ENCODE_API_FUNCTION_LIST &functions, void *encoder, NVENCSTATUS status,
                   std::string_view operation) {
  if (status == NV_ENC_SUCCESS) {
    return;
  }
  const char *detail = nullptr;
  if (functions.nvEncGetLastErrorString != nullptr && encoder != nullptr) {
    detail = functions.nvEncGetLastErrorString(encoder);
  }
  throw FixtureError(std::string(operation) +
                     ":status=" + std::to_string(static_cast<int>(status)) + ":" +
                     (detail == nullptr ? "unavailable" : detail));
}

struct FrameObservation {
  std::size_t frame_index = 0U;
  std::size_t bytes = 0U;
  double latency_ms = 0.0;
  std::size_t pending_count = 0U;
  double oldest_pending_ms = 0.0;
};

struct EncodeObservation {
  std::uint64_t measurement_bytes = 0U;
  double payload_bps = 0.0;
  std::vector<FrameObservation> frames;
  std::string configuration_sha256;
  std::string configuration_json;
};

struct SurfaceSlot {
  CUdeviceptr device = 0U;
  std::size_t pitch = 0U;
  NV_ENC_REGISTERED_PTR registered = nullptr;
  NV_ENC_INPUT_PTR mapped = nullptr;
  NV_ENC_OUTPUT_PTR output = nullptr;
  std::size_t frame_index = 0U;
  Clock::time_point submitted{};
  bool mapped_active = false;
  bool pending = false;
};

class BrowserFixtureEncoder {
public:
  explicit BrowserFixtureEncoder(CudaPrimaryContext &cuda_context)
      : context_(cuda_context), guard_(context_), library_(), functions_(library_.functions()) {
    if (!guard_.active()) {
      throw FixtureError("nvenc_cuda_context_guard_failed:" + guard_.reason());
    }
    try {
      validate_function_table();
      open();
      configure();
      allocate_slots();
    } catch (...) {
      cleanup();
      throw;
    }
  }

  ~BrowserFixtureEncoder() { cleanup(); }

  BrowserFixtureEncoder(const BrowserFixtureEncoder &) = delete;
  BrowserFixtureEncoder &operator=(const BrowserFixtureEncoder &) = delete;

  EncodeObservation encode(const std::filesystem::path &stream_path) {
    std::ofstream stream(stream_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!stream) {
      throw FixtureError("browser_fixture_stream_open_failed");
    }
    M0BrowserSyntheticSource source;
    std::vector<std::uint8_t> host_frame(M0BrowserSourceGeometry::frame_bytes);
    std::vector<FrameObservation> observations;
    observations.reserve(M0BrowserSourceGeometry::frame_count);
    for (std::size_t frame_index = 0U; frame_index < M0BrowserSourceGeometry::frame_count;
         ++frame_index) {
      while (free_slot() == nullptr) {
        drain_one(stream, observations);
      }
      auto *slot = free_slot();
      if (slot == nullptr) {
        throw FixtureError("nvenc_surface_ring_has_no_free_slot");
      }
      source.generate(frame_index, host_frame);
      upload(*slot, host_frame);
      submit(*slot, frame_index);
      if (!pending_.empty() && last_submit_status_ == NV_ENC_SUCCESS) {
        drain_one(stream, observations);
      }
    }
    flush(stream, observations);
    stream.flush();
    if (!stream || observations.size() != M0BrowserSourceGeometry::frame_count) {
      throw FixtureError("browser_fixture_output_incomplete");
    }
    EncodeObservation result;
    result.frames = std::move(observations);
    for (const auto &frame : result.frames) {
      if (frame.frame_index >= M0BrowserSourceGeometry::warmup_frames) {
        result.measurement_bytes += frame.bytes;
      }
    }
    result.payload_bps = static_cast<double>(result.measurement_bytes) * 8.0 *
                         static_cast<double>(M0BrowserSourceGeometry::frames_per_second) /
                         static_cast<double>(M0BrowserSourceGeometry::measurement_frames);
    result.configuration_sha256 = configuration_sha256_;
    result.configuration_json = configuration_json_;
    return result;
  }

private:
  void validate_function_table() {
    if (functions_.nvEncOpenEncodeSessionEx == nullptr ||
        functions_.nvEncGetEncodePresetConfigEx == nullptr ||
        functions_.nvEncInitializeEncoder == nullptr ||
        functions_.nvEncRegisterResource == nullptr ||
        functions_.nvEncUnregisterResource == nullptr ||
        functions_.nvEncMapInputResource == nullptr ||
        functions_.nvEncUnmapInputResource == nullptr ||
        functions_.nvEncCreateBitstreamBuffer == nullptr ||
        functions_.nvEncDestroyBitstreamBuffer == nullptr ||
        functions_.nvEncEncodePicture == nullptr || functions_.nvEncLockBitstream == nullptr ||
        functions_.nvEncUnlockBitstream == nullptr || functions_.nvEncDestroyEncoder == nullptr) {
      throw FixtureError("nvenc_required_function_missing");
    }
  }

  void open() {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS parameters{};
    parameters.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    parameters.device = reinterpret_cast<void *>(context_.native_handle());
    parameters.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    parameters.apiVersion = NVENCAPI_VERSION;
    require_nvenc(functions_, nullptr, functions_.nvEncOpenEncodeSessionEx(&parameters, &encoder_),
                  "nvEncOpenEncodeSessionEx");
    if (encoder_ == nullptr) {
      throw FixtureError("nvenc_session_open_returned_null");
    }
  }

  void configure() {
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    require_nvenc(functions_, encoder_,
                  functions_.nvEncGetEncodePresetConfigEx(encoder_, NV_ENC_CODEC_H264_GUID,
                                                          NV_ENC_PRESET_P4_GUID,
                                                          NV_ENC_TUNING_INFO_LOW_LATENCY, &preset),
                  "nvEncGetEncodePresetConfigEx");
    config_ = preset.presetCfg;
    config_.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    config_.gopLength = kGopFrames;
    config_.frameIntervalP = 1U;
    config_.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    config_.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;
    config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config_.rcParams.averageBitRate = kRequestedBitrate;
    config_.rcParams.maxBitRate = kRequestedBitrate;
    config_.rcParams.vbvBufferSize = kRequestedBitrate / M0BrowserSourceGeometry::frames_per_second;
    config_.rcParams.vbvInitialDelay = config_.rcParams.vbvBufferSize;
    config_.rcParams.enableAQ = 0U;
    config_.rcParams.aqStrength = 0U;
    config_.rcParams.enableTemporalAQ = 0U;
    config_.rcParams.enableLookahead = 0U;
    config_.rcParams.lookaheadDepth = 0U;
    config_.rcParams.enableNonRefP = 0U;
    config_.rcParams.zeroReorderDelay = 1U;
    config_.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    config_.rcParams.qpMapMode = NV_ENC_QP_MAP_DISABLED;
    auto &h264 = config_.encodeCodecConfig.h264Config;
    h264.level = NV_ENC_LEVEL_H264_31;
    h264.idrPeriod = kGopFrames;
    h264.repeatSPSPPS = 1U;
    h264.enableFillerDataInsertion = 1U;
    h264.chromaFormatIDC = 1U;
    h264.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    h264.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    h264.hierarchicalPFrames = 0U;
    h264.hierarchicalBFrames = 0U;
    h264.numTemporalLayers = 1U;
    h264.maxTemporalLayers = 1U;
    h264.maxNumRefFrames = 1U;
    h264.numRefL0 = NV_ENC_NUM_REF_FRAMES_1;
    h264.numRefL1 = NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
    h264.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
    h264.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
    h264.tfLevel = NV_ENC_TEMPORAL_FILTER_LEVEL_0;
    h264.sliceMode = 0U;
    h264.sliceModeData = 0U;
    auto &vui = h264.h264VUIParameters;
    vui.videoSignalTypePresentFlag = 1U;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0U;
    vui.colourDescriptionPresentFlag = 1U;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    vui.timingInfoPresentFlag = 1U;
    vui.numUnitInTicks = 1U;
    vui.timeScale = 2U * M0BrowserSourceGeometry::frames_per_second;

    initialize_ = {};
    initialize_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize_.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initialize_.presetGUID = NV_ENC_PRESET_P4_GUID;
    initialize_.encodeWidth = M0BrowserSourceGeometry::visible_width;
    initialize_.encodeHeight = M0BrowserSourceGeometry::visible_height;
    initialize_.darWidth = M0BrowserSourceGeometry::visible_width;
    initialize_.darHeight = M0BrowserSourceGeometry::visible_height;
    initialize_.frameRateNum = M0BrowserSourceGeometry::frames_per_second;
    initialize_.frameRateDen = 1U;
    initialize_.enableEncodeAsync = 0U;
    initialize_.enablePTD = 1U;
    initialize_.enableWeightedPrediction = 0U;
    initialize_.enableUniDirectionalB = 0U;
    initialize_.encodeConfig = &config_;
    initialize_.maxEncodeWidth = initialize_.encodeWidth;
    initialize_.maxEncodeHeight = initialize_.encodeHeight;
    initialize_.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;

    serialize_configuration();
    require_nvenc(functions_, encoder_, functions_.nvEncInitializeEncoder(encoder_, &initialize_),
                  "nvEncInitializeEncoder");
  }

  void serialize_configuration() {
    auto normalized = initialize_;
    normalized.encodeConfig = nullptr;
    const auto initialize_bytes = std::as_bytes(std::span(&normalized, 1U));
    const auto config_bytes = std::as_bytes(std::span(&config_, 1U));
    std::vector<std::uint8_t> digest_input;
    digest_input.reserve(initialize_bytes.size() + config_bytes.size());
    for (const auto value : initialize_bytes) {
      digest_input.push_back(std::to_integer<std::uint8_t>(value));
    }
    for (const auto value : config_bytes) {
      digest_input.push_back(std::to_integer<std::uint8_t>(value));
    }
    configuration_sha256_ = sha256_hex(digest_input);
    std::ostringstream output;
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"codec\": \"h264\",\n"
           << "  \"preset\": \"p4\",\n"
           << "  \"tuning\": \"low_latency\",\n"
           << "  \"profile\": \"baseline\",\n"
           << "  \"level_idc\": 31,\n"
           << "  \"width\": " << M0BrowserSourceGeometry::visible_width << ",\n"
           << "  \"height\": " << M0BrowserSourceGeometry::visible_height << ",\n"
           << "  \"frames_per_second\": " << M0BrowserSourceGeometry::frames_per_second << ",\n"
           << "  \"gop_frames\": " << kGopFrames << ",\n"
           << "  \"requested_bps\": " << kRequestedBitrate << ",\n"
           << "  \"vbv_buffer_bits\": " << config_.rcParams.vbvBufferSize << ",\n"
           << "  \"vbv_initial_delay_bits\": " << config_.rcParams.vbvInitialDelay << ",\n"
           << "  \"filler_data_insertion\": true,\n"
           << "  \"qp_map_mode\": \"disabled\",\n"
           << "  \"initialize_struct_hex\": " << json_quote(hexadecimal(initialize_bytes)) << ",\n"
           << "  \"config_struct_hex\": " << json_quote(hexadecimal(config_bytes)) << ",\n"
           << "  \"configuration_sha256\": " << json_quote(configuration_sha256_) << "\n"
           << "}\n";
    configuration_json_ = output.str();
  }

  void allocate_slots() {
    for (auto &slot : slots_) {
      std::size_t pitch = 0U;
      require_cuda(cuMemAllocPitch(&slot.device, &pitch, M0BrowserSourceGeometry::coded_width,
                                   M0BrowserSourceGeometry::coded_height * 3U / 2U, 16U),
                   "cuMemAllocPitch");
      slot.pitch = pitch;
      NV_ENC_REGISTER_RESOURCE resource{};
      resource.version = NV_ENC_REGISTER_RESOURCE_VER;
      resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
      resource.width = M0BrowserSourceGeometry::coded_width;
      resource.height = M0BrowserSourceGeometry::coded_height;
      resource.pitch = static_cast<std::uint32_t>(slot.pitch);
      resource.resourceToRegister =
          reinterpret_cast<void *>(static_cast<std::uintptr_t>(slot.device));
      resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
      resource.bufferUsage = NV_ENC_INPUT_IMAGE;
      require_nvenc(functions_, encoder_, functions_.nvEncRegisterResource(encoder_, &resource),
                    "nvEncRegisterResource");
      slot.registered = resource.registeredResource;
      NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
      bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
      require_nvenc(functions_, encoder_,
                    functions_.nvEncCreateBitstreamBuffer(encoder_, &bitstream),
                    "nvEncCreateBitstreamBuffer");
      slot.output = bitstream.bitstreamBuffer;
    }
  }

  SurfaceSlot *free_slot() {
    const auto found = std::find_if(slots_.begin(), slots_.end(),
                                    [](const SurfaceSlot &slot) { return !slot.pending; });
    return found == slots_.end() ? nullptr : &*found;
  }

  void upload(SurfaceSlot &slot, std::span<const std::uint8_t> frame) {
    if (frame.size() != M0BrowserSourceGeometry::frame_bytes) {
      throw FixtureError("nvenc_input_frame_size_invalid");
    }
    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_HOST;
    copy.srcHost = frame.data();
    copy.srcPitch = M0BrowserSourceGeometry::coded_width;
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = slot.device;
    copy.dstPitch = slot.pitch;
    copy.WidthInBytes = M0BrowserSourceGeometry::coded_width;
    copy.Height = M0BrowserSourceGeometry::coded_height;
    require_cuda(cuMemcpy2D(&copy), "cuMemcpy2D_luma");
    copy.srcHost = frame.data() + M0BrowserSourceGeometry::luma_bytes;
    copy.dstDevice = slot.device + slot.pitch * M0BrowserSourceGeometry::coded_height;
    copy.Height = M0BrowserSourceGeometry::coded_height / 2U;
    require_cuda(cuMemcpy2D(&copy), "cuMemcpy2D_chroma");
  }

  void submit(SurfaceSlot &slot, std::size_t frame_index) {
    if (slot.pending || slot.mapped_active) {
      throw FixtureError("nvenc_slot_not_free_before_submit");
    }
    NV_ENC_MAP_INPUT_RESOURCE mapping{};
    mapping.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapping.registeredResource = slot.registered;
    require_nvenc(functions_, encoder_, functions_.nvEncMapInputResource(encoder_, &mapping),
                  "nvEncMapInputResource");
    slot.mapped = mapping.mappedResource;
    slot.mapped_active = true;
    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = M0BrowserSourceGeometry::visible_width;
    picture.inputHeight = M0BrowserSourceGeometry::visible_height;
    picture.inputPitch = static_cast<std::uint32_t>(slot.pitch);
    picture.frameIdx = static_cast<std::uint32_t>(frame_index);
    picture.inputTimeStamp = frame_index;
    picture.inputDuration = 1U;
    picture.inputBuffer = slot.mapped;
    picture.outputBitstream = slot.output;
    picture.bufferFmt = mapping.mappedBufferFmt;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    if (frame_index % kGopFrames == 0U) {
      picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    slot.frame_index = frame_index;
    slot.submitted = Clock::now();
    NVENCSTATUS status = NV_ENC_ERR_ENCODER_BUSY;
    for (std::size_t retry = 0U; retry <= kMaximumBusyRetries; ++retry) {
      status = functions_.nvEncEncodePicture(encoder_, &picture);
      if (status != NV_ENC_ERR_ENCODER_BUSY) {
        break;
      }
      if (retry == kMaximumBusyRetries) {
        throw FixtureError("nvEncEncodePicture_busy_retry_limit");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
      require_nvenc(functions_, encoder_, status, "nvEncEncodePicture");
    }
    slot.pending = true;
    pending_.push_back(&slot);
    last_submit_status_ = status;
  }

  void drain_one(std::ofstream &stream, std::vector<FrameObservation> &observations) {
    if (pending_.empty()) {
      throw FixtureError("nvenc_pending_fifo_empty");
    }
    SurfaceSlot &slot = *pending_.front();
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = slot.output;
    lock.doNotWait = 0U;
    require_nvenc(functions_, encoder_, functions_.nvEncLockBitstream(encoder_, &lock),
                  "nvEncLockBitstream");
    const auto available = Clock::now();
    if (lock.outputTimeStamp != slot.frame_index || lock.bitstreamBufferPtr == nullptr ||
        lock.bitstreamSizeInBytes == 0U) {
      static_cast<void>(functions_.nvEncUnlockBitstream(encoder_, slot.output));
      throw FixtureError("nvenc_output_identity_or_payload_invalid");
    }
    const auto bytes = std::span(static_cast<const std::uint8_t *>(lock.bitstreamBufferPtr),
                                 lock.bitstreamSizeInBytes);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (slot.frame_index == 0U) {
      validate_first_access_unit(bytes);
    }
    require_nvenc(functions_, encoder_, functions_.nvEncUnlockBitstream(encoder_, slot.output),
                  "nvEncUnlockBitstream");
    require_nvenc(functions_, encoder_, functions_.nvEncUnmapInputResource(encoder_, slot.mapped),
                  "nvEncUnmapInputResource");
    slot.mapped_active = false;
    slot.mapped = nullptr;
    slot.pending = false;
    pending_.pop_front();
    double oldest = 0.0;
    if (!pending_.empty()) {
      oldest = std::chrono::duration<double, std::milli>(available - pending_.front()->submitted)
                   .count();
    }
    observations.push_back(
        {slot.frame_index, bytes.size(),
         std::chrono::duration<double, std::milli>(available - slot.submitted).count(),
         pending_.size(), oldest});
  }

  void validate_first_access_unit(std::span<const std::uint8_t> bytes) {
    const auto parsed = parse_annex_b_access_unit(bytes);
    if (!parsed.passed || !parsed.access_unit.starts_with_parameter_sets_and_idr()) {
      throw FixtureError("nvenc_initial_access_unit_missing_sps_pps_idr");
    }
    const auto sps =
        std::find_if(parsed.access_unit.nal_units.begin(), parsed.access_unit.nal_units.end(),
                     [](const AnnexBNalUnit &unit) { return unit.unit_type == 7U; });
    if (sps == parsed.access_unit.nal_units.end()) {
      throw FixtureError("nvenc_initial_sps_missing");
    }
    const auto information = parse_h264_sps(parsed.access_unit.payload(*sps));
    if (!information.passed) {
      throw FixtureError("nvenc_initial_sps_invalid:" + information.reason);
    }
    const auto compatibility =
        validate_recording_profile_sps(information.info, M0BrowserSourceGeometry::visible_width,
                                       M0BrowserSourceGeometry::visible_height);
    if (!compatibility.compatible) {
      throw FixtureError("nvenc_initial_sps_incompatible:" + compatibility.reason);
    }
    if (information.info.profile_level.level_idc != 31U) {
      throw FixtureError("nvenc_initial_sps_level_mismatch");
    }
  }

  void flush(std::ofstream &stream, std::vector<FrameObservation> &observations) {
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    require_nvenc(functions_, encoder_, functions_.nvEncEncodePicture(encoder_, &eos),
                  "nvEncEncodePicture_eos");
    while (!pending_.empty()) {
      drain_one(stream, observations);
    }
  }

  void cleanup() noexcept {
    if (encoder_ == nullptr) {
      return;
    }
    for (auto &slot : slots_) {
      if (slot.mapped_active && functions_.nvEncUnmapInputResource != nullptr) {
        static_cast<void>(functions_.nvEncUnmapInputResource(encoder_, slot.mapped));
      }
      if (slot.output != nullptr && functions_.nvEncDestroyBitstreamBuffer != nullptr) {
        static_cast<void>(functions_.nvEncDestroyBitstreamBuffer(encoder_, slot.output));
      }
      if (slot.registered != nullptr && functions_.nvEncUnregisterResource != nullptr) {
        static_cast<void>(functions_.nvEncUnregisterResource(encoder_, slot.registered));
      }
      if (slot.device != 0U) {
        static_cast<void>(cuMemFree(slot.device));
      }
      slot = {};
    }
    if (functions_.nvEncDestroyEncoder != nullptr) {
      static_cast<void>(functions_.nvEncDestroyEncoder(encoder_));
    }
    encoder_ = nullptr;
  }

  CudaPrimaryContext &context_;
  ScopedCudaContext guard_;
  NvencLibrary library_;
  NV_ENCODE_API_FUNCTION_LIST &functions_;
  void *encoder_ = nullptr;
  NV_ENC_CONFIG config_{};
  NV_ENC_INITIALIZE_PARAMS initialize_{};
  std::array<SurfaceSlot, kSurfaceCount> slots_{};
  std::deque<SurfaceSlot *> pending_;
  NVENCSTATUS last_submit_status_ = NV_ENC_SUCCESS;
  std::string configuration_sha256_;
  std::string configuration_json_;
};

bool read_exact(int descriptor, std::span<std::uint8_t> destination) {
  std::size_t offset = 0U;
  while (offset < destination.size()) {
    const auto count = ::read(descriptor, destination.data() + offset, destination.size() - offset);
    if (count == 0) {
      return false;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw FixtureError("ffmpeg_decode_pipe_read_failed");
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

void independently_decode(const std::filesystem::path &stream_path) {
  std::array<int, 2U> descriptors{};
  if (pipe(descriptors.data()) != 0) {
    throw FixtureError("browser_fixture_decode_pipe_creation_failed");
  }
  const pid_t child = fork();
  if (child < 0) {
    static_cast<void>(close(descriptors[0]));
    static_cast<void>(close(descriptors[1]));
    throw FixtureError("browser_fixture_decode_fork_failed");
  }
  if (child == 0) {
    static_cast<void>(close(descriptors[0]));
    if (dup2(descriptors[1], STDOUT_FILENO) < 0) {
      _exit(126);
    }
    static_cast<void>(close(descriptors[1]));
    execlp("ffmpeg", "ffmpeg", "-v", "error", "-f", "h264", "-i", stream_path.c_str(), "-an", "-sn",
           "-dn", "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "yuv420p", "pipe:1",
           static_cast<char *>(nullptr));
    _exit(127);
  }

  static_cast<void>(close(descriptors[1]));
  std::vector<std::uint8_t> decoded(M0BrowserSourceGeometry::frame_bytes);
  bool complete = true;
  try {
    for (std::size_t frame_index = 0U; frame_index < M0BrowserSourceGeometry::frame_count;
         ++frame_index) {
      if (!read_exact(descriptors[0], decoded)) {
        complete = false;
        break;
      }
    }
    if (complete) {
      std::uint8_t trailing = 0U;
      ssize_t trailing_count = -1;
      do {
        trailing_count = ::read(descriptors[0], &trailing, 1U);
      } while (trailing_count < 0 && errno == EINTR);
      if (trailing_count != 0) {
        complete = false;
      }
    }
  } catch (...) {
    static_cast<void>(close(descriptors[0]));
    int ignored_status = 0;
    static_cast<void>(waitpid(child, &ignored_status, 0));
    throw;
  }
  static_cast<void>(close(descriptors[0]));
  int child_status = 0;
  if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
      WEXITSTATUS(child_status) != 0 || !complete) {
    throw FixtureError("browser_fixture_independent_decode_incomplete_or_failed");
  }
}

void write_fixture(const std::filesystem::path &directory, const M0ProtocolLock &protocol,
                   const EncodeObservation &observation) {
  const auto stream_path = directory / "nvenc-browser-720p30.h264";
  const auto table_path = directory / "nvenc-browser-720p30-frames.tsv";
  const auto configuration_path = directory / "nvenc-browser-720p30-configuration.json";

  std::ofstream table(table_path, std::ios::out | std::ios::trunc);
  if (!table) {
    throw FixtureError("browser_fixture_frame_table_open_failed");
  }
  table << "frame_index\tbytes\tlatency_ms\tpending_count\toldest_pending_ms\n";
  for (const auto &frame : observation.frames) {
    table << frame.frame_index << '\t' << frame.bytes << '\t' << std::setprecision(17)
          << frame.latency_ms << '\t' << frame.pending_count << '\t' << frame.oldest_pending_ms
          << '\n';
  }
  table.flush();
  if (!table) {
    throw FixtureError("browser_fixture_frame_table_write_failed");
  }

  std::ofstream configuration(configuration_path, std::ios::out | std::ios::trunc);
  configuration << observation.configuration_json;
  configuration.flush();
  if (!configuration) {
    throw FixtureError("browser_fixture_configuration_write_failed");
  }

  M0BrowserSyntheticSource source;
  std::ofstream summary(directory / "browser-fixture-summary.json",
                        std::ios::out | std::ios::trunc);
  if (!summary) {
    throw FixtureError("browser_fixture_summary_open_failed");
  }
  summary << "{\n"
          << "  \"schema_version\": 1,\n"
          << "  \"protocol\": \"m0_nvenc_browser_fixture_v1\",\n"
          << "  \"status\": \"PASSED\",\n"
          << "  \"manifest_sha256\": " << json_quote(protocol.manifest_sha256) << ",\n"
          << "  \"presentation\": \"sharing_720p30\",\n"
          << "  \"width\": " << M0BrowserSourceGeometry::visible_width << ",\n"
          << "  \"height\": " << M0BrowserSourceGeometry::visible_height << ",\n"
          << "  \"frames_per_second\": " << M0BrowserSourceGeometry::frames_per_second << ",\n"
          << "  \"level_idc\": 31,\n"
          << "  \"warmup_frames\": " << M0BrowserSourceGeometry::warmup_frames << ",\n"
          << "  \"measurement_frames\": " << M0BrowserSourceGeometry::measurement_frames << ",\n"
          << "  \"frame_count\": " << observation.frames.size() << ",\n"
          << "  \"requested_bps\": " << kRequestedBitrate << ",\n"
          << "  \"measurement_bytes\": " << observation.measurement_bytes << ",\n"
          << "  \"payload_bps\": " << std::setprecision(17) << observation.payload_bps << ",\n"
          << "  \"configuration_sha256\": " << json_quote(observation.configuration_sha256) << ",\n"
          << "  \"stream_sha256\": " << json_quote(sha256_file_hex(stream_path)) << ",\n"
          << "  \"frame_table_sha256\": " << json_quote(sha256_file_hex(table_path)) << ",\n"
          << "  \"configuration_file_sha256\": " << json_quote(sha256_file_hex(configuration_path))
          << ",\n"
          << "  \"source_sha256\": {\n"
          << "    \"frame_0\": " << json_quote(sha256_hex(source.generate(0U))) << ",\n"
          << "    \"frame_300\": "
          << json_quote(sha256_hex(source.generate(M0BrowserSourceGeometry::warmup_frames)))
          << ",\n"
          << "    \"frame_2099\": "
          << json_quote(sha256_hex(source.generate(M0BrowserSourceGeometry::frame_count - 1U)))
          << "\n"
          << "  },\n"
          << "  \"independent_decoder\": \"ffmpeg_raw_yuv420p_exact_frame_count\"\n"
          << "}\n";
  summary.flush();
  if (!summary) {
    throw FixtureError("browser_fixture_summary_write_failed");
  }
}

void write_failure(const std::filesystem::path &directory, std::string_view reason) noexcept {
  try {
    std::ofstream output(directory / "FAILED.json", std::ios::out | std::ios::trunc);
    output << "{\"schema_version\":1,\"status\":\"FAILED\",\"reason\":" << json_quote(reason)
           << "}\n";
  } catch (...) {
  }
}

} // namespace
#endif

M0BenchmarkResult run_m0_nvenc_browser_fixture(const M0BrowserFixtureRequest &request) {
#if GLYPHRELAY_HAS_NVENC
  if (request.protocol.manifest_sha256 != GLYPHRELAY_M0_PROTOCOL_SHA256) {
    return {M0BenchmarkStatus::failed, "m0_protocol_identity_not_compiled"};
  }
  if (request.output_directory.empty() ||
      request.output_directory != request.output_directory.lexically_normal()) {
    return {M0BenchmarkStatus::failed, "browser_fixture_output_path_invalid"};
  }
  try {
    CudaPrimaryContext context(0);
    if (!context.available()) {
      return {M0BenchmarkStatus::unsupported,
              "browser_fixture_cuda_primary_context_unavailable:" + context.reason()};
    }
    const auto capabilities = probe_nvenc_capabilities(context);
    if (!capabilities.passed) {
      const auto reason = "browser_fixture_nvenc_capability_unavailable:" + capabilities.reason;
      if (!context.shutdown()) {
        return {M0BenchmarkStatus::failed,
                reason + ":cuda_context_shutdown_failed:" + context.reason()};
      }
      return {M0BenchmarkStatus::unsupported, reason};
    }
    if (!std::filesystem::create_directory(request.output_directory)) {
      throw FixtureError("browser_fixture_output_creation_failed");
    }
    const auto stream = request.output_directory / "nvenc-browser-720p30.h264";
    EncodeObservation observation;
    {
      BrowserFixtureEncoder encoder(context);
      observation = encoder.encode(stream);
    }
    independently_decode(stream);
    write_fixture(request.output_directory, request.protocol, observation);
    if (!context.shutdown()) {
      throw FixtureError("browser_fixture_cuda_context_shutdown_failed:" + context.reason());
    }
    std::ofstream marker(request.output_directory / "PASSED", std::ios::out);
    marker << request.protocol.manifest_sha256 << '\n';
    marker.flush();
    if (!marker) {
      throw FixtureError("browser_fixture_completion_marker_write_failed");
    }
    return {M0BenchmarkStatus::passed, "m0_nvenc_browser_fixture_passed"};
  } catch (const std::exception &error) {
    if (std::filesystem::is_directory(request.output_directory)) {
      write_failure(request.output_directory, error.what());
    }
    return {M0BenchmarkStatus::failed, error.what()};
  }
#else
  static_cast<void>(request);
  return {M0BenchmarkStatus::unsupported, "nvenc_browser_fixture_backend_not_built"};
#endif
}

} // namespace glyphrelay
