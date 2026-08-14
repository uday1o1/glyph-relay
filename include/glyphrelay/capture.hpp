#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glyphrelay {

enum class PackedPixelOrder { bgra, rgba };
enum class CursorMode { hidden, embedded, metadata };
enum class CaptureOrientation {
  upright,
  rotate90,
  rotate180,
  rotate270,
  flipped,
  flipped90,
  flipped180,
  flipped270,
};
enum class CaptureState {
  idle,
  creating_session,
  selecting_sources,
  starting,
  streaming,
  cancelled,
  closed,
  revoked,
  disconnected,
};

struct PortalSelectionContract {
  bool window_sources_only = true;
  bool multiple = false;
  std::uint32_t persist_mode = 0;
  std::vector<CursorMode> cursor_preference = {CursorMode::metadata, CursorMode::embedded,
                                               CursorMode::hidden};
};

struct PortalTransition {
  bool passed = false;
  std::string reason;
  CaptureState state = CaptureState::idle;
};

class PortalSelectionStateMachine {
public:
  PortalTransition begin(std::string request_handle);
  PortalTransition session_created(std::string_view request_handle, std::string session_handle,
                                   std::string select_request_handle);
  PortalTransition sources_selected(std::string_view request_handle,
                                    std::string start_request_handle);
  PortalTransition started(std::string_view request_handle, std::uint32_t pipewire_node_id);
  PortalTransition cancel(std::string_view request_handle);
  PortalTransition close(CaptureState terminal_state);

  CaptureState state() const;
  std::string_view session_handle() const;
  std::uint32_t pipewire_node_id() const;
  const PortalSelectionContract &contract() const;

private:
  PortalTransition transition(CaptureState state, std::string reason);
  bool expected(std::string_view request_handle) const;

  PortalSelectionContract contract_;
  CaptureState state_ = CaptureState::idle;
  std::string request_handle_;
  std::string session_handle_;
  std::uint32_t pipewire_node_id_ = 0;
};

struct CaptureCrop {
  std::size_t x = 0;
  std::size_t y = 0;
  std::size_t width = 0;
  std::size_t height = 0;
};

struct DamageRectangle {
  std::size_t x = 0;
  std::size_t y = 0;
  std::size_t width = 0;
  std::size_t height = 0;
};

struct CursorMetadataView {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t hotspot_x = 0;
  std::int32_t hotspot_y = 0;
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t pitch = 0;
  std::span<const std::uint8_t> rgba;
};

struct SharedMemoryBufferView {
  std::span<const std::uint8_t> bytes;
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t pitch = 0;
  CaptureCrop crop;
  PackedPixelOrder pixel_order = PackedPixelOrder::bgra;
  CaptureOrientation orientation = CaptureOrientation::upright;
  CursorMode cursor_mode = CursorMode::hidden;
  std::optional<CursorMetadataView> cursor;
  std::span<const DamageRectangle> damage;
};

struct CaptureGeometry {
  std::uint64_t epoch = 0;
  std::size_t source_width = 0;
  std::size_t source_height = 0;
  CaptureCrop source_crop;
  std::size_t visible_width = 0;
  std::size_t visible_height = 0;
  std::size_t coded_width = 0;
  std::size_t coded_height = 0;
  CaptureOrientation source_orientation = CaptureOrientation::upright;
};

struct CapturedFrame {
  std::uint64_t frame_id = 0;
  std::uint64_t monotonic_timestamp_ns = 0;
  CaptureGeometry geometry;
  PackedPixelOrder pixel_order = PackedPixelOrder::bgra;
  std::size_t pitch = 0;
  CursorMode cursor_mode = CursorMode::hidden;
  std::optional<std::pair<std::int32_t, std::int32_t>> cursor_position;
  std::vector<DamageRectangle> damage;
  std::vector<std::uint8_t> pixels;
};

class CapturedFrameLease {
public:
  CapturedFrameLease() = default;
  ~CapturedFrameLease();
  CapturedFrameLease(CapturedFrameLease &&other) noexcept;
  CapturedFrameLease &operator=(CapturedFrameLease &&other) noexcept;
  CapturedFrameLease(const CapturedFrameLease &) = delete;
  CapturedFrameLease &operator=(const CapturedFrameLease &) = delete;

  explicit operator bool() const;
  const CapturedFrame &frame() const;
  void reset();

private:
  friend class SharedMemoryCapturePool;
  CapturedFrameLease(std::shared_ptr<const CapturedFrame> frame, std::function<void()> release);

  std::shared_ptr<const CapturedFrame> frame_;
  std::function<void()> release_;
};

struct CapturePoolDiagnostics {
  std::size_t capacity = 0;
  std::size_t ready = 0;
  std::size_t leased = 0;
  std::uint64_t accepted = 0;
  std::uint64_t dropped_oldest = 0;
  std::uint64_t dropped_starved = 0;
  std::uint64_t rejected = 0;
  std::uint64_t requeued = 0;
  std::uint64_t requeue_failures = 0;
  bool admission_open = true;
  CaptureState terminal_state = CaptureState::idle;
};

struct CaptureIngressResult {
  bool accepted = false;
  bool requeued = false;
  std::string reason;
  std::uint64_t frame_id = 0;
  std::uint64_t geometry_epoch = 0;
};

class SharedMemoryCapturePool {
public:
  explicit SharedMemoryCapturePool(std::size_t capacity);
  ~SharedMemoryCapturePool();
  SharedMemoryCapturePool(SharedMemoryCapturePool &&) noexcept;
  SharedMemoryCapturePool &operator=(SharedMemoryCapturePool &&) noexcept;
  SharedMemoryCapturePool(const SharedMemoryCapturePool &) = delete;
  SharedMemoryCapturePool &operator=(const SharedMemoryCapturePool &) = delete;

  CaptureIngressResult ingest(const SharedMemoryBufferView &buffer,
                              std::uint64_t monotonic_timestamp_ns,
                              const std::function<void()> &release_on_pipewire_loop);
  std::optional<CapturedFrameLease> take_latest();
  void stop(CaptureState terminal_state);
  CapturePoolDiagnostics diagnostics() const;

private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

std::string_view packed_pixel_order_name(PackedPixelOrder order);
std::string_view cursor_mode_name(CursorMode mode);
std::string_view capture_state_name(CaptureState state);
std::string_view capture_orientation_name(CaptureOrientation orientation);

} // namespace glyphrelay
