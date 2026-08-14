#include "glyphrelay/cuda_context.hpp"
#include "glyphrelay/dependency_versions.hpp"
#include "glyphrelay/doctor.hpp"
#include "glyphrelay/nvenc_probe.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace glyphrelay {
namespace {

constexpr std::size_t kMaximumCommandOutput = 16U * 1024U;
constexpr auto kCommandTimeout = std::chrono::seconds(5);

struct CommandResult {
  bool succeeded = false;
  std::string output;
};

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1U);
}

CommandResult run_command(const std::vector<std::string> &arguments) {
#if defined(__APPLE__) || defined(__linux__)
  if (arguments.empty()) {
    return {};
  }
  std::array<int, 2> descriptors{};
  if (pipe(descriptors.data()) != 0) {
    return {};
  }

  posix_spawn_file_actions_t actions{};
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return {};
  }
  static_cast<void>(posix_spawn_file_actions_addclose(&actions, descriptors[0]));
  static_cast<void>(posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO));
  static_cast<void>(posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO));
  static_cast<void>(posix_spawn_file_actions_addclose(&actions, descriptors[1]));

  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1U);
  for (const auto &argument : arguments) {
    argv.push_back(const_cast<char *>(argument.c_str()));
  }
  argv.push_back(nullptr);

  pid_t child = 0;
  const int spawn_status =
      posix_spawnp(&child, arguments.front().c_str(), &actions, nullptr, argv.data(), environ);
  static_cast<void>(posix_spawn_file_actions_destroy(&actions));
  close(descriptors[1]);
  if (spawn_status != 0) {
    close(descriptors[0]);
    return {};
  }

  const int current_flags = fcntl(descriptors[0], F_GETFL, 0);
  if (current_flags < 0 || fcntl(descriptors[0], F_SETFL, current_flags | O_NONBLOCK) < 0) {
    close(descriptors[0]);
    static_cast<void>(kill(child, SIGKILL));
    int ignored_status = 0;
    while (waitpid(child, &ignored_status, 0) < 0 && errno == EINTR) {
    }
    return {};
  }

  std::string output;
  std::array<char, 1024> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + kCommandTimeout;
  bool timed_out = false;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      timed_out = true;
      break;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{descriptors[0], POLLIN, 0};
    const auto ready = poll(&descriptor, 1, static_cast<int>(remaining));
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0) {
      timed_out = true;
      break;
    }
    while (true) {
      const auto count = read(descriptors[0], buffer.data(), buffer.size());
      if (count > 0) {
        const auto available =
            kMaximumCommandOutput - std::min(output.size(), kMaximumCommandOutput);
        const auto received = static_cast<std::size_t>(count);
        output.append(buffer.data(), std::min(received, available));
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count == 0) {
        descriptor.revents = POLLHUP;
      }
      break;
    }
    if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
      break;
    }
  }
  close(descriptors[0]);

  if (timed_out) {
    static_cast<void>(kill(child, SIGKILL));
  }
  int status = 0;
  pid_t waited = -1;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (timed_out || waited != child) {
    return {};
  }
  return {WIFEXITED(status) && WEXITSTATUS(status) == 0, trim(std::move(output))};
#else
  static_cast<void>(arguments);
  return {};
#endif
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
    if (!path.empty() && access((std::filesystem::path(path) / command).c_str(), X_OK) == 0) {
      return true;
    }
  }
#else
  static_cast<void>(command);
#endif
  return false;
}

std::optional<std::string> numeric_version(std::string_view value) {
  for (std::size_t start = 0; start < value.size(); ++start) {
    if (value[start] < '0' || value[start] > '9') {
      continue;
    }
    std::size_t end = start;
    bool has_dot = false;
    while (end < value.size() && ((value[end] >= '0' && value[end] <= '9') || value[end] == '.')) {
      has_dot = has_dot || value[end] == '.';
      ++end;
    }
    if (has_dot) {
      return std::string(value.substr(start, end - start));
    }
    start = end;
  }
  return std::nullopt;
}

#if defined(__linux__)
std::optional<unsigned int> version_major(std::string_view value) {
  const auto version = numeric_version(value);
  if (!version) {
    return std::nullopt;
  }
  unsigned int major = 0;
  for (const char character : *version) {
    if (character == '.') {
      return major;
    }
    major = major * 10U + static_cast<unsigned int>(character - '0');
  }
  return major;
}
#endif

std::optional<unsigned int> unsigned_property(std::string_view value) {
  constexpr std::string_view marker = "uint32 ";
  const auto marker_position = value.find(marker);
  if (marker_position == std::string_view::npos) {
    return std::nullopt;
  }
  const auto start = marker_position + marker.size();
  unsigned int result = 0;
  bool found = false;
  for (std::size_t index = start; index < value.size(); ++index) {
    const char character = value[index];
    if (character < '0' || character > '9') {
      break;
    }
    found = true;
    result = result * 10U + static_cast<unsigned int>(character - '0');
  }
  return found ? std::optional(result) : std::nullopt;
}

std::vector<std::string> portal_flags(unsigned int value,
                                      const std::array<std::string_view, 3> &names) {
  std::vector<std::string> result;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if ((value & (1U << index)) != 0U) {
      result.emplace_back(names[index]);
    }
  }
  return result;
}

std::string portal_backend() {
  const auto names =
      run_command({"gdbus", "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path",
                   "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.ListNames"});
  if (!names.succeeded) {
    return "unknown";
  }
  constexpr std::string_view prefix = "org.freedesktop.impl.portal.desktop.";
  const auto position = names.output.find(prefix);
  if (position == std::string::npos) {
    return "unknown";
  }
  std::size_t end = position + prefix.size();
  while (end < names.output.size()) {
    const char character = names.output[end];
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '_' || character == '-')) {
      break;
    }
    ++end;
  }
  return names.output.substr(position, end - position);
}

void probe_portal(EnvironmentSnapshot &snapshot, bool session_bus_available) {
  if (!session_bus_available) {
    snapshot.xdg_portal = {"unavailable", "desktop_session_bus_unavailable"};
    return;
  }
  if (!command_available("gdbus")) {
    snapshot.xdg_portal = {"not_probed", "gdbus_unavailable"};
    return;
  }
  const auto source_types = run_command(
      {"gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop", "--object-path",
       "/org/freedesktop/portal/desktop", "--method", "org.freedesktop.DBus.Properties.Get",
       "org.freedesktop.portal.ScreenCast", "AvailableSourceTypes"});
  const auto cursor_modes = run_command(
      {"gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop", "--object-path",
       "/org/freedesktop/portal/desktop", "--method", "org.freedesktop.DBus.Properties.Get",
       "org.freedesktop.portal.ScreenCast", "AvailableCursorModes"});
  const auto version = run_command(
      {"gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop", "--object-path",
       "/org/freedesktop/portal/desktop", "--method", "org.freedesktop.DBus.Properties.Get",
       "org.freedesktop.portal.ScreenCast", "version"});
  const auto source_value = unsigned_property(source_types.output);
  const auto cursor_value = unsigned_property(cursor_modes.output);
  if (!source_types.succeeded || !cursor_modes.succeeded || !source_value || !cursor_value) {
    snapshot.xdg_portal = {"unavailable", "screencast_portal_unavailable"};
    return;
  }
  snapshot.portal_source_types = portal_flags(*source_value, {"monitor", "window", "virtual"});
  snapshot.portal_cursor_modes = portal_flags(*cursor_value, {"hidden", "embedded", "metadata"});
  snapshot.portal_backend = portal_backend();
  if (const auto portal_api_version = unsigned_property(version.output);
      version.succeeded && portal_api_version) {
    snapshot.portal_version = std::to_string(*portal_api_version);
  } else {
    snapshot.portal_version = "unknown";
  }
  snapshot.xdg_portal =
      std::find(snapshot.portal_source_types.begin(), snapshot.portal_source_types.end(),
                "window") != snapshot.portal_source_types.end()
          ? ProbeResult{"available", "window_screencast_portal_available"}
          : ProbeResult{"incompatible", "window_source_type_unavailable"};
}

#if defined(__APPLE__) || defined(__linux__)
template <typename Function> Function load_function(void *library, const char *name) {
  const void *symbol = dlsym(library, name);
  Function function = nullptr;
  static_assert(sizeof(function) == sizeof(symbol));
  std::memcpy(&function, &symbol, sizeof(function));
  return function;
}

void *open_library(const std::vector<const char *> &names) {
  for (const char *name : names) {
    if (void *library = dlopen(name, RTLD_LAZY | RTLD_LOCAL); library != nullptr) {
      return library;
    }
  }
  return nullptr;
}
#endif

#if defined(__linux__)
std::string cuda_version_string(int version) {
  if (version <= 0) {
    return "unavailable";
  }
  const int major = version / 1000;
  const int minor = (version % 1000) / 10;
  return std::to_string(major) + "." + std::to_string(minor);
}
#endif

void probe_pipewire(EnvironmentSnapshot &snapshot) {
#if defined(__linux__)
  void *library = open_library({"libpipewire-0.3.so.0", "libpipewire-0.3.so"});
  if (library == nullptr) {
    snapshot.pipewire = {"unavailable", "pipewire_library_unavailable"};
    return;
  }
  using GetVersion = const char *(*)();
  const auto get_version = load_function<GetVersion>(library, "pw_get_library_version");
  if (get_version != nullptr) {
    const char *version = get_version();
    snapshot.pipewire_version = version == nullptr ? "unknown" : trim(version);
    snapshot.pipewire = {"available", "pipewire_library_loadable"};
  } else {
    snapshot.pipewire = {"incompatible", "pipewire_version_symbol_unavailable"};
  }
  dlclose(library);
#else
  snapshot.pipewire = {"unsupported", "linux_sender_only"};
#endif
}

void probe_cuda(EnvironmentSnapshot &snapshot) {
#if defined(__linux__)
  void *driver = open_library({"libcuda.so.1", "libcuda.so"});
  if (driver == nullptr) {
    snapshot.cuda_driver = {"unavailable", "cuda_driver_library_unavailable"};
  } else {
    using CuInit = int (*)(unsigned int);
    using CuDriverGetVersion = int (*)(int *);
    const auto initialize = load_function<CuInit>(driver, "cuInit");
    const auto get_version = load_function<CuDriverGetVersion>(driver, "cuDriverGetVersion");
    int version = 0;
    if (initialize != nullptr && get_version != nullptr && initialize(0U) == 0 &&
        get_version(&version) == 0) {
      snapshot.cuda_driver = {"available", "cuda_driver_initialized"};
      snapshot.cuda_driver_version = cuda_version_string(version);
    } else {
      snapshot.cuda_driver = {"incompatible", "cuda_driver_initialization_failed"};
    }
    dlclose(driver);
  }

  void *runtime = open_library({"libcudart.so", "libcudart.so.13", "libcudart.so.12"});
  if (runtime == nullptr) {
    snapshot.cuda_runtime = {"unavailable", "cuda_runtime_library_unavailable"};
  } else {
    using RuntimeGetVersion = int (*)(int *);
    const auto get_version = load_function<RuntimeGetVersion>(runtime, "cudaRuntimeGetVersion");
    int version = 0;
    if (get_version != nullptr && get_version(&version) == 0) {
      snapshot.cuda_runtime = {"available", "cuda_runtime_version_queried"};
      snapshot.cuda_runtime_version = cuda_version_string(version);
    } else {
      snapshot.cuda_runtime = {"incompatible", "cuda_runtime_version_query_failed"};
    }
    dlclose(runtime);
  }
#else
  snapshot.cuda_driver = {"unsupported", "linux_sender_only"};
  snapshot.cuda_runtime = {"unsupported", "linux_sender_only"};
#endif
}

void probe_nvenc(EnvironmentSnapshot &snapshot) {
#if defined(__linux__)
  void *library = open_library({"libnvidia-encode.so.1", "libnvidia-encode.so"});
  if (library == nullptr) {
    snapshot.nvenc_api_compatibility = {"unavailable", "nvenc_driver_library_unavailable"};
    snapshot.h264_nvenc = {"unavailable", "nvenc_driver_library_unavailable"};
    snapshot.emphasis_map = {"unavailable", "nvenc_driver_library_unavailable"};
    return;
  }
  using GetMaxVersion = int (*)(std::uint32_t *);
  const auto get_max_version =
      load_function<GetMaxVersion>(library, "NvEncodeAPIGetMaxSupportedVersion");
  std::uint32_t maximum = 0;
  if (get_max_version == nullptr || get_max_version(&maximum) != 0) {
    snapshot.nvenc_api_compatibility = {"incompatible", "nvenc_api_version_query_failed"};
  } else {
    const auto major = maximum & 0xFFU;
    const auto minor = (maximum >> 24U) & 0xFFU;
    snapshot.nvenc_max_api_version = std::to_string(major) + "." + std::to_string(minor);
    const std::uint32_t compiled =
        static_cast<std::uint32_t>(dependency_versions::nvenc_api_major) |
        (static_cast<std::uint32_t>(dependency_versions::nvenc_api_minor) << 24U);
    const auto driver_major = version_major(snapshot.nvidia_driver_version);
    if (driver_major && *driver_major < 610U) {
      snapshot.nvenc_api_compatibility = {"incompatible", "nvidia_driver_below_header_minimum"};
    } else {
      snapshot.nvenc_api_compatibility =
          nvenc_api_version_compatible(maximum, compiled)
              ? ProbeResult{"available", "compiled_nvenc_api_supported"}
              : ProbeResult{"incompatible", "compiled_nvenc_api_too_new"};
    }
  }
  dlclose(library);
#if GLYPHRELAY_HAS_NVENC
  if (snapshot.nvenc_api_compatibility.status != "available") {
    snapshot.h264_nvenc = {"incompatible", "nvenc_api_compatibility_required"};
    snapshot.emphasis_map = {"incompatible", "nvenc_api_compatibility_required"};
    return;
  }
  CudaPrimaryContext context(0);
  const auto capability = probe_nvenc_capabilities(context);
  snapshot.h264_nvenc = capability.h264 ? ProbeResult{"available", "h264_nvenc_supported"}
                                        : ProbeResult{"unavailable", "h264_nvenc_unsupported"};
  snapshot.emphasis_map = capability.emphasis_map
                              ? ProbeResult{"available", "emphasis_map_supported"}
                              : ProbeResult{"unavailable", "emphasis_map_unsupported"};
  if (capability.nv12) {
    snapshot.nvenc_input_formats.push_back("nv12");
  }
  snapshot.maximum_width = capability.maximum_width;
  snapshot.maximum_height = capability.maximum_height;
  if (!context.shutdown()) {
    snapshot.h264_nvenc = {"error", "cuda_primary_context_release_failed"};
    snapshot.emphasis_map = {"error", "cuda_primary_context_release_failed"};
  }
#else
  snapshot.h264_nvenc = {"not_probed", "nvenc_device_capability_query_required"};
  snapshot.emphasis_map = {"not_probed", "nvenc_device_capability_query_required"};
#endif
#else
  snapshot.nvenc_api_compatibility = {"unsupported", "linux_sender_only"};
  snapshot.h264_nvenc = {"unsupported", "linux_sender_only"};
  snapshot.emphasis_map = {"unsupported", "linux_sender_only"};
#endif
}

void probe_openh264(EnvironmentSnapshot &snapshot) {
#if defined(__APPLE__) || defined(__linux__)
  void *library =
      open_library({"libopenh264.so.7", "libopenh264.so.6", "libopenh264.so", "libopenh264.dylib"});
  if (library == nullptr) {
    snapshot.cpu_encoder = {"unavailable", "system_openh264_library_unavailable"};
    return;
  }
  struct OpenH264Version {
    unsigned int major;
    unsigned int minor;
    unsigned int revision;
    unsigned int reserved;
  };
  using GetVersion = void (*)(OpenH264Version *);
  const auto get_version = load_function<GetVersion>(library, "WelsGetCodecVersionEx");
  if (get_version == nullptr) {
    snapshot.cpu_encoder = {"incompatible", "openh264_version_symbol_unavailable"};
  } else {
    OpenH264Version version{};
    get_version(&version);
    snapshot.openh264_version = std::to_string(version.major) + "." +
                                std::to_string(version.minor) + "." +
                                std::to_string(version.revision);
    snapshot.cpu_encoder = {"available", "system_openh264_library_loadable"};
  }
  dlclose(library);
#else
  snapshot.cpu_encoder = {"unsupported", "unsupported_platform"};
#endif
}

std::pair<ProbeResult, std::string> probe_browser(const char *path_environment,
                                                  const std::vector<std::string> &commands,
                                                  const std::string &missing_reason) {
  std::vector<std::string> candidates;
  if (const char *configured_path = std::getenv(path_environment);
      configured_path != nullptr && configured_path[0] != '\0') {
    candidates.emplace_back(configured_path);
  }
  for (const auto &command : commands) {
    if (command.find('/') != std::string::npos || command_available(command)) {
      candidates.push_back(command);
    }
  }
  for (const auto &candidate : candidates) {
    const auto result = run_command({candidate, "--version"});
    if (!result.succeeded) {
      continue;
    }
    if (const auto version = numeric_version(result.output); version) {
      return {{"available", "browser_version_queried"}, *version};
    }
    return {{"not_probed", "browser_version_unparsed"}, "unparsed"};
  }
  return {{"unavailable", missing_reason}, "unavailable"};
}

void probe_gpu_inventory(EnvironmentSnapshot &snapshot) {
  if (!command_available("nvidia-smi")) {
    return;
  }
  const auto result = run_command(
      {"nvidia-smi", "--query-gpu=name,compute_cap,driver_version", "--format=csv,noheader"});
  if (!result.succeeded || result.output.empty()) {
    return;
  }
  std::stringstream lines(result.output);
  std::string line;
  std::vector<std::string> devices;
  while (std::getline(lines, line)) {
    if (!trim(line).empty()) {
      devices.push_back(trim(line));
    }
  }
  snapshot.visible_gpu_count = static_cast<int>(devices.size());
  if (devices.size() != 1U) {
    snapshot.gpu_model = devices.empty() ? "unavailable" : "multiple_visible_gpus";
    snapshot.gpu_architecture = devices.empty() ? "unavailable" : "selection_required";
    snapshot.nvidia_driver_version = devices.empty() ? "unavailable" : "selection_required";
    return;
  }
  std::stringstream fields(devices.front());
  std::string model;
  std::string compute_capability;
  std::string driver;
  std::getline(fields, model, ',');
  std::getline(fields, compute_capability, ',');
  std::getline(fields, driver, ',');
  snapshot.gpu_model = trim(std::move(model));
  snapshot.gpu_architecture = "compute_" + trim(std::move(compute_capability));
  snapshot.nvidia_driver_version = trim(std::move(driver));
}

ProbeResult signaling_origin_probe() {
  const char *origin = std::getenv("GLYPHRELAY_SIGNALING_ORIGIN");
  if (origin == nullptr || origin[0] == '\0') {
    return {"unavailable", "signaling_origin_not_configured"};
  }
  const std::string_view value(origin);
  const auto scheme_end = value.find("://");
  if (scheme_end == std::string_view::npos) {
    return {"incompatible", "invalid_signaling_origin"};
  }
  const auto authority_start = scheme_end + 3U;
  const auto authority_end = value.find_first_of("/?#", authority_start);
  const auto authority = value.substr(authority_start, authority_end == std::string_view::npos
                                                           ? value.size() - authority_start
                                                           : authority_end - authority_start);
  if (authority.empty() || authority.find('@') != std::string_view::npos ||
      authority.find_first_of(" \t\r\n") != std::string_view::npos) {
    return {"incompatible", "invalid_signaling_origin"};
  }
  if (value.starts_with("https://")) {
    return {"available", "secure_signaling_origin_configured"};
  }
  const auto port_separator = authority.rfind(':');
  const auto host =
      authority.starts_with('[')
          ? authority.substr(0U, authority.find(']') + 1U)
          : authority.substr(0U, port_separator == std::string_view::npos ? authority.size()
                                                                          : port_separator);
  if (value.starts_with("http://") &&
      (host == "127.0.0.1" || host == "localhost" || host == "[::1]")) {
    return {"available", "loopback_signaling_origin_configured"};
  }
  return {"incompatible", "insecure_non_loopback_signaling_origin"};
}

ProbeResult turn_probe() {
  const char *turn_url = std::getenv("GLYPHRELAY_TURN_URL");
  if (turn_url == nullptr || turn_url[0] == '\0') {
    return {"unavailable", "turn_not_configured"};
  }
  const std::string_view value(turn_url);
  if (!value.starts_with("turn:") || value.find('@') != std::string_view::npos ||
      value.find_first_of(" \t\r\n") != std::string_view::npos) {
    return {"incompatible", "turn_udp_url_required"};
  }
  const auto transport = value.find("transport=");
  if (transport != std::string_view::npos) {
    const auto transport_value = value.substr(transport + std::string_view("transport=").size());
    if (!(transport_value == "udp" || transport_value.starts_with("udp&"))) {
      return {"incompatible", "turn_udp_transport_required"};
    }
  }
  return {"not_probed", "turn_udp_connectivity_test_required"};
}

std::string normalized_session(const char *session) {
  if (session == nullptr || session[0] == '\0') {
    return "unavailable";
  }
  const std::string_view value(session);
  return value == "wayland" || value == "x11" ? std::string(value) : "other";
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
  snapshot.desktop_session = normalized_session(std::getenv("XDG_SESSION_TYPE"));

  if (snapshot.operating_system == "linux") {
    probe_portal(snapshot, std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr);
    snapshot.dmabuf_import = {"not_probed", "selected_capture_format_required"};
    snapshot.shared_memory_capture = {"not_probed", "portal_capture_session_required"};
    snapshot.screen_lock_hook = {"not_probed", "desktop_lock_signal_test_required"};
    snapshot.capture_revocation_hook = {"not_probed", "portal_revocation_test_required"};
  } else {
    snapshot.xdg_portal = {"unsupported", "linux_sender_only"};
    snapshot.dmabuf_import = {"unsupported", "linux_sender_only"};
    snapshot.shared_memory_capture = {"unsupported", "linux_sender_only"};
    snapshot.screen_lock_hook = {"unsupported", "linux_sender_only"};
    snapshot.capture_revocation_hook = {"unsupported", "linux_sender_only"};
  }
  probe_pipewire(snapshot);
  probe_gpu_inventory(snapshot);
  probe_cuda(snapshot);
  probe_nvenc(snapshot);
  probe_openh264(snapshot);
  snapshot.recording_profile_compatibility = {"not_probed", "browser_offers_required"};

#if GLYPHRELAY_HAS_CUDA_COMPILER
  snapshot.cuda_toolkit_version = GLYPHRELAY_CUDA_COMPILER_VERSION;
#endif

  const auto chromium =
      probe_browser("GLYPHRELAY_CHROMIUM_PATH",
                    {"chromium", "chromium-browser", "google-chrome"
#if defined(__APPLE__)
                     ,
                     "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
#endif
                    },
                    "chromium_binary_unavailable");
  snapshot.chromium = chromium.first;
  snapshot.chromium_version = chromium.second;
  const auto firefox =
      probe_browser("GLYPHRELAY_FIREFOX_PATH", {"firefox"}, "firefox_binary_unavailable");
  snapshot.firefox = firefox.first;
  snapshot.firefox_version = firefox.second;
  snapshot.signaling_origin = signaling_origin_probe();
  snapshot.turn_configuration = turn_probe();
  snapshot.wire_cap_accounting = {"unavailable", "transport_hook_not_initialized"};
  return snapshot;
}

} // namespace glyphrelay
