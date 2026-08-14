#pragma once

#include "glyphrelay/capture.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace glyphrelay {

struct CaptureOperationResult {
  bool passed = false;
  bool cancelled = false;
  std::string reason;
};

struct PortalCapabilities {
  std::uint32_t version = 0;
  std::uint32_t available_source_types = 0;
  std::uint32_t available_cursor_modes = 0;
  CursorMode selected_cursor_mode = CursorMode::hidden;
};

class PortalWindowGrant {
public:
  PortalWindowGrant() = default;
  ~PortalWindowGrant();
  PortalWindowGrant(PortalWindowGrant &&other) noexcept;
  PortalWindowGrant &operator=(PortalWindowGrant &&other) noexcept;
  PortalWindowGrant(const PortalWindowGrant &) = delete;
  PortalWindowGrant &operator=(const PortalWindowGrant &) = delete;

  explicit operator bool() const;
  std::uint32_t pipewire_node_id() const;
  CursorMode cursor_mode() const;
  int release_pipewire_remote_fd();

private:
  friend class LinuxPortalClient;
  PortalWindowGrant(int pipewire_remote_fd, std::uint32_t pipewire_node_id, CursorMode cursor_mode);
  void reset();

  int pipewire_remote_fd_ = -1;
  std::uint32_t pipewire_node_id_ = 0;
  CursorMode cursor_mode_ = CursorMode::hidden;
};

struct PortalOpenResult {
  bool passed = false;
  bool cancelled = false;
  std::string reason;
  PortalCapabilities capabilities;
  std::optional<PortalWindowGrant> grant;
};

class LinuxPortalClient {
public:
  LinuxPortalClient();
  ~LinuxPortalClient();
  LinuxPortalClient(LinuxPortalClient &&) noexcept;
  LinuxPortalClient &operator=(LinuxPortalClient &&) noexcept;
  LinuxPortalClient(const LinuxPortalClient &) = delete;
  LinuxPortalClient &operator=(const LinuxPortalClient &) = delete;

  PortalOpenResult open_window(std::string_view parent_window, std::uint32_t timeout_ms);
  std::optional<CaptureState> poll_terminal();
  CaptureOperationResult close();

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

using CaptureEventCallback = std::function<void(std::string)>;

class LinuxPipeWireCapture {
public:
  LinuxPipeWireCapture();
  ~LinuxPipeWireCapture();
  LinuxPipeWireCapture(LinuxPipeWireCapture &&) noexcept;
  LinuxPipeWireCapture &operator=(LinuxPipeWireCapture &&) noexcept;
  LinuxPipeWireCapture(const LinuxPipeWireCapture &) = delete;
  LinuxPipeWireCapture &operator=(const LinuxPipeWireCapture &) = delete;

  CaptureOperationResult start(PortalWindowGrant grant, SharedMemoryCapturePool &capture_pool,
                               CaptureEventCallback event_callback = {});
  CaptureOperationResult stop(CaptureState terminal_state = CaptureState::closed);
  bool running() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

bool linux_capture_backend_available();
std::string_view linux_capture_backend_version();

} // namespace glyphrelay
