#include "glyphrelay/doctor.hpp"
#include "glyphrelay/dependency_versions.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>

namespace glyphrelay {
namespace {

constexpr std::string_view kAvailable = "available";
constexpr std::array<std::string_view, 6> kProbeStatuses = {
    "available", "unavailable", "unsupported", "not_probed", "incompatible", "error"};

bool valid_status(std::string_view status) {
  return std::find(kProbeStatuses.begin(), kProbeStatuses.end(), status) != kProbeStatuses.end();
}

bool safe_reason(std::string_view reason) {
  return !reason.empty() && std::all_of(reason.begin(), reason.end(), [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
           character == '_';
  });
}

bool contains_sensitive_value(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  if (value.front() == '/' || value.front() == '~' || value.find("://") != std::string_view::npos ||
      value.starts_with("turn:") || value.starts_with("turns:") ||
      value.find("/Users/") != std::string_view::npos ||
      value.find("/home/") != std::string_view::npos ||
      value.find("\\Users\\") != std::string_view::npos) {
    return true;
  }
  return value.size() >= 3U && value[1] == ':' && (value[2] == '\\' || value[2] == '/');
}

std::string public_value(std::string_view value) {
  if (contains_sensitive_value(value)) {
    return "redacted";
  }
  constexpr std::size_t maximum_length = 160U;
  return std::string(value.substr(0U, maximum_length));
}

std::string json_escape(std::string_view value) {
  std::ostringstream output;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
    case '"':
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

std::string json_quote(std::string_view value) {
  return "\"" + json_escape(public_value(value)) + "\"";
}

std::string result_json(const ProbeResult &result) {
  const std::string status = valid_status(result.status) ? result.status : "error";
  const std::string reason = safe_reason(result.reason) ? result.reason : "invalid_probe_reason";
  return "{\"status\":" + json_quote(status) + ",\"reason\":" + json_quote(reason) + "}";
}

std::string string_array_json(const std::vector<std::string> &values) {
  std::ostringstream output;
  output << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    output << json_quote(values[index]);
  }
  output << ']';
  return output.str();
}

bool available(const ProbeResult &result) { return result.status == kAvailable; }

bool pending(const ProbeResult &result) {
  return result.status == "not_probed" || result.status == "error";
}

void add_reason(std::vector<std::string> &reasons, std::string reason) {
  if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
    reasons.push_back(std::move(reason));
  }
}

void decide_mode(DoctorReport &report) {
  const bool supported_platform =
      report.operating_system == "linux" &&
      (report.architecture == "x86_64" || report.architecture == "amd64");
  if (!supported_platform) {
    report.mode = "unsupported_sender";
    add_reason(report.reasons, "linux_x86_64_sender_required");
    return;
  }

  const std::array required_runtime = {
      &report.xdg_portal,
      &report.pipewire,
      &report.shared_memory_capture,
      &report.screen_lock_hook,
      &report.capture_revocation_hook,
  };
  bool runtime_pending = false;
  bool runtime_unavailable = false;
  for (const ProbeResult *probe : required_runtime) {
    if (!available(*probe)) {
      add_reason(report.reasons, probe->reason);
      runtime_pending = runtime_pending || pending(*probe);
      runtime_unavailable = runtime_unavailable || !pending(*probe);
    }
  }
  if (runtime_pending || runtime_unavailable) {
    report.mode = runtime_unavailable ? "unsupported" : "diagnostic_only";
    return;
  }

  if (available(report.h264_nvenc) && available(report.nvenc_api_compatibility)) {
    if (!available(report.recording_profile_compatibility)) {
      report.mode = "diagnostic_only";
      add_reason(report.reasons, report.recording_profile_compatibility.reason);
      return;
    }
    if (available(report.emphasis_map)) {
      report.mode = "enhanced_nvenc";
      add_reason(report.reasons, "enhanced_nvenc_requirements_satisfied");
    } else {
      report.mode = "uniform_nvenc";
      add_reason(report.reasons, report.emphasis_map.reason);
      add_reason(report.reasons, "uniform_nvenc_selected");
    }
    return;
  }

  if (available(report.cpu_encoder) && available(report.recording_profile_compatibility)) {
    report.mode = "cpu_fallback";
    add_reason(report.reasons, available(report.nvenc_api_compatibility)
                                   ? report.h264_nvenc.reason
                                   : report.nvenc_api_compatibility.reason);
    add_reason(report.reasons, "cpu_fallback_selected");
    return;
  }

  if (pending(report.h264_nvenc) || pending(report.nvenc_api_compatibility) ||
      pending(report.recording_profile_compatibility)) {
    report.mode = "diagnostic_only";
    add_reason(report.reasons, "encoder_capabilities_not_fully_verified");
    return;
  }

  report.mode = "unsupported";
  add_reason(report.reasons, "no_supported_encoder_available");
}

} // namespace

DoctorReport build_doctor_report(const EnvironmentSnapshot &snapshot) {
  DoctorReport report;
  report.application_version = GLYPHRELAY_VERSION;
  report.operating_system = snapshot.operating_system;
  report.architecture = snapshot.architecture;
  report.desktop_session = snapshot.desktop_session;
  report.xdg_portal = snapshot.xdg_portal;
  report.portal_backend = snapshot.portal_backend;
  report.portal_version = snapshot.portal_version;
  report.portal_source_types = snapshot.portal_source_types;
  report.portal_cursor_modes = snapshot.portal_cursor_modes;
  report.pipewire = snapshot.pipewire;
  report.pipewire_version = snapshot.pipewire_version;
  report.gpu_model = snapshot.gpu_model;
  report.gpu_architecture = snapshot.gpu_architecture;
  report.nvidia_driver_version = snapshot.nvidia_driver_version;
  report.visible_gpu_count = snapshot.visible_gpu_count;
  report.cuda_driver = snapshot.cuda_driver;
  report.cuda_driver_version = snapshot.cuda_driver_version;
  report.cuda_runtime = snapshot.cuda_runtime;
  report.cuda_runtime_version = snapshot.cuda_runtime_version;
  report.cuda_toolkit_version = snapshot.cuda_toolkit_version;
  report.nvenc_max_api_version = snapshot.nvenc_max_api_version;
  report.nvenc_api_compatibility = snapshot.nvenc_api_compatibility;
  report.pinned_nvenc_header_version = dependency_versions::nvenc_header_version;
  report.pinned_nvenc_header_sha256 = dependency_versions::nvenc_header_sha256;
  report.pinned_nvenc_header_license = dependency_versions::nvenc_header_license;
  report.pinned_nvenc_minimum_driver_version = dependency_versions::nvenc_minimum_linux_driver;
  report.recording_profile_hash = snapshot.recording_profile_hash;
  report.recording_profile_compatibility = snapshot.recording_profile_compatibility;
  report.h264_nvenc = snapshot.h264_nvenc;
  report.emphasis_map = snapshot.emphasis_map;
  report.nvenc_input_formats = snapshot.nvenc_input_formats;
  report.maximum_width = snapshot.maximum_width;
  report.maximum_height = snapshot.maximum_height;
  report.maximum_sessions = snapshot.maximum_sessions;
  report.dmabuf_import = snapshot.dmabuf_import;
  report.shared_memory_capture = snapshot.shared_memory_capture;
  report.cpu_encoder = snapshot.cpu_encoder;
  report.openh264_version = snapshot.openh264_version;
  report.screen_lock_hook = snapshot.screen_lock_hook;
  report.capture_revocation_hook = snapshot.capture_revocation_hook;
  report.chromium = snapshot.chromium;
  report.chromium_version = snapshot.chromium_version;
  report.firefox = snapshot.firefox;
  report.firefox_version = snapshot.firefox_version;
  report.signaling_origin = snapshot.signaling_origin;
  report.turn_configuration = snapshot.turn_configuration;
  report.wire_cap_accounting = snapshot.wire_cap_accounting;
  report.selected_ip_family = snapshot.selected_ip_family;
  decide_mode(report);
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
         << "\"portal_version\":" << json_quote(report.portal_version) << ','
         << "\"source_types\":" << string_array_json(report.portal_source_types) << ','
         << "\"cursor_modes\":" << string_array_json(report.portal_cursor_modes) << ','
         << "\"pipewire\":" << result_json(report.pipewire) << ','
         << "\"pipewire_version\":" << json_quote(report.pipewire_version) << ','
         << "\"dmabuf_import\":" << result_json(report.dmabuf_import) << ','
         << "\"shared_memory\":" << result_json(report.shared_memory_capture) << "},"
         << "\"gpu\":{"
         << "\"model\":" << json_quote(report.gpu_model) << ','
         << "\"architecture\":" << json_quote(report.gpu_architecture) << ','
         << "\"visible_count\":" << report.visible_gpu_count << ','
         << "\"nvidia_driver_version\":" << json_quote(report.nvidia_driver_version) << ','
         << "\"cuda_driver\":" << result_json(report.cuda_driver) << ','
         << "\"cuda_driver_version\":" << json_quote(report.cuda_driver_version) << ','
         << "\"cuda_runtime\":" << result_json(report.cuda_runtime) << ','
         << "\"cuda_runtime_version\":" << json_quote(report.cuda_runtime_version) << ','
         << "\"cuda_toolkit_version\":" << json_quote(report.cuda_toolkit_version) << "},"
         << "\"nvenc\":{"
         << "\"maximum_api_version\":" << json_quote(report.nvenc_max_api_version) << ','
         << "\"api_compatibility\":" << result_json(report.nvenc_api_compatibility) << ','
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
         << "\"cpu_encoder\":" << result_json(report.cpu_encoder) << ','
         << "\"openh264_version\":" << json_quote(report.openh264_version) << "},"
         << "\"privacy\":{"
         << "\"screen_lock_hook\":" << result_json(report.screen_lock_hook) << ','
         << "\"capture_revocation_hook\":" << result_json(report.capture_revocation_hook) << "},"
         << "\"browser\":{"
         << "\"chromium\":" << result_json(report.chromium) << ','
         << "\"chromium_version\":" << json_quote(report.chromium_version) << ','
         << "\"firefox\":" << result_json(report.firefox) << ','
         << "\"firefox_version\":" << json_quote(report.firefox_version) << "},"
         << "\"network\":{"
         << "\"signaling_origin\":" << result_json(report.signaling_origin) << ','
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
         << "Environment: " << public_value(report.operating_system) << " / "
         << public_value(report.architecture) << '\n'
         << "Desktop session: " << public_value(report.desktop_session) << '\n'
         << "XDG portal: " << report.xdg_portal.status << " (" << report.xdg_portal.reason << ")"
         << ", API " << public_value(report.portal_version) << '\n'
         << "PipeWire: " << report.pipewire.status << " (" << report.pipewire.reason << ")"
         << ", version " << public_value(report.pipewire_version) << '\n'
         << "GPU: " << public_value(report.gpu_model) << " / "
         << public_value(report.gpu_architecture) << '\n'
         << "NVIDIA driver: " << public_value(report.nvidia_driver_version) << '\n'
         << "CUDA driver: " << report.cuda_driver.status << " (" << report.cuda_driver.reason
         << "), version " << public_value(report.cuda_driver_version) << '\n'
         << "CUDA runtime: " << report.cuda_runtime.status << " (" << report.cuda_runtime.reason
         << "), version " << public_value(report.cuda_runtime_version) << '\n'
         << "NVENC API: " << report.nvenc_api_compatibility.status << " ("
         << report.nvenc_api_compatibility.reason << "), maximum "
         << public_value(report.nvenc_max_api_version) << '\n'
         << "H.264 NVENC: " << report.h264_nvenc.status << " (" << report.h264_nvenc.reason << ")\n"
         << "Emphasis map: " << report.emphasis_map.status << " (" << report.emphasis_map.reason
         << ")\n"
         << "CPU encoder: " << report.cpu_encoder.status << " (" << report.cpu_encoder.reason
         << "), OpenH264 " << public_value(report.openh264_version) << '\n'
         << "Chromium: " << report.chromium.status << " (" << report.chromium.reason
         << "), version " << public_value(report.chromium_version) << '\n'
         << "Firefox: " << report.firefox.status << " (" << report.firefox.reason << "), version "
         << public_value(report.firefox_version) << '\n'
         << "TURN: " << report.turn_configuration.status << " (" << report.turn_configuration.reason
         << ")\n"
         << "Wire cap accounting: " << report.wire_cap_accounting.status << " ("
         << report.wire_cap_accounting.reason << ")\n"
         << "Mode: " << report.mode << '\n';
  for (const auto &reason : report.reasons) {
    output << "Reason: " << public_value(reason) << '\n';
  }
  return output.str();
}

} // namespace glyphrelay
