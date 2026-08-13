#pragma once

#include <string>
#include <vector>

namespace glyphrelay {

struct ProbeResult {
  std::string status = "not_probed";
  std::string reason = "probe_not_run";
};

struct EnvironmentSnapshot {
  std::string operating_system;
  std::string architecture;
  std::string desktop_session;
  ProbeResult xdg_portal;
  std::string portal_backend = "unavailable";
  std::string portal_version = "unavailable";
  std::vector<std::string> portal_source_types;
  std::vector<std::string> portal_cursor_modes;
  ProbeResult pipewire;
  std::string pipewire_version = "unavailable";
  std::string gpu_model = "unavailable";
  std::string gpu_architecture = "unavailable";
  std::string nvidia_driver_version = "unavailable";
  int visible_gpu_count = 0;
  ProbeResult cuda_driver;
  std::string cuda_driver_version = "unavailable";
  ProbeResult cuda_runtime;
  std::string cuda_runtime_version = "unavailable";
  std::string cuda_toolkit_version = "unavailable";
  std::string nvenc_max_api_version = "unavailable";
  ProbeResult nvenc_api_compatibility;
  std::string recording_profile_hash = "not_frozen";
  ProbeResult recording_profile_compatibility;
  ProbeResult h264_nvenc;
  ProbeResult emphasis_map;
  std::vector<std::string> nvenc_input_formats;
  int maximum_width = 0;
  int maximum_height = 0;
  int maximum_sessions = 0;
  ProbeResult dmabuf_import;
  ProbeResult shared_memory_capture;
  ProbeResult cpu_encoder;
  std::string openh264_version = "unavailable";
  ProbeResult screen_lock_hook;
  ProbeResult capture_revocation_hook;
  ProbeResult chromium;
  std::string chromium_version = "unavailable";
  ProbeResult firefox;
  std::string firefox_version = "unavailable";
  ProbeResult signaling_origin;
  ProbeResult turn_configuration;
  ProbeResult wire_cap_accounting;
  std::string selected_ip_family = "none";
};

struct DoctorReport {
  int schema_version = 1;
  std::string application_version;
  std::string operating_system;
  std::string architecture;
  std::string desktop_session;
  ProbeResult xdg_portal;
  std::string portal_backend;
  std::string portal_version;
  std::vector<std::string> portal_source_types;
  std::vector<std::string> portal_cursor_modes;
  ProbeResult pipewire;
  std::string pipewire_version;
  std::string gpu_model;
  std::string gpu_architecture;
  std::string nvidia_driver_version;
  int visible_gpu_count = 0;
  ProbeResult cuda_driver;
  std::string cuda_driver_version;
  ProbeResult cuda_runtime;
  std::string cuda_runtime_version;
  std::string cuda_toolkit_version;
  std::string nvenc_max_api_version;
  ProbeResult nvenc_api_compatibility;
  std::string pinned_nvenc_header_version;
  std::string pinned_nvenc_header_sha256;
  std::string pinned_nvenc_header_license;
  std::string pinned_nvenc_minimum_driver_version;
  std::string recording_profile_hash;
  ProbeResult recording_profile_compatibility;
  ProbeResult h264_nvenc;
  ProbeResult emphasis_map;
  std::vector<std::string> nvenc_input_formats;
  int maximum_width = 0;
  int maximum_height = 0;
  int maximum_sessions = 0;
  ProbeResult dmabuf_import;
  ProbeResult shared_memory_capture;
  ProbeResult cpu_encoder;
  std::string openh264_version;
  ProbeResult screen_lock_hook;
  ProbeResult capture_revocation_hook;
  ProbeResult chromium;
  std::string chromium_version;
  ProbeResult firefox;
  std::string firefox_version;
  ProbeResult signaling_origin;
  ProbeResult turn_configuration;
  ProbeResult wire_cap_accounting;
  std::string selected_ip_family;
  std::string mode;
  std::vector<std::string> reasons;
};

EnvironmentSnapshot collect_environment_snapshot();
DoctorReport build_doctor_report(const EnvironmentSnapshot &snapshot);
std::string doctor_report_json(const DoctorReport &report);
std::string doctor_report_text(const DoctorReport &report);

} // namespace glyphrelay
