#include "glyphrelay/nvenc_benchmark.hpp"

#include "glyphrelay/benchmark_gate.hpp"
#include "glyphrelay/cuda_context.hpp"
#include "glyphrelay/nvenc_probe.hpp"
#include "glyphrelay/synthetic_source.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if GLYPHRELAY_HAS_NVENC
#include "glyphrelay/annex_b.hpp"
#include "glyphrelay/h264_sps.hpp"
#include "glyphrelay/quality_metrics.hpp"
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

bool nvenc_benchmark_backend_available() {
#if GLYPHRELAY_HAS_NVENC
  return true;
#else
  return false;
#endif
}

#if GLYPHRELAY_HAS_NVENC
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kMinimumRequestedBitrate = 800'000U;
constexpr std::uint32_t kMaximumRequestedBitrate = 1'200'000U;
constexpr std::uint32_t kDesiredPayloadBitrate = 1'000'000U;
constexpr std::size_t kCalibrationIterations = 8U;
constexpr std::size_t kCalibrationFrames =
    M0SourceGeometry::warmup_frames + 30U * M0SourceGeometry::frames_per_second;
constexpr std::size_t kMeasuredRepeats = 10U;
constexpr std::size_t kSurfaceCount = 4U;
constexpr std::size_t kMaximumBusyRetries = 100U;

class BenchmarkError final : public std::runtime_error {
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
  for (std::size_t index = 0; index < bytes.size(); ++index) {
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
  throw BenchmarkError(std::string(operation) + ":" + (name == nullptr ? "CUDA_ERROR" : name) +
                       ":" + (description == nullptr ? "unavailable" : description));
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
      throw BenchmarkError("nvenc_driver_library_unavailable");
    }
    using GetMaximumVersion = NVENCSTATUS(NVENCAPI *)(std::uint32_t *);
    using CreateInstance = NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
    const auto get_maximum =
        load_function<GetMaximumVersion>(handle_, "NvEncodeAPIGetMaxSupportedVersion");
    const auto create_instance =
        load_function<CreateInstance>(handle_, "NvEncodeAPICreateInstance");
    if (get_maximum == nullptr || create_instance == nullptr) {
      throw BenchmarkError("nvenc_driver_entry_point_missing");
    }
    std::uint32_t maximum = 0U;
    if (get_maximum(&maximum) != NV_ENC_SUCCESS ||
        !nvenc_api_version_compatible(maximum, NVENCAPI_VERSION)) {
      throw BenchmarkError("nvenc_compiled_api_too_new");
    }
    functions_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create_instance(&functions_) != NV_ENC_SUCCESS) {
      throw BenchmarkError("nvenc_function_table_creation_failed");
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
  throw BenchmarkError(std::string(operation) +
                       ":status=" + std::to_string(static_cast<int>(status)) + ":" +
                       (detail == nullptr ? "unavailable" : detail));
}

struct EncodedFrame {
  std::size_t frame_index = 0U;
  std::size_t bytes = 0U;
  double latency_ms = 0.0;
  std::size_t pending_count = 0U;
  double oldest_pending_ms = 0.0;
};

struct DecodedFrameQuality {
  std::size_t frame_index = 0U;
  M0QualityMetrics metrics;
};

struct QualityObservation {
  std::vector<DecodedFrameQuality> frames;
  double whole_frame_psnr_db = 0.0;
  double protected_psnr_db = 0.0;
  double comparison_psnr_db = 0.0;
  double protected_minus_comparison_db = 0.0;
};

struct EncodeObservation {
  std::uint32_t requested_bps = 0U;
  std::uint64_t measurement_bytes = 0U;
  double payload_bps = 0.0;
  std::vector<EncodedFrame> frames;
  std::string configuration_sha256;
  std::string configuration_json;
  QualityObservation quality;
};

struct SurfaceSlot {
  CUdeviceptr device = 0U;
  std::size_t pitch = 0U;
  NV_ENC_REGISTERED_PTR registered = nullptr;
  NV_ENC_INPUT_PTR mapped = nullptr;
  NV_ENC_OUTPUT_PTR output = nullptr;
  std::int8_t *map = nullptr;
  std::size_t frame_index = 0U;
  Clock::time_point submitted{};
  bool mapped_active = false;
  bool pending = false;
};

class Encoder {
public:
  Encoder(CudaPrimaryContext &cuda_context, std::uint32_t requested_bps, bool emphasis)
      : context_(cuda_context), guard_(context_), requested_bps_(requested_bps),
        emphasis_(emphasis), library_(), functions_(library_.functions()) {
    if (!guard_.active()) {
      throw BenchmarkError("nvenc_cuda_context_guard_failed:" + guard_.reason());
    }
    validate_function_table();
    open();
    configure();
    allocate_slots();
  }

  ~Encoder() { cleanup(); }

  Encoder(const Encoder &) = delete;
  Encoder &operator=(const Encoder &) = delete;

  EncodeObservation encode(std::size_t frame_count, const std::filesystem::path *stream_path) {
    if (frame_count <= M0SourceGeometry::warmup_frames ||
        frame_count > M0SourceGeometry::frame_count) {
      throw BenchmarkError("benchmark_frame_count_invalid");
    }
    std::ofstream stream;
    if (stream_path != nullptr) {
      stream.open(*stream_path, std::ios::binary | std::ios::out | std::ios::trunc);
      if (!stream) {
        throw BenchmarkError("benchmark_stream_open_failed");
      }
    }
    M0SyntheticSource source;
    std::vector<std::uint8_t> host_frame(M0SourceGeometry::frame_bytes);
    std::vector<EncodedFrame> observations;
    observations.reserve(frame_count);
    for (std::size_t frame_index = 0U; frame_index < frame_count; ++frame_index) {
      while (free_slot() == nullptr) {
        drain_one(stream, observations);
      }
      auto *slot = free_slot();
      if (slot == nullptr) {
        throw BenchmarkError("nvenc_surface_ring_has_no_free_slot");
      }
      source.generate(frame_index, host_frame);
      upload(*slot, host_frame);
      submit(*slot, frame_index);
      if (!pending_.empty() && last_submit_status_ == NV_ENC_SUCCESS) {
        drain_one(stream, observations);
      }
    }
    flush(stream, observations);
    if (stream) {
      stream.flush();
      if (!stream) {
        throw BenchmarkError("benchmark_stream_write_failed");
      }
    }
    if (observations.size() != frame_count) {
      throw BenchmarkError("nvenc_output_frame_count_mismatch");
    }
    EncodeObservation result;
    result.requested_bps = requested_bps_;
    result.frames = std::move(observations);
    for (const auto &frame : result.frames) {
      if (frame.frame_index >= M0SourceGeometry::warmup_frames) {
        result.measurement_bytes += frame.bytes;
      }
    }
    const auto measurement_frames = frame_count - M0SourceGeometry::warmup_frames;
    result.payload_bps = static_cast<double>(result.measurement_bytes) * 8.0 *
                         static_cast<double>(M0SourceGeometry::frames_per_second) /
                         static_cast<double>(measurement_frames);
    result.configuration_json = configuration_json_;
    result.configuration_sha256 = configuration_sha256_;
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
      throw BenchmarkError("nvenc_required_function_missing");
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
      throw BenchmarkError("nvenc_session_open_returned_null");
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
    config_.gopLength = 60U;
    config_.frameIntervalP = 1;
    config_.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    config_.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;
    config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config_.rcParams.averageBitRate = requested_bps_;
    config_.rcParams.maxBitRate = requested_bps_;
    config_.rcParams.vbvBufferSize =
        std::max<std::uint32_t>(1U, requested_bps_ / M0SourceGeometry::frames_per_second);
    config_.rcParams.vbvInitialDelay = config_.rcParams.vbvBufferSize;
    config_.rcParams.enableAQ = 0U;
    config_.rcParams.aqStrength = 0U;
    config_.rcParams.enableTemporalAQ = 0U;
    config_.rcParams.enableLookahead = 0U;
    config_.rcParams.lookaheadDepth = 0U;
    config_.rcParams.enableNonRefP = 0U;
    config_.rcParams.zeroReorderDelay = 1U;
    config_.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    config_.rcParams.qpMapMode = emphasis_ ? NV_ENC_QP_MAP_EMPHASIS : NV_ENC_QP_MAP_DISABLED;
    auto &h264 = config_.encodeCodecConfig.h264Config;
    h264.level = NV_ENC_LEVEL_H264_4;
    h264.idrPeriod = 60U;
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
    vui.timeScale = 2U * static_cast<std::uint32_t>(M0SourceGeometry::frames_per_second);

    initialize_ = {};
    initialize_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize_.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initialize_.presetGUID = NV_ENC_PRESET_P4_GUID;
    initialize_.encodeWidth = static_cast<std::uint32_t>(M0SourceGeometry::visible_width);
    initialize_.encodeHeight = static_cast<std::uint32_t>(M0SourceGeometry::visible_height);
    initialize_.darWidth = static_cast<std::uint32_t>(M0SourceGeometry::visible_width);
    initialize_.darHeight = static_cast<std::uint32_t>(M0SourceGeometry::visible_height);
    initialize_.frameRateNum = static_cast<std::uint32_t>(M0SourceGeometry::frames_per_second);
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
           << "  \"level_idc\": 40,\n"
           << "  \"requested_bps\": " << requested_bps_ << ",\n"
           << "  \"vbv_buffer_bits\": " << config_.rcParams.vbvBufferSize << ",\n"
           << "  \"vbv_initial_delay_bits\": " << config_.rcParams.vbvInitialDelay << ",\n"
           << "  \"filler_data_insertion\": true,\n"
           << "  \"qp_map_mode\": " << json_quote(emphasis_ ? "emphasis" : "disabled") << ",\n"
           << "  \"initialize_struct_hex\": " << json_quote(hexadecimal(initialize_bytes)) << ",\n"
           << "  \"config_struct_hex\": " << json_quote(hexadecimal(config_bytes)) << ",\n"
           << "  \"configuration_sha256\": " << json_quote(configuration_sha256_) << "\n"
           << "}\n";
    configuration_json_ = output.str();
  }

  void allocate_slots() {
    const auto fixed_map = m0_fixed_emphasis_map();
    for (auto &slot : slots_) {
      std::size_t pitch = 0U;
      require_cuda(cuMemAllocPitch(&slot.device, &pitch, M0SourceGeometry::coded_width,
                                   M0SourceGeometry::coded_height * 3U / 2U, 16U),
                   "cuMemAllocPitch");
      slot.pitch = pitch;
      NV_ENC_REGISTER_RESOURCE resource{};
      resource.version = NV_ENC_REGISTER_RESOURCE_VER;
      resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
      resource.width = static_cast<std::uint32_t>(M0SourceGeometry::coded_width);
      resource.height = static_cast<std::uint32_t>(M0SourceGeometry::coded_height);
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
      void *map = nullptr;
      require_cuda(cuMemHostAlloc(&map, M0SourceGeometry::map_entries, 0U), "cuMemHostAlloc");
      slot.map = static_cast<std::int8_t *>(map);
      std::copy(fixed_map.begin(), fixed_map.end(), slot.map);
    }
  }

  SurfaceSlot *free_slot() {
    const auto found = std::find_if(slots_.begin(), slots_.end(),
                                    [](const SurfaceSlot &slot) { return !slot.pending; });
    return found == slots_.end() ? nullptr : &*found;
  }

  void upload(SurfaceSlot &slot, std::span<const std::uint8_t> frame) {
    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_HOST;
    copy.srcHost = frame.data();
    copy.srcPitch = M0SourceGeometry::coded_width;
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = slot.device;
    copy.dstPitch = slot.pitch;
    copy.WidthInBytes = M0SourceGeometry::coded_width;
    copy.Height = M0SourceGeometry::coded_height;
    require_cuda(cuMemcpy2D(&copy), "cuMemcpy2D_luma");
    copy.srcHost = frame.data() + M0SourceGeometry::luma_bytes;
    copy.dstDevice = slot.device + slot.pitch * M0SourceGeometry::coded_height;
    copy.Height = M0SourceGeometry::coded_height / 2U;
    require_cuda(cuMemcpy2D(&copy), "cuMemcpy2D_chroma");
  }

  void submit(SurfaceSlot &slot, std::size_t frame_index) {
    if (slot.pending || slot.mapped_active) {
      throw BenchmarkError("nvenc_slot_not_free_before_submit");
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
    picture.inputWidth = static_cast<std::uint32_t>(M0SourceGeometry::visible_width);
    picture.inputHeight = static_cast<std::uint32_t>(M0SourceGeometry::visible_height);
    picture.inputPitch = static_cast<std::uint32_t>(slot.pitch);
    picture.frameIdx = static_cast<std::uint32_t>(frame_index);
    picture.inputTimeStamp = frame_index;
    picture.inputDuration = 1U;
    picture.inputBuffer = slot.mapped;
    picture.outputBitstream = slot.output;
    picture.bufferFmt = mapping.mappedBufferFmt;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    if (frame_index % 60U == 0U) {
      picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    if (emphasis_) {
      picture.qpDeltaMap = slot.map;
      picture.qpDeltaMapSize = static_cast<std::uint32_t>(M0SourceGeometry::map_entries);
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
        throw BenchmarkError("nvEncEncodePicture_busy_retry_limit");
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

  void drain_one(std::ofstream &stream, std::vector<EncodedFrame> &observations) {
    if (pending_.empty()) {
      throw BenchmarkError("nvenc_pending_fifo_empty");
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
      throw BenchmarkError("nvenc_output_identity_or_payload_invalid");
    }
    const auto bytes = std::span(static_cast<const std::uint8_t *>(lock.bitstreamBufferPtr),
                                 lock.bitstreamSizeInBytes);
    if (stream) {
      stream.write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
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
      throw BenchmarkError("nvenc_initial_access_unit_missing_sps_pps_idr");
    }
    const auto sps =
        std::find_if(parsed.access_unit.nal_units.begin(), parsed.access_unit.nal_units.end(),
                     [](const AnnexBNalUnit &unit) { return unit.unit_type == 7U; });
    if (sps == parsed.access_unit.nal_units.end()) {
      throw BenchmarkError("nvenc_initial_sps_missing");
    }
    const auto information = parse_h264_sps(parsed.access_unit.payload(*sps));
    if (!information.passed) {
      throw BenchmarkError("nvenc_initial_sps_invalid:" + information.reason);
    }
    const auto compatibility = validate_recording_profile_sps(
        information.info, M0SourceGeometry::visible_width, M0SourceGeometry::visible_height);
    if (!compatibility.compatible) {
      throw BenchmarkError("nvenc_initial_sps_incompatible:" + compatibility.reason);
    }
  }

  void flush(std::ofstream &stream, std::vector<EncodedFrame> &observations) {
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
      if (slot.map != nullptr) {
        static_cast<void>(cuMemFreeHost(slot.map));
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
  std::uint32_t requested_bps_;
  bool emphasis_;
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

struct CalibrationPoint {
  std::uint32_t requested_bps = 0U;
  double measured_bps = 0.0;
  std::string configuration_sha256;
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
      throw BenchmarkError("ffmpeg_decode_pipe_read_failed");
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

QualityObservation decode_and_measure(const std::filesystem::path &stream_path) {
  std::array<int, 2U> descriptors{};
  if (pipe(descriptors.data()) != 0) {
    throw BenchmarkError("ffmpeg_decode_pipe_creation_failed");
  }
  const pid_t child = fork();
  if (child < 0) {
    static_cast<void>(close(descriptors[0]));
    static_cast<void>(close(descriptors[1]));
    throw BenchmarkError("ffmpeg_decode_fork_failed");
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
  QualityObservation result;
  result.frames.reserve(M0SourceGeometry::measurement_frames);
  constexpr std::size_t decoded_frame_bytes =
      M0SourceGeometry::visible_width * M0SourceGeometry::visible_height * 3U / 2U;
  std::vector<std::uint8_t> decoded(decoded_frame_bytes);
  std::vector<std::uint8_t> reference(M0SourceGeometry::frame_bytes);
  M0SyntheticSource source;
  bool complete = true;
  try {
    for (std::size_t frame_index = 0U; frame_index < M0SourceGeometry::frame_count; ++frame_index) {
      if (!read_exact(descriptors[0], decoded)) {
        complete = false;
        break;
      }
      if (frame_index >= M0SourceGeometry::warmup_frames) {
        source.generate(frame_index, reference);
        auto metrics = compute_m0_quality_metrics(reference, M0SourceGeometry::coded_width, decoded,
                                                  M0SourceGeometry::visible_width);
        result.frames.push_back({frame_index, metrics});
      }
    }
    std::uint8_t trailing = 0U;
    if (complete && ::read(descriptors[0], &trailing, 1U) != 0) {
      complete = false;
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
      WEXITSTATUS(child_status) != 0 || !complete ||
      result.frames.size() != M0SourceGeometry::measurement_frames) {
    throw BenchmarkError("ffmpeg_independent_decode_incomplete_or_failed");
  }
  const auto mean = [&result](auto accessor) {
    return std::accumulate(result.frames.begin(), result.frames.end(), 0.0,
                           [&accessor](double sum, const DecodedFrameQuality &frame) {
                             return sum + accessor(frame.metrics);
                           }) /
           static_cast<double>(result.frames.size());
  };
  result.whole_frame_psnr_db =
      mean([](const M0QualityMetrics &metrics) { return metrics.whole_frame.psnr_db; });
  result.protected_psnr_db =
      mean([](const M0QualityMetrics &metrics) { return metrics.protected_region.psnr_db; });
  result.comparison_psnr_db =
      mean([](const M0QualityMetrics &metrics) { return metrics.comparison_region.psnr_db; });
  result.protected_minus_comparison_db =
      mean([](const M0QualityMetrics &metrics) { return metrics.protected_minus_comparison_db; });
  if (!std::isfinite(result.whole_frame_psnr_db) || !std::isfinite(result.protected_psnr_db) ||
      !std::isfinite(result.comparison_psnr_db) ||
      !std::isfinite(result.protected_minus_comparison_db)) {
    throw BenchmarkError("benchmark_quality_metric_not_finite");
  }
  return result;
}

struct ConditionResult {
  std::string name;
  bool emphasis = false;
  std::uint32_t selected_bps = 0U;
  std::vector<CalibrationPoint> calibration;
  std::vector<EncodeObservation> runs;
};

struct GateEvaluation {
  bool passed = false;
  std::vector<std::string> failures;
  std::array<double, 2U> mean_payload_bps{};
  double between_condition_payload_difference_fraction = 0.0;
  double protected_psnr_improvement_db = 0.0;
  double spatial_allocation_improvement_db = 0.0;
  std::array<std::vector<M0BenchmarkRunGate>, 2U> runs;
};

GateEvaluation evaluate_gate(const std::array<ConditionResult, 2U> &conditions) {
  GateEvaluation gate;
  std::array<double, 2U> protected_psnr{};
  std::array<double, 2U> spatial_allocation{};
  for (std::size_t condition_index = 0U; condition_index < conditions.size(); ++condition_index) {
    const auto &condition = conditions[condition_index];
    if (condition.runs.size() != kMeasuredRepeats) {
      gate.failures.push_back(condition.name + ":measured_repeat_count_mismatch");
      continue;
    }
    double protected_sum = 0.0;
    double allocation_sum = 0.0;
    const auto selected_calibration =
        std::find_if(condition.calibration.begin(), condition.calibration.end(),
                     [&condition](const CalibrationPoint &point) {
                       return point.requested_bps == condition.selected_bps;
                     });
    std::vector<double> payload_measurements;
    payload_measurements.reserve(condition.runs.size());
    for (std::size_t repeat = 0U; repeat < condition.runs.size(); ++repeat) {
      const auto &run = condition.runs[repeat];
      const auto label = condition.name + ":repeat_" + std::to_string(repeat + 1U);
      bool frame_identities_match = run.frames.size() == M0SourceGeometry::frame_count;
      for (std::size_t index = 0U; frame_identities_match && index < run.frames.size(); ++index) {
        frame_identities_match = run.frames[index].frame_index == index;
      }
      if (!frame_identities_match) {
        gate.failures.push_back(label + ":encoded_frame_identity_mismatch");
        continue;
      }
      if (selected_calibration == condition.calibration.end() ||
          run.configuration_sha256 != selected_calibration->configuration_sha256) {
        gate.failures.push_back(label + ":effective_configuration_changed");
      }
      const auto measured = std::span(run.frames).subspan(M0SourceGeometry::warmup_frames);
      std::vector<M0BenchmarkTimingSample> timing_samples;
      timing_samples.reserve(measured.size());
      for (const auto &frame : measured) {
        timing_samples.push_back({frame.latency_ms, frame.pending_count, frame.oldest_pending_ms});
      }
      const auto run_gate = evaluate_m0_benchmark_run_gate(run.payload_bps, timing_samples);
      gate.runs[condition_index].push_back(run_gate);
      for (const auto &failure : run_gate.failures) {
        gate.failures.push_back(label + ":" + failure);
      }
      payload_measurements.push_back(run.payload_bps);
      protected_sum += run.quality.protected_psnr_db;
      allocation_sum += run.quality.protected_minus_comparison_db;
    }
    gate.mean_payload_bps[condition_index] = m0_payload_mean(payload_measurements);
    if (!m0_payload_mean_within_window(payload_measurements)) {
      gate.failures.push_back(condition.name + ":mean_payload_bitrate_outside_matched_window");
    }
    protected_psnr[condition_index] = protected_sum / static_cast<double>(condition.runs.size());
    spatial_allocation[condition_index] =
        allocation_sum / static_cast<double>(condition.runs.size());
  }
  const auto payload_mean = (gate.mean_payload_bps[0] + gate.mean_payload_bps[1]) / 2.0;
  gate.between_condition_payload_difference_fraction =
      payload_mean == 0.0
          ? std::numeric_limits<double>::infinity()
          : std::abs(gate.mean_payload_bps[1] - gate.mean_payload_bps[0]) / payload_mean;
  gate.protected_psnr_improvement_db = protected_psnr[1] - protected_psnr[0];
  gate.spatial_allocation_improvement_db = spatial_allocation[1] - spatial_allocation[0];
  if (gate.between_condition_payload_difference_fraction > 0.02) {
    gate.failures.push_back("between_condition_payload_difference_exceeded");
  }
  if (gate.protected_psnr_improvement_db < 1.0) {
    gate.failures.push_back("protected_psnr_improvement_below_gate");
  }
  if (gate.spatial_allocation_improvement_db < 0.75) {
    gate.failures.push_back("spatial_allocation_improvement_below_gate");
  }
  gate.passed = gate.failures.empty();
  return gate;
}

CalibrationPoint calibrate_once(CudaPrimaryContext &context, std::uint32_t requested_bps,
                                bool emphasis) {
  Encoder encoder(context, requested_bps, emphasis);
  const auto observation = encoder.encode(kCalibrationFrames, nullptr);
  return {requested_bps, observation.payload_bps, observation.configuration_sha256};
}

ConditionResult calibrate(CudaPrimaryContext &context, std::string name, bool emphasis) {
  ConditionResult result;
  result.name = std::move(name);
  result.emphasis = emphasis;
  std::uint32_t lower = kMinimumRequestedBitrate;
  std::uint32_t upper = kMaximumRequestedBitrate;
  for (std::size_t iteration = 0U; iteration < kCalibrationIterations && lower <= upper;
       ++iteration) {
    const auto requested = lower + (upper - lower) / 2U;
    const auto point = calibrate_once(context, requested, emphasis);
    result.calibration.push_back(point);
    if (point.measured_bps < static_cast<double>(kDesiredPayloadBitrate)) {
      lower = requested + 1U;
    } else if (requested == 0U) {
      break;
    } else {
      upper = requested - 1U;
    }
  }
  if (result.calibration.empty()) {
    throw BenchmarkError("nvenc_calibration_produced_no_points");
  }
  const auto selected = std::min_element(
      result.calibration.begin(), result.calibration.end(),
      [](const CalibrationPoint &left, const CalibrationPoint &right) {
        const auto left_distance = std::abs(left.measured_bps - kDesiredPayloadBitrate);
        const auto right_distance = std::abs(right.measured_bps - kDesiredPayloadBitrate);
        return left_distance < right_distance ||
               (left_distance == right_distance && left.requested_bps < right.requested_bps);
      });
  result.selected_bps = selected->requested_bps;
  return result;
}

void write_observation(const std::filesystem::path &directory, std::string_view condition,
                       std::size_t repeat, const EncodeObservation &observation) {
  const auto stem =
      std::string(condition) + "-repeat-" + (repeat < 9U ? "0" : "") + std::to_string(repeat + 1U);
  std::ofstream frames(directory / (stem + "-frames.tsv"), std::ios::out | std::ios::trunc);
  if (!frames) {
    throw BenchmarkError("benchmark_frame_metrics_open_failed");
  }
  frames << "frame_index\tbytes\tlatency_ms\tpending_count\toldest_pending_ms\n";
  for (const auto &frame : observation.frames) {
    frames << frame.frame_index << '\t' << frame.bytes << '\t' << std::setprecision(17)
           << frame.latency_ms << '\t' << frame.pending_count << '\t' << frame.oldest_pending_ms
           << '\n';
  }
  std::ofstream quality(directory / (stem + "-quality.tsv"), std::ios::out | std::ios::trunc);
  if (!quality) {
    throw BenchmarkError("benchmark_quality_metrics_open_failed");
  }
  quality << "frame_index\twhole_squared_error\twhole_psnr_db\tprotected_squared_error\t"
             "protected_psnr_db\tcomparison_squared_error\tcomparison_psnr_db\t"
             "protected_minus_comparison_db\n";
  for (const auto &frame : observation.quality.frames) {
    quality << frame.frame_index << '\t' << frame.metrics.whole_frame.squared_error << '\t'
            << std::setprecision(17) << frame.metrics.whole_frame.psnr_db << '\t'
            << frame.metrics.protected_region.squared_error << '\t'
            << frame.metrics.protected_region.psnr_db << '\t'
            << frame.metrics.comparison_region.squared_error << '\t'
            << frame.metrics.comparison_region.psnr_db << '\t'
            << frame.metrics.protected_minus_comparison_db << '\n';
  }
  std::ofstream configuration(directory / (stem + "-configuration.json"),
                              std::ios::out | std::ios::trunc);
  configuration << observation.configuration_json;
  if (!frames || !quality || !configuration) {
    throw BenchmarkError("benchmark_observation_write_failed");
  }
}

void write_summary(const std::filesystem::path &directory, const M0ProtocolLock &protocol,
                   const std::array<ConditionResult, 2U> &conditions) {
  std::ofstream output(directory / "encoder-summary.json", std::ios::out | std::ios::trunc);
  if (!output) {
    throw BenchmarkError("benchmark_summary_open_failed");
  }
  output << "{\n  \"schema_version\": 1,\n  \"protocol\": \"m0_fixed_map_v1\",\n"
         << "  \"manifest_sha256\": " << json_quote(protocol.manifest_sha256) << ",\n"
         << "  \"conditions\": [\n";
  for (std::size_t condition_index = 0U; condition_index < conditions.size(); ++condition_index) {
    const auto &condition = conditions[condition_index];
    output << "    {\n      \"name\": " << json_quote(condition.name)
           << ",\n      \"selected_requested_bps\": " << condition.selected_bps
           << ",\n      \"calibration\": [\n";
    for (std::size_t index = 0U; index < condition.calibration.size(); ++index) {
      const auto &point = condition.calibration[index];
      output << "        {\"requested_bps\": " << point.requested_bps
             << ", \"measured_bps\": " << std::setprecision(17) << point.measured_bps
             << ", \"configuration_sha256\": " << json_quote(point.configuration_sha256) << "}"
             << (index + 1U == condition.calibration.size() ? "\n" : ",\n");
    }
    output << "      ],\n      \"runs\": [\n";
    for (std::size_t index = 0U; index < condition.runs.size(); ++index) {
      const auto &run = condition.runs[index];
      output << "        {\"repeat\": " << index + 1U
             << ", \"measurement_bytes\": " << run.measurement_bytes
             << ", \"payload_bps\": " << std::setprecision(17) << run.payload_bps
             << ", \"configuration_sha256\": " << json_quote(run.configuration_sha256)
             << ", \"whole_frame_psnr_db\": " << run.quality.whole_frame_psnr_db
             << ", \"protected_psnr_db\": " << run.quality.protected_psnr_db
             << ", \"comparison_psnr_db\": " << run.quality.comparison_psnr_db
             << ", \"protected_minus_comparison_db\": " << run.quality.protected_minus_comparison_db
             << "}" << (index + 1U == condition.runs.size() ? "\n" : ",\n");
    }
    output << "      ]\n    }" << (condition_index + 1U == conditions.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  if (!output) {
    throw BenchmarkError("benchmark_summary_write_failed");
  }
}

void write_gate(const std::filesystem::path &directory, const GateEvaluation &gate) {
  std::ofstream output(directory / "gate.json", std::ios::out | std::ios::trunc);
  if (!output) {
    throw BenchmarkError("benchmark_gate_open_failed");
  }
  output << "{\n  \"schema_version\": 1,\n  \"status\": "
         << json_quote(gate.passed ? "PASSED" : "FAILED") << ",\n"
         << "  \"mean_payload_bps\": {\"controlled_uniform\": " << std::setprecision(17)
         << gate.mean_payload_bps[0] << ", \"fixed_emphasis_level_4\": " << gate.mean_payload_bps[1]
         << "},\n"
         << "  \"between_condition_payload_difference_fraction\": "
         << gate.between_condition_payload_difference_fraction << ",\n"
         << "  \"protected_psnr_improvement_db\": " << gate.protected_psnr_improvement_db << ",\n"
         << "  \"spatial_allocation_improvement_db\": " << gate.spatial_allocation_improvement_db
         << ",\n"
         << "  \"failures\": [";
  for (std::size_t index = 0U; index < gate.failures.size(); ++index) {
    output << (index == 0U ? "\n    " : ",\n    ") << json_quote(gate.failures[index]);
  }
  output << (gate.failures.empty() ? "" : "\n  ") << "],\n  \"runs\": {\n";
  constexpr std::array<std::string_view, 2U> names = {"controlled_uniform",
                                                      "fixed_emphasis_level_4"};
  for (std::size_t condition = 0U; condition < names.size(); ++condition) {
    output << "    " << json_quote(names[condition]) << ": [\n";
    for (std::size_t repeat = 0U; repeat < gate.runs[condition].size(); ++repeat) {
      const auto &run = gate.runs[condition][repeat];
      output << "      {\"repeat\": " << repeat + 1U << ", \"payload_bps\": " << run.payload_bps
             << ", \"latency_p95_ms\": " << run.latency_p95_ms
             << ", \"latency_p99_ms\": " << run.latency_p99_ms
             << ", \"maximum_pending_age_ms\": " << run.maximum_pending_age_ms
             << ", \"first_quarter_pending_mean\": " << run.first_quarter_pending_mean
             << ", \"last_quarter_pending_mean\": " << run.last_quarter_pending_mean << "}"
             << (repeat + 1U == gate.runs[condition].size() ? "\n" : ",\n");
    }
    output << "    ]" << (condition + 1U == names.size() ? "\n" : ",\n");
  }
  output << "  }\n}\n";
  if (!output) {
    throw BenchmarkError("benchmark_gate_write_failed");
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

M0BenchmarkResult run_m0_nvenc_benchmark(const M0BenchmarkRequest &request) {
#if GLYPHRELAY_HAS_NVENC
  if (request.protocol.manifest_sha256 != GLYPHRELAY_M0_PROTOCOL_SHA256) {
    return {M0BenchmarkStatus::failed, "m0_protocol_identity_not_compiled"};
  }
  if (request.output_directory.empty() ||
      request.output_directory != request.output_directory.lexically_normal()) {
    return {M0BenchmarkStatus::failed, "benchmark_output_path_invalid"};
  }
  try {
    CudaPrimaryContext context(0);
    if (!context.available()) {
      return {M0BenchmarkStatus::unsupported,
              "benchmark_cuda_primary_context_unavailable:" + context.reason()};
    }
    const auto capabilities = probe_nvenc_capabilities(context);
    if (!capabilities.passed) {
      const auto reason = "benchmark_nvenc_capability_unavailable:" + capabilities.reason;
      if (!context.shutdown()) {
        return {M0BenchmarkStatus::failed,
                reason + ":cuda_context_shutdown_failed:" + context.reason()};
      }
      return {M0BenchmarkStatus::unsupported, reason};
    }
    if (!std::filesystem::create_directory(request.output_directory)) {
      throw BenchmarkError("benchmark_output_creation_failed");
    }
    std::array<ConditionResult, 2U> conditions = {
        calibrate(context, "controlled_uniform", false),
        calibrate(context, "fixed_emphasis_level_4", true),
    };
    for (std::size_t repeat = 0U; repeat < kMeasuredRepeats; ++repeat) {
      for (auto &condition : conditions) {
        const auto stream =
            request.output_directory / (condition.name + "-repeat-" + (repeat < 9U ? "0" : "") +
                                        std::to_string(repeat + 1U) + ".h264");
        Encoder encoder(context, condition.selected_bps, condition.emphasis);
        auto observation = encoder.encode(M0SourceGeometry::frame_count, &stream);
        observation.quality = decode_and_measure(stream);
        write_observation(request.output_directory, condition.name, repeat, observation);
        condition.runs.push_back(std::move(observation));
      }
    }
    write_summary(request.output_directory, request.protocol, conditions);
    const auto gate = evaluate_gate(conditions);
    write_gate(request.output_directory, gate);
    if (!gate.passed) {
      throw BenchmarkError("m0_fixed_map_acceptance_gate_failed");
    }
    if (!context.shutdown()) {
      throw BenchmarkError("benchmark_cuda_context_shutdown_failed:" + context.reason());
    }
    std::ofstream marker(request.output_directory / "PASSED", std::ios::out);
    marker << request.protocol.manifest_sha256 << '\n';
    marker.flush();
    if (!marker) {
      throw BenchmarkError("benchmark_completion_marker_write_failed");
    }
    return {M0BenchmarkStatus::passed, "m0_nvenc_benchmark_gate_passed"};
  } catch (const std::exception &error) {
    if (std::filesystem::is_directory(request.output_directory)) {
      write_failure(request.output_directory, error.what());
    }
    return {M0BenchmarkStatus::failed, error.what()};
  }
#else
  static_cast<void>(request);
  return {M0BenchmarkStatus::unsupported, "nvenc_benchmark_backend_not_built"};
#endif
}

} // namespace glyphrelay
