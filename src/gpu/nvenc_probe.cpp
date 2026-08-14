#include "glyphrelay/nvenc_probe.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string_view>
#include <utility>

#if GLYPHRELAY_HAS_NVENC
#include <dlfcn.h>
#include <ffnvcodec/nvEncodeAPI.h>

static_assert(NV_ENC_EMPHASIS_MAP_LEVEL_0 == 0);
static_assert(NV_ENC_EMPHASIS_MAP_LEVEL_1 == 1);
static_assert(NV_ENC_EMPHASIS_MAP_LEVEL_2 == 2);
static_assert(NV_ENC_EMPHASIS_MAP_LEVEL_3 == 3);
static_assert(NV_ENC_EMPHASIS_MAP_LEVEL_4 == 4);
static_assert(NV_ENC_EMPHASIS_MAP_LEVEL_5 == 5);
#endif

namespace glyphrelay {
namespace {

std::string quote(std::string_view value) {
  std::string result = "\"";
  for (const char character : value) {
    if (character == '\\' || character == '"') {
      result.push_back('\\');
    }
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

#if GLYPHRELAY_HAS_NVENC
template <typename Function> Function load_function(void *library, const char *name) {
  void *symbol = dlsym(library, name);
  Function function = nullptr;
  static_assert(sizeof(function) == sizeof(symbol));
  std::memcpy(&function, &symbol, sizeof(function));
  return function;
}

bool same_guid(const GUID &left, const GUID &right) {
  return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

std::string api_version_string(std::uint32_t version) {
  return std::to_string(version & 0xFFU) + "." + std::to_string((version >> 24U) & 0xFFU);
}

class DynamicLibrary {
public:
  DynamicLibrary() : handle_(dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL)) {}
  ~DynamicLibrary() {
    if (handle_ != nullptr) {
      static_cast<void>(dlclose(handle_));
    }
  }

  DynamicLibrary(const DynamicLibrary &) = delete;
  DynamicLibrary &operator=(const DynamicLibrary &) = delete;

  void *get() const { return handle_; }

private:
  void *handle_ = nullptr;
};

class EncoderSession {
public:
  EncoderSession(NV_ENCODE_API_FUNCTION_LIST &functions, void *encoder)
      : functions_(functions), encoder_(encoder) {}
  ~EncoderSession() {
    if (encoder_ != nullptr) {
      static_cast<void>(functions_.nvEncDestroyEncoder(encoder_));
    }
  }

  EncoderSession(const EncoderSession &) = delete;
  EncoderSession &operator=(const EncoderSession &) = delete;

private:
  NV_ENCODE_API_FUNCTION_LIST &functions_;
  void *encoder_ = nullptr;
};
#endif

} // namespace

bool nvenc_api_version_compatible(std::uint32_t maximum_supported, std::uint32_t compiled_version) {
  const auto maximum_major = maximum_supported & 0xFFU;
  const auto maximum_minor = (maximum_supported >> 24U) & 0xFFU;
  const auto compiled_major = compiled_version & 0xFFU;
  const auto compiled_minor = (compiled_version >> 24U) & 0xFFU;
  return maximum_major > compiled_major ||
         (maximum_major == compiled_major && maximum_minor >= compiled_minor);
}

NvencCapabilityReport probe_nvenc_capabilities(CudaPrimaryContext &context) {
  NvencCapabilityReport report;
  report.context = context.identity();
  if (!context.available()) {
    report.reason = "nvenc_cuda_primary_context_unavailable";
    return report;
  }
#if GLYPHRELAY_HAS_NVENC
  ScopedCudaContext guard(context);
  if (!guard.active()) {
    report.reason = guard.reason();
    return report;
  }
  DynamicLibrary library;
  if (library.get() == nullptr) {
    report.reason = "nvenc_driver_library_unavailable";
    return report;
  }
  using GetMaximumVersion = NVENCSTATUS(NVENCAPI *)(std::uint32_t *);
  using CreateInstance = NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
  const auto get_maximum_version =
      load_function<GetMaximumVersion>(library.get(), "NvEncodeAPIGetMaxSupportedVersion");
  const auto create_instance =
      load_function<CreateInstance>(library.get(), "NvEncodeAPICreateInstance");
  if (get_maximum_version == nullptr || create_instance == nullptr) {
    report.reason = "nvenc_driver_entry_point_missing";
    return report;
  }
  std::uint32_t maximum_version = 0U;
  if (get_maximum_version(&maximum_version) != NV_ENC_SUCCESS) {
    report.reason = "nvenc_maximum_api_query_failed";
    return report;
  }
  report.maximum_driver_api_version = api_version_string(maximum_version);
  report.api_compatible = nvenc_api_version_compatible(maximum_version, NVENCAPI_VERSION);
  if (!report.api_compatible) {
    report.reason = "nvenc_compiled_api_too_new";
    return report;
  }
  NV_ENCODE_API_FUNCTION_LIST functions{};
  functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  if (create_instance(&functions) != NV_ENC_SUCCESS ||
      functions.nvEncOpenEncodeSessionEx == nullptr || functions.nvEncDestroyEncoder == nullptr) {
    report.reason = "nvenc_function_table_creation_failed";
    return report;
  }
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
  open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  open.device = reinterpret_cast<void *>(context.native_handle());
  open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  open.apiVersion = NVENCAPI_VERSION;
  void *encoder = nullptr;
  if (functions.nvEncOpenEncodeSessionEx(&open, &encoder) != NV_ENC_SUCCESS || encoder == nullptr) {
    report.reason = "nvenc_session_open_failed";
    return report;
  }
  EncoderSession session(functions, encoder);

  std::uint32_t guid_count = 0U;
  if (functions.nvEncGetEncodeGUIDCount == nullptr || functions.nvEncGetEncodeGUIDs == nullptr ||
      functions.nvEncGetEncodeGUIDCount(encoder, &guid_count) != NV_ENC_SUCCESS ||
      guid_count == 0U || guid_count > 64U) {
    report.reason = "nvenc_encode_guid_query_failed";
    return report;
  }
  std::vector<GUID> guids(guid_count);
  std::uint32_t returned_guids = 0U;
  if (functions.nvEncGetEncodeGUIDs(encoder, guids.data(), guid_count, &returned_guids) !=
          NV_ENC_SUCCESS ||
      returned_guids > guid_count) {
    report.reason = "nvenc_encode_guid_list_failed";
    return report;
  }
  report.h264 = std::any_of(guids.begin(), guids.begin() + returned_guids, [](const GUID &guid) {
    return same_guid(guid, NV_ENC_CODEC_H264_GUID);
  });
  if (!report.h264 || functions.nvEncGetEncodeCaps == nullptr) {
    report.reason = "nvenc_h264_unavailable";
    return report;
  }
  const auto query_cap = [&functions, &encoder](NV_ENC_CAPS capability, int &value) {
    NV_ENC_CAPS_PARAM parameter{};
    parameter.version = NV_ENC_CAPS_PARAM_VER;
    parameter.capsToQuery = capability;
    return functions.nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_H264_GUID, &parameter, &value) ==
           NV_ENC_SUCCESS;
  };
  int emphasis = 0;
  int maximum_width = 0;
  int maximum_height = 0;
  if (!query_cap(NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP, emphasis) ||
      !query_cap(NV_ENC_CAPS_WIDTH_MAX, maximum_width) ||
      !query_cap(NV_ENC_CAPS_HEIGHT_MAX, maximum_height) || maximum_width <= 0 ||
      maximum_height <= 0) {
    report.reason = "nvenc_h264_capability_query_failed";
    return report;
  }
  report.emphasis_map = emphasis == 1;
  report.maximum_width = static_cast<std::size_t>(maximum_width);
  report.maximum_height = static_cast<std::size_t>(maximum_height);

  if (functions.nvEncGetInputFormatCount == nullptr || functions.nvEncGetInputFormats == nullptr) {
    report.reason = "nvenc_input_format_query_unavailable";
    return report;
  }
  std::uint32_t format_count = 0U;
  if (functions.nvEncGetInputFormatCount(encoder, NV_ENC_CODEC_H264_GUID, &format_count) !=
          NV_ENC_SUCCESS ||
      format_count == 0U || format_count > 128U) {
    report.reason = "nvenc_input_format_count_failed";
    return report;
  }
  std::vector<NV_ENC_BUFFER_FORMAT> formats(format_count);
  std::uint32_t returned_formats = 0U;
  if (functions.nvEncGetInputFormats(encoder, NV_ENC_CODEC_H264_GUID, formats.data(), format_count,
                                     &returned_formats) != NV_ENC_SUCCESS ||
      returned_formats > format_count) {
    report.reason = "nvenc_input_format_list_failed";
    return report;
  }
  report.nv12 = std::find(formats.begin(), formats.begin() + returned_formats,
                          NV_ENC_BUFFER_FORMAT_NV12) != formats.begin() + returned_formats;
  report.passed = report.h264 && report.emphasis_map && report.nv12;
  report.reason = report.passed ? "nvenc_h264_emphasis_nv12_supported"
                                : "nvenc_required_capability_unavailable";
#else
  report.reason = "nvenc_probe_not_built";
#endif
  return report;
}

std::string nvenc_capability_report_json(const NvencCapabilityReport &report) {
  std::ostringstream output;
  output << '{' << "\"passed\":" << (report.passed ? "true" : "false") << ','
         << "\"reason\":" << quote(report.reason) << ','
         << "\"compiled_header_version\":" << quote(report.compiled_header_version) << ','
         << "\"maximum_driver_api_version\":" << quote(report.maximum_driver_api_version) << ','
         << "\"api_compatible\":" << (report.api_compatible ? "true" : "false") << ','
         << "\"h264\":" << (report.h264 ? "true" : "false") << ','
         << "\"emphasis_map\":" << (report.emphasis_map ? "true" : "false") << ','
         << "\"nv12\":" << (report.nv12 ? "true" : "false") << ','
         << "\"maximum_width\":" << report.maximum_width << ','
         << "\"maximum_height\":" << report.maximum_height << ','
         << "\"device_ordinal\":" << report.context.device_ordinal << ','
         << "\"context_generation\":" << report.context.generation << '}';
  return output.str();
}

} // namespace glyphrelay
