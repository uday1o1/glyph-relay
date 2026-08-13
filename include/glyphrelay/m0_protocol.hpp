#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace glyphrelay {

struct ProtocolComponent {
  std::filesystem::path relative_path;
  std::string sha256;
};

struct M0ProtocolLock {
  std::filesystem::path repository_root;
  std::string manifest_sha256;
  std::vector<ProtocolComponent> components;
};

struct ProtocolVerification {
  bool passed = false;
  std::string reason;
  M0ProtocolLock lock;
};

ProtocolVerification verify_m0_protocol(const std::filesystem::path &manifest_path);

} // namespace glyphrelay
