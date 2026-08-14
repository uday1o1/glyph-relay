#include "glyphrelay/linux_capture.hpp"

#include <stdexcept>
#include <utility>

namespace glyphrelay {

PortalWindowGrant::PortalWindowGrant(int pipewire_remote_fd, std::uint32_t pipewire_node_id,
                                     CursorMode cursor_mode)
    : pipewire_remote_fd_(pipewire_remote_fd), pipewire_node_id_(pipewire_node_id),
      cursor_mode_(cursor_mode) {}

PortalWindowGrant::~PortalWindowGrant() { reset(); }

PortalWindowGrant::PortalWindowGrant(PortalWindowGrant &&other) noexcept
    : pipewire_remote_fd_(std::exchange(other.pipewire_remote_fd_, -1)),
      pipewire_node_id_(std::exchange(other.pipewire_node_id_, 0U)),
      cursor_mode_(other.cursor_mode_) {}

PortalWindowGrant &PortalWindowGrant::operator=(PortalWindowGrant &&other) noexcept {
  if (this != &other) {
    reset();
    pipewire_remote_fd_ = std::exchange(other.pipewire_remote_fd_, -1);
    pipewire_node_id_ = std::exchange(other.pipewire_node_id_, 0U);
    cursor_mode_ = other.cursor_mode_;
  }
  return *this;
}

PortalWindowGrant::operator bool() const {
  return pipewire_remote_fd_ >= 0 && pipewire_node_id_ != 0U;
}

std::uint32_t PortalWindowGrant::pipewire_node_id() const { return pipewire_node_id_; }
CursorMode PortalWindowGrant::cursor_mode() const { return cursor_mode_; }

int PortalWindowGrant::release_pipewire_remote_fd() {
  pipewire_node_id_ = 0U;
  return std::exchange(pipewire_remote_fd_, -1);
}

void PortalWindowGrant::reset() {
  pipewire_remote_fd_ = -1;
  pipewire_node_id_ = 0U;
}

struct LinuxPortalClient::Implementation {};

LinuxPortalClient::LinuxPortalClient() : implementation_(std::make_unique<Implementation>()) {}
LinuxPortalClient::~LinuxPortalClient() = default;
LinuxPortalClient::LinuxPortalClient(LinuxPortalClient &&) noexcept = default;
LinuxPortalClient &LinuxPortalClient::operator=(LinuxPortalClient &&) noexcept = default;

PortalOpenResult LinuxPortalClient::open_window(std::string_view, std::uint32_t) {
  return {.passed = false,
          .cancelled = false,
          .reason = "linux_capture_backend_unavailable",
          .capabilities = {},
          .grant = std::nullopt};
}

std::optional<CaptureState> LinuxPortalClient::poll_terminal() { return std::nullopt; }

CaptureOperationResult LinuxPortalClient::close() {
  return {true, false, "linux_capture_session_closed"};
}

struct LinuxPipeWireCapture::Implementation {};

LinuxPipeWireCapture::LinuxPipeWireCapture()
    : implementation_(std::make_unique<Implementation>()) {}
LinuxPipeWireCapture::~LinuxPipeWireCapture() = default;
LinuxPipeWireCapture::LinuxPipeWireCapture(LinuxPipeWireCapture &&) noexcept = default;
LinuxPipeWireCapture &LinuxPipeWireCapture::operator=(LinuxPipeWireCapture &&) noexcept = default;

CaptureOperationResult LinuxPipeWireCapture::start(PortalWindowGrant, SharedMemoryCapturePool &,
                                                   CaptureEventCallback) {
  return {false, false, "linux_capture_backend_unavailable"};
}

CaptureOperationResult LinuxPipeWireCapture::stop(CaptureState terminal_state) {
  if (terminal_state != CaptureState::cancelled && terminal_state != CaptureState::closed &&
      terminal_state != CaptureState::revoked && terminal_state != CaptureState::disconnected) {
    throw std::invalid_argument("PipeWire stop requires a terminal capture state");
  }
  return {true, false, "linux_capture_stream_stopped"};
}

bool LinuxPipeWireCapture::running() const { return false; }

bool linux_capture_backend_available() { return false; }
std::string_view linux_capture_backend_version() { return "unavailable"; }

} // namespace glyphrelay
