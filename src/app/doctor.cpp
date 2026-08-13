#include "glyphrelay/doctor.hpp"
#include "glyphrelay/dependency_versions.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <utility>

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace glyphrelay {
namespace {

constexpr std::string_view kUnavailable = "unavailable";
constexpr std::string_view kUnsupported = "unsupported";
constexpr std::string_view kNotProbed = "not_probed";

std::string json_escape(std::string_view value) {
  std::ostringstream output;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
    case '\"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
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
        constexpr char digits[] = "0123456789abcdef";
        output << "\\u00" << digits[(character >> 4U) & 0x0FU] << digits[character & 0x0FU];
      } else {
        output << static_cast<char>(character);
      }
    }
  }
  return output.str();
}

std::string json_quote(std::string_view value) { return "\"" + json_escape(value) + "\""; }

std::string result_json(const ProbeResult &result) {
  return "{\"status\":" + json_quote(result.status) + ",\"reason\":" + json_quote(result.reason) +
         "}";
}

std::string string_array_json(const std::vector<std::string> &values) {
  std::ostringstream output;
  output << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    output << json_quote(values[index]);
  }
  output << ']';
  return output.str();
}

std::string environment_value(const EnvironmentSnapshot &snapshot, std::string_view key) {
  const auto iterator = snapshot.environment.find(std::string(key));
  return iterator == snapshot.environment.end() ? std::string{} : iterator->second;
}

bool has_library(const std::vector<const char *> &names) {
#if defined(__APPLE__) || defined(__linux__)
  for (const char *name : names) {
    if (void *handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL); handle != nullptr) {
      dlclose(handle);
      return true;
    }
  }
#else
  static_cast<void>(names);
#endif
  return false;
}

bool command_available(std::string_view command) {
#if defined(__APPLE__) || defined(__linux__)
  const char *path_value = std::getenv("PATH");
  if (path_value == nullptr) {
    return false;
  }
  std::stringstream paths(path_value);
  std::string path;
  while (std::getline(paths, path, ':')) {
    if (path.empty()) {
      continue;
    }
    const auto candidate = std::filesystem::path(path) / command;
    if (access(candidate.c_str(), X_OK) == 0) {
      return true;
    }
  }
#else
  static_cast<void>(command);
#endif
  return false;
}

std::string normalized_session(std::string session) {
  if (session == "wayland" || session == "x11") {
    return session;
  }
  return session.empty() ? "unavailable" : "other";
}

ProbeResult available_if(bool value, std::string available_reason, std::string missing_reason) {
  return {value ? "available" : std::string(kUnavailable),
          value ? std::move(available_reason) : std::move(missing_reason)};
}

} // namespace

EnvironmentSnapshot collect_environment_snapshot() {
  EnvironmentSnapshot snapshot;
#if defined(__APPLE__)
  snapshot.operating_system = "macos";
#elif defined(__linux__)
  snapshot.operating_system = "linux";
#elif defined(_WIN32)
  snapshot.operating_system = "windows";
#else
  snapshot.operating_system = "other";
#endif

#if defined(__APPLE__) || defined(__linux__)
  utsname identity{};
  if (uname(&identity) == 0) {
    snapshot.architecture = identity.machine;
  }
#endif
  if (snapshot.architecture.empty()) {
    snapshot.architecture = "unknown";
  }

  constexpr std::array environment_keys = {"XDG_SESSION_TYPE", "DBUS_SESSION_BUS_ADDRESS",
                                           "GLYPHRELAY_SIGNALING_ORIGIN", "GLYPHRELAY_TURN_URL"};
  for (const char *key : environment_keys) {
    if (const char *value = std::getenv(key); value != nullptr) {
      snapshot.environment.emplace(key, value);
    }
  }

  snapshot.cuda_driver_library_available =
      has_library({"libcuda.so.1", "libcuda.so", "libcuda.dylib"});
  snapshot.nvenc_driver_library_available =
      has_library({"libnvidia-encode.so.1", "libnvidia-encode.so"});
  snapshot.openh264_library_available =
      has_library({"libopenh264.so.7", "libopenh264.so.6", "libopenh264.so", "libopenh264.dylib"});
  snapshot.chromium_available = command_available("chromium") ||
                                command_available("chromium-browser") ||
                                command_available("google-chrome");
#if defined(__APPLE__)
  snapshot.chromium_available =
      snapshot.chromium_available ||
      std::filesystem::exists("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
#endif
  snapshot.firefox_available = command_available("firefox");
  return snapshot;
}

DoctorReport build_doctor_report(const EnvironmentSnapshot &snapshot) {
  DoctorReport report;
  report.application_version = GLYPHRELAY_VERSION;
  report.operating_system = snapshot.operating_system;
  report.architecture = snapshot.architecture;
  report.desktop_session = normalized_session(environment_value(snapshot, "XDG_SESSION_TYPE"));

  const bool linux = snapshot.operating_system == "linux";
  const bool session_bus = !environment_value(snapshot, "DBUS_SESSION_BUS_ADDRESS").empty();
  report.xdg_portal = linux ? ProbeResult{std::string(kNotProbed),
                                          session_bus ? "portal_runtime_probe_not_implemented"
                                                      : "desktop_session_bus_unavailable"}
                            : ProbeResult{std::string(kUnsupported), "linux_sender_only"};
  report.portal_backend = "unknown";
  report.pipewire =
      linux ? ProbeResult{std::string(kNotProbed), "pipewire_runtime_probe_not_implemented"}
            : ProbeResult{std::string(kUnsupported), "linux_sender_only"};

  report.gpu_model = "unknown";
  report.gpu_architecture = "unknown";
  report.nvidia_driver_version = "unknown";
  report.cuda_driver =
      available_if(snapshot.cuda_driver_library_available, "cuda_driver_library_loadable",
                   "cuda_driver_library_unavailable");
  report.cuda_runtime = {std::string(kNotProbed), "cuda_runtime_probe_not_implemented"};
  report.cuda_toolkit_version = GLYPHRELAY_HAS_CUDA_COMPILER ? "compiler_available" : "unavailable";
  report.nvenc_max_api_version = "not_probed";
  report.pinned_nvenc_header_version = dependency_versions::nvenc_header_version;
  report.pinned_nvenc_header_sha256 = dependency_versions::nvenc_header_sha256;
  report.pinned_nvenc_header_license = dependency_versions::nvenc_header_license;
  report.pinned_nvenc_minimum_driver_version = dependency_versions::nvenc_minimum_linux_driver;
  report.recording_profile_hash = "not_frozen";
  report.recording_profile_compatibility = {std::string(kNotProbed), "browser_offers_required"};
  report.h264_nvenc =
      snapshot.nvenc_driver_library_available
          ? ProbeResult{std::string(kNotProbed), "nvenc_capability_query_required"}
          : ProbeResult{std::string(kUnavailable), "nvenc_driver_library_unavailable"};
  report.emphasis_map =
      snapshot.nvenc_driver_library_available
          ? ProbeResult{std::string(kNotProbed), "nvenc_capability_query_required"}
          : ProbeResult{std::string(kUnavailable), "nvenc_driver_library_unavailable"};
  report.dmabuf_import = linux ? ProbeResult{std::string(kNotProbed), "capture_format_required"}
                               : ProbeResult{std::string(kUnsupported), "linux_sender_only"};
  report.shared_memory_capture =
      linux ? ProbeResult{std::string(kNotProbed), "portal_capture_required"}
            : ProbeResult{std::string(kUnsupported), "linux_sender_only"};
  report.cpu_encoder =
      available_if(snapshot.openh264_library_available, "system_openh264_library_loadable",
                   "system_openh264_library_unavailable");
  report.screen_lock_hook =
      linux ? ProbeResult{std::string(kNotProbed), "desktop_hook_probe_required"}
            : ProbeResult{std::string(kUnsupported), "linux_sender_only"};
  report.capture_revocation_hook =
      linux ? ProbeResult{std::string(kNotProbed), "portal_session_probe_required"}
            : ProbeResult{std::string(kUnsupported), "linux_sender_only"};
  report.chromium = available_if(snapshot.chromium_available, "browser_binary_available",
                                 "browser_binary_unavailable");
  report.firefox = available_if(snapshot.firefox_available, "browser_binary_available",
                                "browser_binary_unavailable");
  report.turn_configuration =
      environment_value(snapshot, "GLYPHRELAY_TURN_URL").empty()
          ? ProbeResult{std::string(kUnavailable), "turn_not_configured"}
          : ProbeResult{std::string(kNotProbed), "turn_configuration_requires_validation"};
  report.wire_cap_accounting = {std::string(kUnavailable), "transport_hook_not_initialized"};
  report.selected_ip_family = "none";

  if (!linux) {
    report.mode = "unsupported_sender";
    report.reasons.emplace_back("linux_x86_64_sender_required");
  } else if (!snapshot.openh264_library_available && !snapshot.nvenc_driver_library_available) {
    report.mode = "unsupported";
    report.reasons.emplace_back("no_supported_encoder_available");
  } else {
    report.mode = "diagnostic_only";
    report.reasons.emplace_back("runtime_capability_probes_incomplete");
  }
  if (report.emphasis_map.status != "available") {
    report.reasons.emplace_back("enhanced_mode_not_verified");
  }
  return report;
}

std::string doctor_report_json(const DoctorReport &report) {
  std::ostringstream output;
  output << '{' << "\"schema_version\":" << report.schema_version << ','
         << "\"application_version\":" << json_quote(report.application_version) << ','
         << "\"environment\":{"
         << "\"operating_system\":" << json_quote(report.operating_system) << ','
         << "\"architecture\":" << json_quote(report.architecture) << ','
         << "\"desktop_session\":" << json_quote(report.desktop_session) << "},"
         << "\"capture\":{"
         << "\"xdg_portal\":" << result_json(report.xdg_portal) << ','
         << "\"portal_backend\":" << json_quote(report.portal_backend) << ','
         << "\"source_types\":" << string_array_json(report.portal_source_types) << ','
         << "\"cursor_modes\":" << string_array_json(report.portal_cursor_modes) << ','
         << "\"pipewire\":" << result_json(report.pipewire) << ','
         << "\"dmabuf_import\":" << result_json(report.dmabuf_import) << ','
         << "\"shared_memory\":" << result_json(report.shared_memory_capture) << "},"
         << "\"gpu\":{"
         << "\"model\":" << json_quote(report.gpu_model) << ','
         << "\"architecture\":" << json_quote(report.gpu_architecture) << ','
         << "\"nvidia_driver_version\":" << json_quote(report.nvidia_driver_version) << ','
         << "\"cuda_driver\":" << result_json(report.cuda_driver) << ','
         << "\"cuda_runtime\":" << result_json(report.cuda_runtime) << ','
         << "\"cuda_toolkit_version\":" << json_quote(report.cuda_toolkit_version) << "},"
         << "\"nvenc\":{"
         << "\"maximum_api_version\":" << json_quote(report.nvenc_max_api_version) << ','
         << "\"pinned_header_version\":" << json_quote(report.pinned_nvenc_header_version) << ','
         << "\"pinned_header_sha256\":" << json_quote(report.pinned_nvenc_header_sha256) << ','
         << "\"pinned_header_license\":" << json_quote(report.pinned_nvenc_header_license) << ','
         << "\"minimum_driver_version\":" << json_quote(report.pinned_nvenc_minimum_driver_version)
         << ',' << "\"recording_profile_hash\":" << json_quote(report.recording_profile_hash) << ','
         << "\"recording_profile_compatibility\":"
         << result_json(report.recording_profile_compatibility) << ','
         << "\"h264\":" << result_json(report.h264_nvenc) << ','
         << "\"emphasis_map\":" << result_json(report.emphasis_map) << ','
         << "\"input_formats\":" << string_array_json(report.nvenc_input_formats) << ','
         << "\"maximum_width\":" << report.maximum_width << ','
         << "\"maximum_height\":" << report.maximum_height << ','
         << "\"maximum_sessions\":" << report.maximum_sessions << "},"
         << "\"fallbacks\":{"
         << "\"cpu_encoder\":" << result_json(report.cpu_encoder) << "},"
         << "\"privacy\":{"
         << "\"screen_lock_hook\":" << result_json(report.screen_lock_hook) << ','
         << "\"capture_revocation_hook\":" << result_json(report.capture_revocation_hook) << "},"
         << "\"browser\":{"
         << "\"chromium\":" << result_json(report.chromium) << ','
         << "\"firefox\":" << result_json(report.firefox) << "},"
         << "\"network\":{"
         << "\"turn\":" << result_json(report.turn_configuration) << ','
         << "\"wire_cap_accounting\":" << result_json(report.wire_cap_accounting) << ','
         << "\"selected_ip_family\":" << json_quote(report.selected_ip_family) << "},"
         << "\"decision\":{"
         << "\"mode\":" << json_quote(report.mode) << ','
         << "\"reasons\":" << string_array_json(report.reasons) << "}}\n";
  return output.str();
}

std::string doctor_report_text(const DoctorReport &report) {
  std::ostringstream output;
  output << "GlyphRelay doctor schema v" << report.schema_version << '\n'
         << "Environment: " << report.operating_system << " / " << report.architecture << '\n'
         << "Desktop session: " << report.desktop_session << '\n'
         << "XDG portal: " << report.xdg_portal.status << " (" << report.xdg_portal.reason << ")\n"
         << "PipeWire: " << report.pipewire.status << " (" << report.pipewire.reason << ")\n"
         << "CUDA driver: " << report.cuda_driver.status << " (" << report.cuda_driver.reason
         << ")\n"
         << "H.264 NVENC: " << report.h264_nvenc.status << " (" << report.h264_nvenc.reason << ")\n"
         << "Emphasis map: " << report.emphasis_map.status << " (" << report.emphasis_map.reason
         << ")\n"
         << "CPU encoder: " << report.cpu_encoder.status << " (" << report.cpu_encoder.reason
         << ")\n"
         << "Mode: " << report.mode << '\n';
  for (const auto &reason : report.reasons) {
    output << "Reason: " << reason << '\n';
  }
  return output.str();
}

} // namespace glyphrelay
