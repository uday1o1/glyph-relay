#include "glyphrelay/doctor.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

glyphrelay::EnvironmentSnapshot qualified_linux_snapshot() {
  glyphrelay::EnvironmentSnapshot snapshot;
  snapshot.operating_system = "linux";
  snapshot.architecture = "x86_64";
  snapshot.desktop_session = "wayland";
  snapshot.xdg_portal = {"available", "window_screencast_portal_available"};
  snapshot.portal_backend = "org.freedesktop.impl.portal.desktop.gnome";
  snapshot.portal_version = "5";
  snapshot.portal_source_types = {"monitor", "window"};
  snapshot.portal_cursor_modes = {"hidden", "embedded", "metadata"};
  snapshot.pipewire = {"available", "pipewire_library_loadable"};
  snapshot.pipewire_version = "1.0.5";
  snapshot.gpu_model = "NVIDIA test GPU";
  snapshot.gpu_architecture = "compute_8.9";
  snapshot.nvidia_driver_version = "610.43.02";
  snapshot.visible_gpu_count = 1;
  snapshot.cuda_driver = {"available", "cuda_driver_initialized"};
  snapshot.cuda_driver_version = "13.3";
  snapshot.cuda_runtime = {"available", "cuda_runtime_version_queried"};
  snapshot.cuda_runtime_version = "13.3";
  snapshot.cuda_toolkit_version = "13.3";
  snapshot.nvenc_max_api_version = "13.1";
  snapshot.nvenc_api_compatibility = {"available", "compiled_nvenc_api_supported"};
  snapshot.recording_profile_hash =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  snapshot.recording_profile_compatibility = {"available", "recording_profile_compatible"};
  snapshot.h264_nvenc = {"available", "h264_nvenc_supported"};
  snapshot.emphasis_map = {"available", "emphasis_map_supported"};
  snapshot.nvenc_input_formats = {"nv12"};
  snapshot.maximum_width = 8192;
  snapshot.maximum_height = 8192;
  snapshot.maximum_sessions = 8;
  snapshot.dmabuf_import = {"not_probed", "selected_capture_format_required"};
  snapshot.shared_memory_capture = {"available", "shared_memory_capture_verified"};
  snapshot.cpu_encoder = {"available", "system_openh264_library_loadable"};
  snapshot.openh264_version = "2.4.1";
  snapshot.screen_lock_hook = {"available", "screen_lock_hook_verified"};
  snapshot.capture_revocation_hook = {"available", "capture_revocation_hook_verified"};
  snapshot.chromium = {"available", "browser_version_queried"};
  snapshot.chromium_version = "151.0.7922.34";
  snapshot.firefox = {"available", "browser_version_queried"};
  snapshot.firefox_version = "153.0";
  snapshot.signaling_origin = {"available", "loopback_signaling_origin_configured"};
  snapshot.turn_configuration = {"not_probed", "turn_udp_connectivity_test_required"};
  snapshot.wire_cap_accounting = {"available", "final_datagram_hook_verified"};
  snapshot.selected_ip_family = "ipv4";
  return snapshot;
}

} // namespace

int main() {
  const auto enhanced = glyphrelay::build_doctor_report(qualified_linux_snapshot());
  require(enhanced.schema_version == 1, "doctor schema version must be one");
  require(enhanced.mode == "enhanced_nvenc", "fully qualified fixture must select enhanced mode");

  const auto json = glyphrelay::doctor_report_json(enhanced);
  require(json.find("\"schema_version\":1") != std::string::npos,
          "JSON must include its schema version");
  require(json.find("8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228") !=
              std::string::npos,
          "JSON must report the pinned header hash");
  require(json.find("\"minimum_driver_version\":\"610.0\"") != std::string::npos,
          "JSON must report the pinned minimum Linux driver");
  require(json.find("\"api_compatibility\":{\"status\":\"available\"") != std::string::npos,
          "JSON must report compiled API compatibility");

  auto uniform_snapshot = qualified_linux_snapshot();
  uniform_snapshot.emphasis_map = {"unavailable", "emphasis_map_unsupported"};
  const auto uniform = glyphrelay::build_doctor_report(uniform_snapshot);
  require(uniform.mode == "uniform_nvenc", "missing emphasis support must select uniform NVENC");

  auto cpu_snapshot = qualified_linux_snapshot();
  cpu_snapshot.h264_nvenc = {"unavailable", "h264_nvenc_unsupported"};
  cpu_snapshot.emphasis_map = {"unavailable", "emphasis_map_unsupported"};
  const auto cpu = glyphrelay::build_doctor_report(cpu_snapshot);
  require(cpu.mode == "cpu_fallback", "system OpenH264 must provide the CPU fallback");

  auto incompatible_snapshot = qualified_linux_snapshot();
  incompatible_snapshot.nvenc_api_compatibility = {"incompatible", "compiled_nvenc_api_too_new"};
  const auto incompatible = glyphrelay::build_doctor_report(incompatible_snapshot);
  require(incompatible.mode == "cpu_fallback",
          "an incompatible NVENC API must fail closed to the CPU path");

  auto pending_snapshot = qualified_linux_snapshot();
  pending_snapshot.recording_profile_compatibility = {"not_probed", "browser_offers_required"};
  const auto pending = glyphrelay::build_doctor_report(pending_snapshot);
  require(pending.mode == "diagnostic_only",
          "an unfrozen recording profile must prevent an encoder claim");

  auto unavailable_snapshot = qualified_linux_snapshot();
  unavailable_snapshot.xdg_portal = {"unavailable", "screencast_portal_unavailable"};
  const auto unavailable = glyphrelay::build_doctor_report(unavailable_snapshot);
  require(unavailable.mode == "unsupported", "missing required capture must be unsupported");

  auto unsupported_snapshot = qualified_linux_snapshot();
  unsupported_snapshot.operating_system = "macos";
  unsupported_snapshot.architecture = "arm64";
  const auto unsupported = glyphrelay::build_doctor_report(unsupported_snapshot);
  require(unsupported.mode == "unsupported_sender",
          "non-Linux environments must fail with an actionable mode");

  auto sensitive_snapshot = qualified_linux_snapshot();
  sensitive_snapshot.gpu_model = "/Users/private-user/secret-window-title";
  sensitive_snapshot.portal_backend = "https://user:password@example.invalid";
  sensitive_snapshot.h264_nvenc.reason = "secret-host.invalid";
  const auto sensitive =
      glyphrelay::doctor_report_json(glyphrelay::build_doctor_report(sensitive_snapshot));
  require(sensitive.find("private-user") == std::string::npos, "full user paths must be redacted");
  require(sensitive.find("password") == std::string::npos, "credentials must be redacted");
  require(sensitive.find("secret-host") == std::string::npos,
          "invalid dynamic reasons must not enter output");
  require(sensitive.find("invalid_probe_reason") != std::string::npos,
          "invalid reasons must be replaced with a stable marker");

#if defined(__APPLE__) || defined(__linux__)
  require(setenv("GLYPHRELAY_SIGNALING_ORIGIN", "http://localhost.evil.invalid", 1) == 0,
          "test must configure a hostile signaling origin");
  require(setenv("GLYPHRELAY_TURN_URL", "turn:relay.invalid?transport=udpbad", 1) == 0,
          "test must configure a hostile TURN transport");
  const auto hostile_configuration = glyphrelay::collect_environment_snapshot();
  require(hostile_configuration.signaling_origin.status == "incompatible",
          "a hostname with a loopback prefix must not be accepted as loopback");
  require(hostile_configuration.turn_configuration.status == "incompatible",
          "a TURN transport with a UDP prefix must not be accepted as UDP");
  require(unsetenv("GLYPHRELAY_SIGNALING_ORIGIN") == 0,
          "test must clear the hostile signaling origin");
  require(unsetenv("GLYPHRELAY_TURN_URL") == 0, "test must clear the hostile TURN URL");
#endif

  return 0;
}
