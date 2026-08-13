#pragma once

#include <map>
#include <string>
#include <vector>

namespace glyphrelay {

struct ProbeResult {
  std::string status;
  std::string reason;
};

struct EnvironmentSnapshot {
  std::string operating_system;
  std::string architecture;
  std::map<std::string, std::string> environment;
  bool cuda_driver_library_available = false;
  bool nvenc_driver_library_available = false;
  bool openh264_library_available = false;
  bool chromium_available = false;
  bool firefox_available = false;
};

struct DoctorReport {
  int schema_version = 1;
  std::string application_version;
  std::string operating_system;
  std::string architecture;
  std::string desktop_session;
  ProbeResult xdg_portal;
  std::string portal_backend;
  std::vector<std::string> portal_source_types;
  std::vector<std::string> portal_cursor_modes;
  ProbeResult pipewire;
  std::string gpu_model;
  std::string gpu_architecture;
  std::string nvidia_driver_version;
  ProbeResult cuda_driver;
  ProbeResult cuda_runtime;
  std::string cuda_toolkit_version;
  std::string nvenc_max_api_version;
  std::string pinned_nvenc_header_version;
  std::string pinned_nvenc_header_sha256;
  std::string pinned_nvenc_header_license;
  std::string pinned_nvenc_minimum_driver_api;
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
  ProbeResult screen_lock_hook;
  ProbeResult capture_revocation_hook;
  ProbeResult chromium;
  ProbeResult firefox;
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
