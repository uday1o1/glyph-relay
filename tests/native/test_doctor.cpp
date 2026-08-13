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

glyphrelay::EnvironmentSnapshot linux_snapshot() {
  glyphrelay::EnvironmentSnapshot snapshot;
  snapshot.operating_system = "linux";
  snapshot.architecture = "x86_64";
  snapshot.environment = {{"XDG_SESSION_TYPE", "wayland"},
                          {"DBUS_SESSION_BUS_ADDRESS", "redacted-test-address"},
                          {"HOME", "/private/home/should-never-appear"},
                          {"USER", "private-user"},
                          {"GLYPHRELAY_TURN_URL", "turn:secret-host.invalid"}};
  snapshot.openh264_library_available = true;
  snapshot.nvenc_driver_library_available = true;
  snapshot.chromium_available = true;
  return snapshot;
}

} // namespace

int main() {
  const auto linux_report = glyphrelay::build_doctor_report(linux_snapshot());
  require(linux_report.schema_version == 1, "doctor schema version must be one");
  require(linux_report.operating_system == "linux", "operating system must be preserved");
  require(linux_report.desktop_session == "wayland", "known session type must be preserved");
  require(linux_report.mode == "diagnostic_only",
          "unprobed Linux host must not claim enhanced mode");
  require(linux_report.emphasis_map.status == "not_probed",
          "emphasis support must remain unverified until an NVENC query runs");

  const auto json = glyphrelay::doctor_report_json(linux_report);
  require(json.find("/private/home") == std::string::npos, "home path must be redacted");
  require(json.find("private-user") == std::string::npos, "username must be redacted");
  require(json.find("secret-host") == std::string::npos, "TURN host must be redacted");
  require(json.find("redacted-test-address") == std::string::npos,
          "session bus address must be redacted");
  require(json.find("\"schema_version\":1") != std::string::npos,
          "JSON must include its schema version");
  require(json.find("8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228") !=
              std::string::npos,
          "JSON must report the candidate header hash");

  auto unsupported = linux_snapshot();
  unsupported.operating_system = "macos";
  unsupported.architecture = "arm64";
  unsupported.openh264_library_available = false;
  const auto unsupported_report = glyphrelay::build_doctor_report(unsupported);
  require(unsupported_report.mode == "unsupported_sender",
          "non-Linux sender must be rejected truthfully");
  require(unsupported_report.xdg_portal.status == "unsupported",
          "portal capture must not be claimed outside Linux");
  return 0;
}
