#include "glyphrelay/nvenc_encoder.hpp"

#include "glyphrelay/annex_b.hpp"
#include "glyphrelay/cuda_context.hpp"
#include "glyphrelay/nvenc_probe.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if GLYPHRELAY_HAS_NVENC
#include <cuda.h>
#include <dlfcn.h>
#include <ffnvcodec/nvEncodeAPI.h>
#endif

namespace glyphrelay {
namespace {

bool valid_configuration(const NvencEncoderConfig &configuration) {
  if (configuration.width == 0U || configuration.height == 0U || configuration.width > 16'384U ||
      configuration.height > 16'384U || (configuration.width & 1U) != 0U ||
      (configuration.height & 1U) != 0U || configuration.frames_per_second == 0U ||
      configuration.frames_per_second > 240U || configuration.target_bitrate_bps == 0U ||
      configuration.maximum_bitrate_bps < configuration.target_bitrate_bps ||
      configuration.gop_frames == 0U || configuration.level_idc == 0U ||
      configuration.capacity == 0U || configuration.capacity > 64U ||
      configuration.maximum_busy_retries == 0U || configuration.maximum_busy_retries > 10'000U) {
    return false;
  }
  const auto map_entries =
      ((configuration.width + 15U) / 16U) * ((configuration.height + 15U) / 16U);
  if (configuration.mode == NvencFrameMode::fixed_emphasis) {
    return configuration.fixed_emphasis_map.size() == map_entries &&
           std::all_of(configuration.fixed_emphasis_map.begin(),
                       configuration.fixed_emphasis_map.end(),
                       [](std::int8_t level) { return level >= 0 && level <= 5; });
  }
  return configuration.fixed_emphasis_map.empty();
}

} // namespace

#if GLYPHRELAY_HAS_NVENC
namespace {

class NvencRuntimeError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

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
      throw NvencRuntimeError("nvenc_driver_library_unavailable");
    }
    try {
      using GetMaximumVersion = NVENCSTATUS(NVENCAPI *)(std::uint32_t *);
      using CreateInstance = NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
      const auto get_maximum =
          load_function<GetMaximumVersion>(handle_, "NvEncodeAPIGetMaxSupportedVersion");
      const auto create_instance =
          load_function<CreateInstance>(handle_, "NvEncodeAPICreateInstance");
      if (get_maximum == nullptr || create_instance == nullptr) {
        throw NvencRuntimeError("nvenc_driver_entry_point_missing");
      }
      std::uint32_t maximum = 0U;
      if (get_maximum(&maximum) != NV_ENC_SUCCESS ||
          !nvenc_api_version_compatible(maximum, NVENCAPI_VERSION)) {
        throw NvencRuntimeError("nvenc_compiled_api_too_new");
      }
      functions_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
      if (create_instance(&functions_) != NV_ENC_SUCCESS) {
        throw NvencRuntimeError("nvenc_function_table_creation_failed");
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

NV_ENC_LEVEL h264_level(std::uint32_t level_idc) {
  switch (level_idc) {
  case 31U:
    return NV_ENC_LEVEL_H264_31;
  case 40U:
    return NV_ENC_LEVEL_H264_4;
  case 41U:
    return NV_ENC_LEVEL_H264_41;
  case 42U:
    return NV_ENC_LEVEL_H264_42;
  case 50U:
    return NV_ENC_LEVEL_H264_5;
  case 51U:
    return NV_ENC_LEVEL_H264_51;
  case 52U:
    return NV_ENC_LEVEL_H264_52;
  default:
    throw NvencRuntimeError("nvenc_h264_level_unsupported");
  }
}

NvencSubmitStatus submit_status(NVENCSTATUS status) {
  switch (status) {
  case NV_ENC_SUCCESS:
    return NvencSubmitStatus::success;
  case NV_ENC_ERR_NEED_MORE_INPUT:
    return NvencSubmitStatus::need_more_input;
  case NV_ENC_ERR_ENCODER_BUSY:
    return NvencSubmitStatus::encoder_busy;
  default:
    return NvencSubmitStatus::fatal;
  }
}

} // namespace

struct NvencEncoder::Implementation {
  struct Slot {
    NV_ENC_REGISTERED_PTR registered = nullptr;
    NV_ENC_INPUT_PTR mapped = nullptr;
    NV_ENC_OUTPUT_PTR output = nullptr;
    std::uintptr_t registered_pointer = 0U;
    std::int8_t *fixed_map = nullptr;
    std::optional<NvencEncodeInput> input;
    bool mapped_active = false;
  };

  Implementation(std::shared_ptr<CudaPrimaryContext> selected_context,
                 CudaPreprocessor &selected_preprocessor, NvencEncoderConfig selected_configuration,
                 NvencOutputCallback selected_output)
      : context(std::move(selected_context)), preprocessor(selected_preprocessor),
        configuration(std::move(selected_configuration)), output(std::move(selected_output)),
        coordinator(configuration.capacity, configuration.maximum_busy_retries),
        slots(configuration.capacity) {
    try {
      initialize();
      worker = std::thread([this] { output_loop(); });
    } catch (const std::exception &error) {
      reason = error.what();
      fatal = true;
      cleanup_driver();
    }
  }

  ~Implementation() { static_cast<void>(close()); }

  void require_status(NVENCSTATUS status, std::string_view operation) {
    if (status == NV_ENC_SUCCESS) {
      return;
    }
    const char *detail = nullptr;
    if (functions != nullptr && functions->nvEncGetLastErrorString != nullptr &&
        encoder != nullptr) {
      detail = functions->nvEncGetLastErrorString(encoder);
    }
    throw NvencRuntimeError(std::string(operation) +
                            ":status=" + std::to_string(static_cast<int>(status)) + ":" +
                            (detail == nullptr ? "unavailable" : detail));
  }

  void validate_functions() {
    if (functions->nvEncOpenEncodeSessionEx == nullptr ||
        functions->nvEncGetEncodePresetConfigEx == nullptr ||
        functions->nvEncGetEncodeCaps == nullptr || functions->nvEncInitializeEncoder == nullptr ||
        functions->nvEncRegisterResource == nullptr ||
        functions->nvEncUnregisterResource == nullptr ||
        functions->nvEncMapInputResource == nullptr ||
        functions->nvEncUnmapInputResource == nullptr ||
        functions->nvEncCreateBitstreamBuffer == nullptr ||
        functions->nvEncDestroyBitstreamBuffer == nullptr ||
        functions->nvEncEncodePicture == nullptr || functions->nvEncLockBitstream == nullptr ||
        functions->nvEncUnlockBitstream == nullptr || functions->nvEncDestroyEncoder == nullptr) {
      throw NvencRuntimeError("nvenc_required_function_missing");
    }
  }

  void initialize() {
    if (!valid_configuration(configuration)) {
      throw NvencRuntimeError("nvenc_encoder_configuration_invalid");
    }
    if (!context || !context->available() ||
        preprocessor.context_identity() != context->identity()) {
      throw NvencRuntimeError("nvenc_shared_cuda_context_invalid");
    }
    ScopedCudaContext guard(*context);
    if (!guard.active()) {
      throw NvencRuntimeError("nvenc_cuda_context_guard_failed:" + guard.reason());
    }
    library = std::make_unique<NvencLibrary>();
    functions = &library->functions();
    validate_functions();
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.device = reinterpret_cast<void *>(context->native_handle());
    open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    open.apiVersion = NVENCAPI_VERSION;
    require_status(functions->nvEncOpenEncodeSessionEx(&open, &encoder),
                   "nvEncOpenEncodeSessionEx");
    if (encoder == nullptr) {
      throw NvencRuntimeError("nvenc_session_open_returned_null");
    }
    if (configuration.mode != NvencFrameMode::uniform) {
      NV_ENC_CAPS_PARAM parameter{};
      parameter.version = NV_ENC_CAPS_PARAM_VER;
      parameter.capsToQuery = NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP;
      int supported = 0;
      require_status(
          functions->nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_H264_GUID, &parameter, &supported),
          "nvEncGetEncodeCaps_emphasis");
      if (supported != 1) {
        throw NvencRuntimeError("nvenc_emphasis_map_unsupported");
      }
    }
    configure();
    for (auto &slot : slots) {
      NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
      bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
      require_status(functions->nvEncCreateBitstreamBuffer(encoder, &bitstream),
                     "nvEncCreateBitstreamBuffer");
      slot.output = bitstream.bitstreamBuffer;
      if (configuration.mode == NvencFrameMode::fixed_emphasis) {
        void *map = nullptr;
        if (cuMemHostAlloc(&map, configuration.fixed_emphasis_map.size(), 0U) != CUDA_SUCCESS ||
            map == nullptr) {
          throw NvencRuntimeError("nvenc_fixed_map_pinned_allocation_failed");
        }
        slot.fixed_map = static_cast<std::int8_t *>(map);
        std::copy(configuration.fixed_emphasis_map.begin(), configuration.fixed_emphasis_map.end(),
                  slot.fixed_map);
      }
    }
    admission_open = true;
    reason = "nvenc_encoder_ready";
  }

  void configure() {
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    require_status(functions->nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_H264_GUID,
                                                           NV_ENC_PRESET_P4_GUID,
                                                           NV_ENC_TUNING_INFO_LOW_LATENCY, &preset),
                   "nvEncGetEncodePresetConfigEx");
    config = preset.presetCfg;
    config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    config.gopLength = configuration.gop_frames;
    config.frameIntervalP = 1U;
    config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    config.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config.rcParams.averageBitRate = configuration.target_bitrate_bps;
    config.rcParams.maxBitRate = configuration.maximum_bitrate_bps;
    config.rcParams.vbvBufferSize =
        std::max(1U, configuration.maximum_bitrate_bps / configuration.frames_per_second);
    config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
    config.rcParams.enableAQ = 0U;
    config.rcParams.aqStrength = 0U;
    config.rcParams.enableTemporalAQ = 0U;
    config.rcParams.enableLookahead = 0U;
    config.rcParams.lookaheadDepth = 0U;
    config.rcParams.enableNonRefP = 0U;
    config.rcParams.zeroReorderDelay = 1U;
    config.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    config.rcParams.qpMapMode = configuration.mode == NvencFrameMode::uniform
                                    ? NV_ENC_QP_MAP_DISABLED
                                    : NV_ENC_QP_MAP_EMPHASIS;
    auto &h264 = config.encodeCodecConfig.h264Config;
    h264.level = h264_level(configuration.level_idc);
    h264.idrPeriod = configuration.gop_frames;
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
    vui.timeScale = configuration.frames_per_second * 2U;

    initialize_parameters = {};
    initialize_parameters.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize_parameters.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initialize_parameters.presetGUID = NV_ENC_PRESET_P4_GUID;
    initialize_parameters.encodeWidth = static_cast<std::uint32_t>(configuration.width);
    initialize_parameters.encodeHeight = static_cast<std::uint32_t>(configuration.height);
    initialize_parameters.darWidth = static_cast<std::uint32_t>(configuration.width);
    initialize_parameters.darHeight = static_cast<std::uint32_t>(configuration.height);
    initialize_parameters.frameRateNum = configuration.frames_per_second;
    initialize_parameters.frameRateDen = 1U;
    initialize_parameters.enableEncodeAsync = 0U;
    initialize_parameters.enablePTD = 1U;
    initialize_parameters.enableWeightedPrediction = 0U;
    initialize_parameters.enableUniDirectionalB = 0U;
    initialize_parameters.encodeConfig = &config;
    initialize_parameters.maxEncodeWidth = initialize_parameters.encodeWidth;
    initialize_parameters.maxEncodeHeight = initialize_parameters.encodeHeight;
    initialize_parameters.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    require_status(functions->nvEncInitializeEncoder(encoder, &initialize_parameters),
                   "nvEncInitializeEncoder");
  }

  NvencSubmissionRequest request_for(const NvencEncodeInput &input, Slot &slot) const {
    NvencSubmissionRequest request{
        .submission_slot_id = input.ticket.token.surface_slot,
        .submission_sequence = input.submission_sequence,
        .output_bitstream = reinterpret_cast<std::uintptr_t>(slot.output),
        .mode = configuration.mode,
        .force_idr = input.force_idr,
        .surface = input.completion.surface,
        .emphasis_map = {},
    };
    if (configuration.mode == NvencFrameMode::automatic_emphasis) {
      request.emphasis_map = input.completion.emphasis_map;
    } else if (configuration.mode == NvencFrameMode::fixed_emphasis) {
      request.emphasis_map = {
          .frame_id = input.completion.surface.frame_id,
          .geometry_epoch = input.completion.surface.geometry_epoch,
          .context = input.completion.surface.context,
          .memory_space = MemorySpace::host_pinned,
          .host_pointer = reinterpret_cast<std::uintptr_t>(slot.fixed_map),
          .macroblock_width = (configuration.width + 15U) / 16U,
          .macroblock_height = (configuration.height + 15U) / 16U,
          .byte_size = configuration.fixed_emphasis_map.size(),
          .values = {slot.fixed_map, configuration.fixed_emphasis_map.size()},
          .device_to_host_ready = true,
      };
    }
    return request;
  }

  void register_surface(Slot &slot, const Nv12SurfaceDescriptor &surface) {
    if (slot.registered != nullptr) {
      if (slot.registered_pointer != surface.device_pointer) {
        throw NvencRuntimeError("nvenc_submission_slot_surface_changed");
      }
      return;
    }
    if (std::any_of(slots.begin(), slots.end(), [&](const Slot &candidate) {
          return candidate.registered != nullptr &&
                 candidate.registered_pointer == surface.device_pointer;
        })) {
      throw NvencRuntimeError("nvenc_surface_registered_by_multiple_slots");
    }
    NV_ENC_REGISTER_RESOURCE resource{};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    resource.width = static_cast<std::uint32_t>(surface.coded_width);
    resource.height = static_cast<std::uint32_t>(surface.coded_height);
    resource.pitch = static_cast<std::uint32_t>(surface.pitch);
    resource.resourceToRegister = reinterpret_cast<void *>(surface.device_pointer);
    resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    require_status(functions->nvEncRegisterResource(encoder, &resource), "nvEncRegisterResource");
    slot.registered = resource.registeredResource;
    slot.registered_pointer = surface.device_pointer;
  }

  NVENCSTATUS invoke_submit(Slot &slot, const NvencSubmissionRequest &request,
                            const NvencEncodeInput &input) {
    if (!slot.mapped_active) {
      NV_ENC_MAP_INPUT_RESOURCE mapping{};
      mapping.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
      mapping.registeredResource = slot.registered;
      require_status(functions->nvEncMapInputResource(encoder, &mapping), "nvEncMapInputResource");
      slot.mapped = mapping.mappedResource;
      slot.mapped_active = true;
      slot_mapped_formats[request.submission_slot_id] = mapping.mappedBufferFmt;
    }
    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = static_cast<std::uint32_t>(configuration.width);
    picture.inputHeight = static_cast<std::uint32_t>(configuration.height);
    picture.inputPitch = static_cast<std::uint32_t>(request.surface.pitch);
    picture.frameIdx = static_cast<std::uint32_t>(input.submission_sequence);
    picture.inputTimeStamp = input.presentation_timestamp_ns;
    picture.inputDuration = input.duration_ns;
    picture.inputBuffer = slot.mapped;
    picture.outputBitstream = slot.output;
    picture.bufferFmt = slot_mapped_formats[request.submission_slot_id];
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    if (request.force_idr) {
      picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    if (request.mode != NvencFrameMode::uniform) {
      picture.qpDeltaMap = const_cast<std::int8_t *>(request.emphasis_map.values.data());
      picture.qpDeltaMapSize = static_cast<std::uint32_t>(request.emphasis_map.byte_size);
    }
    return functions->nvEncEncodePicture(encoder, &picture);
  }

  NvencEncoderOperation submit(NvencEncodeInput input) {
    std::unique_lock lock(mutex);
    if (!admission_open || fatal || end_of_stream || !input.ticket.passed ||
        !input.completion.passed || input.submission_sequence == 0U || input.duration_ns == 0U ||
        input.dependency_epoch == 0U || input.ticket.token.surface_slot >= slots.size()) {
      return {false, "nvenc_encode_input_invalid_or_closed"};
    }
    const auto slot_id = input.ticket.token.surface_slot;
    auto &slot = slots[slot_id];
    if (slot.input) {
      return {false, "nvenc_encode_slot_already_owned"};
    }
    if (input.completion.surface.coded_width != configuration.width ||
        input.completion.surface.coded_height != configuration.height ||
        input.completion.visible_width != configuration.width ||
        input.completion.visible_height != configuration.height ||
        input.completion.surface.context != context->identity()) {
      return {false, "nvenc_encode_surface_contract_mismatch"};
    }
    slot.input = std::move(input);
    try {
      ScopedCudaContext guard(*context);
      if (!guard.active()) {
        throw NvencRuntimeError("nvenc_cuda_context_guard_failed:" + guard.reason());
      }
      register_surface(slot, slot.input->completion.surface);
      const auto request = request_for(*slot.input, slot);
      const auto validation = validate_nvenc_submission(request);
      if (!validation.passed) {
        const auto ticket = slot.input->ticket;
        slot.input.reset();
        if (slot.mapped_active) {
          static_cast<void>(functions->nvEncUnmapInputResource(encoder, slot.mapped));
          slot.mapped = nullptr;
          slot.mapped_active = false;
        }
        static_cast<void>(preprocessor.abort(ticket));
        return {false, validation.reason};
      }
      SubmissionOperation operation;
      do {
        operation = coordinator.submit(request, [&] {
          const auto status = invoke_submit(slot, request, *slot.input);
          return submit_status(status);
        });
        if (operation.passed && operation.state == SubmissionState::submit_retry_pending) {
          ++busy_retries;
          lock.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          lock.lock();
        }
      } while (operation.passed && operation.state == SubmissionState::submit_retry_pending);
      if (!operation.passed) {
        fail_locked(operation.reason);
        condition.notify_all();
        return {false, reason};
      }
      const auto marked = preprocessor.mark_submitted(slot.input->ticket);
      if (!marked.passed) {
        fail_locked("nvenc_preprocessor_submit_transition_failed");
        condition.notify_all();
        return {false, reason};
      }
      ++accepted_frames;
      reason = operation.reason;
      condition.notify_all();
      return {true, operation.reason};
    } catch (const std::exception &error) {
      fail_locked(error.what());
      condition.notify_all();
      return {false, reason};
    }
  }

  bool output_ready_locked() const {
    const auto pending = coordinator.pending_fifo();
    return !pending.empty() &&
           coordinator.state(pending.front()) == SubmissionState::bitstream_lockable;
  }

  void output_loop() {
    while (true) {
      std::size_t slot_id = 0U;
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] {
          return fatal || output_ready_locked() ||
                 (end_of_stream && coordinator.active_slots() == 0U);
        });
        if (fatal || (end_of_stream && coordinator.active_slots() == 0U)) {
          return;
        }
        const auto selected = coordinator.begin_bitstream_lock();
        if (!selected) {
          continue;
        }
        slot_id = *selected;
      }

      NvencEncodedFrame encoded;
      std::string failure;
      auto &slot = slots[slot_id];
      try {
        ScopedCudaContext guard(*context);
        if (!guard.active()) {
          throw NvencRuntimeError("nvenc_output_context_guard_failed:" + guard.reason());
        }
        NV_ENC_LOCK_BITSTREAM bitstream{};
        bitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
        bitstream.outputBitstream = slot.output;
        bitstream.doNotWait = 0U;
        require_status(functions->nvEncLockBitstream(encoder, &bitstream), "nvEncLockBitstream");
        if (!slot.input || bitstream.bitstreamBufferPtr == nullptr ||
            bitstream.bitstreamSizeInBytes == 0U ||
            bitstream.outputTimeStamp != slot.input->presentation_timestamp_ns) {
          static_cast<void>(functions->nvEncUnlockBitstream(encoder, slot.output));
          throw NvencRuntimeError("nvenc_output_identity_or_payload_invalid");
        }
        encoded.annex_b.assign(static_cast<const std::uint8_t *>(bitstream.bitstreamBufferPtr),
                               static_cast<const std::uint8_t *>(bitstream.bitstreamBufferPtr) +
                                   bitstream.bitstreamSizeInBytes);
        encoded.frame_id = slot.input->completion.surface.frame_id;
        encoded.submission_sequence = slot.input->submission_sequence;
        encoded.presentation_timestamp_ns = slot.input->presentation_timestamp_ns;
        encoded.dependency_epoch = slot.input->dependency_epoch;
        encoded.keyframe = bitstream.pictureType == NV_ENC_PIC_TYPE_IDR;
        const auto parsed = parse_annex_b_access_unit(encoded.annex_b);
        if (!parsed.passed) {
          static_cast<void>(functions->nvEncUnlockBitstream(encoder, slot.output));
          throw NvencRuntimeError("nvenc_output_annex_b_invalid:" + parsed.reason);
        }
        encoded.parameter_sets_present =
            parsed.access_unit.contains(7U) && parsed.access_unit.contains(8U);
        require_status(functions->nvEncUnlockBitstream(encoder, slot.output),
                       "nvEncUnlockBitstream");
        require_status(functions->nvEncUnmapInputResource(encoder, slot.mapped),
                       "nvEncUnmapInputResource");
        slot.mapped = nullptr;
        slot.mapped_active = false;
        const auto released = preprocessor.mark_encoder_input_released(slot.input->ticket);
        const auto recycled = preprocessor.release(slot.input->ticket);
        if (!released.passed || !recycled.passed) {
          throw NvencRuntimeError("nvenc_preprocessor_output_release_failed");
        }
      } catch (const std::exception &error) {
        failure = error.what();
      }

      {
        std::scoped_lock lock(mutex);
        if (!failure.empty()) {
          fail_locked(std::move(failure));
          condition.notify_all();
          return;
        }
        const auto completed = coordinator.complete_bitstream(slot_id);
        if (!completed.passed) {
          fail_locked(completed.reason);
          condition.notify_all();
          return;
        }
        slot.input.reset();
        ++completed_frames;
        reason = "nvenc_output_complete";
        condition.notify_all();
      }
      try {
        if (output) {
          output(std::move(encoded));
        }
      } catch (...) {
        std::scoped_lock lock(mutex);
        fail_locked("nvenc_output_callback_threw");
        condition.notify_all();
        return;
      }
    }
  }

  NvencEncoderOperation flush() {
    std::unique_lock lock(mutex);
    if (fatal) {
      return {false, reason};
    }
    if (end_of_stream) {
      return coordinator.active_slots() == 0U
                 ? NvencEncoderOperation{true, "nvenc_flush_already_complete"}
                 : NvencEncoderOperation{false, "nvenc_flush_already_in_progress"};
    }
    admission_open = false;
    preprocessor.close_admission();
    try {
      ScopedCudaContext guard(*context);
      if (!guard.active()) {
        throw NvencRuntimeError("nvenc_eos_context_guard_failed:" + guard.reason());
      }
      NV_ENC_PIC_PARAMS eos{};
      eos.version = NV_ENC_PIC_PARAMS_VER;
      eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
      require_status(functions->nvEncEncodePicture(encoder, &eos), "nvEncEncodePicture_eos");
      const auto operation = coordinator.begin_end_of_stream();
      if (!operation.passed) {
        throw NvencRuntimeError(operation.reason);
      }
      end_of_stream = true;
      reason = operation.reason;
      condition.notify_all();
      condition.wait(lock, [&] { return fatal || coordinator.active_slots() == 0U; });
      return fatal ? NvencEncoderOperation{false, reason}
                   : NvencEncoderOperation{true, "nvenc_flush_complete"};
    } catch (const std::exception &error) {
      fail_locked(error.what());
      condition.notify_all();
      return {false, reason};
    }
  }

  void fail_locked(std::string failure_reason) {
    if (!fatal) {
      fatal = true;
      admission_open = false;
      reason = std::move(failure_reason);
      preprocessor.close_admission();
      static_cast<void>(coordinator.fail());
    }
  }

  NvencEncoderOperation close() {
    bool should_flush = false;
    {
      std::scoped_lock lock(mutex);
      if (closed) {
        return {true, "nvenc_encoder_already_closed"};
      }
      should_flush = !fatal && !end_of_stream;
    }
    if (should_flush) {
      static_cast<void>(flush());
    }
    {
      std::scoped_lock lock(mutex);
      condition.notify_all();
    }
    if (worker.joinable()) {
      worker.join();
    }
    cleanup_driver();
    std::scoped_lock lock(mutex);
    closed = true;
    admission_open = false;
    return fatal ? NvencEncoderOperation{false, reason}
                 : NvencEncoderOperation{true, "nvenc_encoder_closed"};
  }

  void cleanup_driver() noexcept {
    if (encoder == nullptr) {
      return;
    }
    const bool failed = fatal;
    ScopedCudaContext guard(*context);
    if (!guard.active()) {
      fatal = true;
      reason = "nvenc_cleanup_context_guard_failed";
      return;
    }
    if (failed) {
      if (functions->nvEncDestroyEncoder != nullptr) {
        static_cast<void>(functions->nvEncDestroyEncoder(encoder));
      }
      encoder = nullptr;
    }
    for (auto &slot : slots) {
      if (!failed && slot.mapped_active && functions->nvEncUnmapInputResource != nullptr) {
        static_cast<void>(functions->nvEncUnmapInputResource(encoder, slot.mapped));
      }
      if (!failed && slot.output != nullptr && functions->nvEncDestroyBitstreamBuffer != nullptr) {
        static_cast<void>(functions->nvEncDestroyBitstreamBuffer(encoder, slot.output));
      }
      if (!failed && slot.registered != nullptr && functions->nvEncUnregisterResource != nullptr) {
        static_cast<void>(functions->nvEncUnregisterResource(encoder, slot.registered));
      }
      if (slot.input) {
        static_cast<void>(preprocessor.abort(slot.input->ticket));
      }
      if (slot.fixed_map != nullptr) {
        static_cast<void>(cuMemFreeHost(slot.fixed_map));
      }
      slot = {};
    }
    if (!failed && encoder != nullptr && functions->nvEncDestroyEncoder != nullptr) {
      static_cast<void>(functions->nvEncDestroyEncoder(encoder));
      encoder = nullptr;
    }
    functions = nullptr;
    library.reset();
  }

  NvencEncoderDiagnostics diagnostics() const {
    std::scoped_lock lock(mutex);
    return {
        .available = admission_open && !fatal && !closed,
        .admission_open = admission_open,
        .end_of_stream = end_of_stream,
        .fatal = fatal,
        .capacity = slots.size(),
        .active_submissions = coordinator.active_slots(),
        .accepted_frames = accepted_frames,
        .completed_frames = completed_frames,
        .busy_retries = busy_retries,
        .reason = reason,
    };
  }

  std::shared_ptr<CudaPrimaryContext> context;
  CudaPreprocessor &preprocessor;
  NvencEncoderConfig configuration;
  NvencOutputCallback output;
  NvencSubmissionCoordinator coordinator;
  std::vector<Slot> slots;
  std::vector<NV_ENC_BUFFER_FORMAT> slot_mapped_formats =
      std::vector<NV_ENC_BUFFER_FORMAT>(configuration.capacity, NV_ENC_BUFFER_FORMAT_UNDEFINED);
  std::unique_ptr<NvencLibrary> library;
  NV_ENCODE_API_FUNCTION_LIST *functions = nullptr;
  void *encoder = nullptr;
  NV_ENC_CONFIG config{};
  NV_ENC_INITIALIZE_PARAMS initialize_parameters{};
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::thread worker;
  std::uint64_t accepted_frames = 0U;
  std::uint64_t completed_frames = 0U;
  std::uint64_t busy_retries = 0U;
  std::string reason = "nvenc_encoder_initializing";
  bool admission_open = false;
  bool end_of_stream = false;
  bool fatal = false;
  bool closed = false;
};

#else

struct NvencEncoder::Implementation {
  Implementation(std::shared_ptr<CudaPrimaryContext>, CudaPreprocessor &,
                 NvencEncoderConfig selected_configuration, NvencOutputCallback)
      : configuration(std::move(selected_configuration)) {
    reason = valid_configuration(configuration) ? "nvenc_encoder_not_built"
                                                : "nvenc_encoder_configuration_invalid";
  }

  NvencEncoderOperation submit(NvencEncodeInput) { return {false, reason}; }
  NvencEncoderOperation flush() { return {false, reason}; }
  NvencEncoderOperation close() {
    closed = true;
    return {true, "nvenc_encoder_stub_closed"};
  }
  NvencEncoderDiagnostics diagnostics() const {
    return {
        .available = false,
        .admission_open = false,
        .end_of_stream = false,
        .fatal = false,
        .capacity = configuration.capacity,
        .active_submissions = 0U,
        .accepted_frames = 0U,
        .completed_frames = 0U,
        .busy_retries = 0U,
        .reason = reason,
    };
  }

  NvencEncoderConfig configuration;
  std::string reason;
  bool closed = false;
};

#endif

NvencEncoder::NvencEncoder(std::shared_ptr<CudaPrimaryContext> context,
                           CudaPreprocessor &preprocessor, NvencEncoderConfig configuration,
                           NvencOutputCallback output)
    : implementation_(std::make_unique<Implementation>(
          std::move(context), preprocessor, std::move(configuration), std::move(output))) {}

NvencEncoder::~NvencEncoder() = default;
NvencEncoder::NvencEncoder(NvencEncoder &&) noexcept = default;

bool NvencEncoder::available() const { return implementation_->diagnostics().available; }
std::string NvencEncoder::reason() const { return implementation_->diagnostics().reason; }
NvencEncoderOperation NvencEncoder::submit(NvencEncodeInput input) {
  return implementation_->submit(std::move(input));
}
NvencEncoderOperation NvencEncoder::flush() { return implementation_->flush(); }
NvencEncoderOperation NvencEncoder::close() { return implementation_->close(); }
NvencEncoderDiagnostics NvencEncoder::diagnostics() const { return implementation_->diagnostics(); }

} // namespace glyphrelay
