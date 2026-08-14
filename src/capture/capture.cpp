#include "glyphrelay/capture.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace glyphrelay {
namespace {

bool terminal(CaptureState state) {
  return state == CaptureState::cancelled || state == CaptureState::closed ||
         state == CaptureState::revoked || state == CaptureState::disconnected;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t &result) {
  if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool valid_buffer(const SharedMemoryBufferView &buffer, std::string &reason) {
  if (buffer.width == 0U || buffer.height == 0U || buffer.crop.width == 0U ||
      buffer.crop.height == 0U) {
    reason = "capture_geometry_empty";
    return false;
  }
  if (buffer.crop.x > buffer.width || buffer.crop.y > buffer.height ||
      buffer.crop.width > buffer.width - buffer.crop.x ||
      buffer.crop.height > buffer.height - buffer.crop.y) {
    reason = "capture_crop_out_of_bounds";
    return false;
  }
  std::size_t row_bytes = 0U;
  if (!checked_multiply(buffer.width, 4U, row_bytes) || buffer.pitch < row_bytes) {
    reason = "capture_pitch_invalid";
    return false;
  }
  std::size_t required = 0U;
  if (!checked_multiply(buffer.height - 1U, buffer.pitch, required) ||
      row_bytes > std::numeric_limits<std::size_t>::max() - required ||
      buffer.bytes.size() < required + row_bytes) {
    reason = "capture_buffer_too_small";
    return false;
  }
  if (buffer.damage.size() > 64U) {
    reason = "capture_damage_count_exceeded";
    return false;
  }
  for (const auto &damage : buffer.damage) {
    if (damage.width == 0U || damage.height == 0U || damage.x > buffer.crop.width ||
        damage.y > buffer.crop.height || damage.width > buffer.crop.width - damage.x ||
        damage.height > buffer.crop.height - damage.y) {
      reason = "capture_damage_out_of_bounds";
      return false;
    }
  }
  if (buffer.cursor_mode == CursorMode::metadata && buffer.cursor) {
    const auto &cursor = *buffer.cursor;
    std::size_t cursor_row = 0U;
    std::size_t cursor_required = 0U;
    if (cursor.width == 0U || cursor.height == 0U ||
        !checked_multiply(cursor.width, 4U, cursor_row) || cursor.pitch < cursor_row ||
        !checked_multiply(cursor.height - 1U, cursor.pitch, cursor_required) ||
        cursor_row > std::numeric_limits<std::size_t>::max() - cursor_required ||
        cursor.rgba.size() < cursor_required + cursor_row) {
      reason = "capture_cursor_bitmap_invalid";
      return false;
    }
  }
  return true;
}

bool same_geometry(const CaptureGeometry &geometry, const SharedMemoryBufferView &buffer) {
  return geometry.source_width == buffer.width && geometry.source_height == buffer.height &&
         geometry.source_crop.x == buffer.crop.x && geometry.source_crop.y == buffer.crop.y &&
         geometry.source_crop.width == buffer.crop.width &&
         geometry.source_crop.height == buffer.crop.height &&
         geometry.source_orientation == buffer.orientation;
}

CaptureGeometry make_geometry(std::uint64_t epoch, const SharedMemoryBufferView &buffer) {
  const bool swaps_dimensions = buffer.orientation == CaptureOrientation::rotate90 ||
                                buffer.orientation == CaptureOrientation::rotate270 ||
                                buffer.orientation == CaptureOrientation::flipped90 ||
                                buffer.orientation == CaptureOrientation::flipped270;
  const auto visible_width = swaps_dimensions ? buffer.crop.height : buffer.crop.width;
  const auto visible_height = swaps_dimensions ? buffer.crop.width : buffer.crop.height;
  return {epoch,
          buffer.width,
          buffer.height,
          buffer.crop,
          visible_width,
          visible_height,
          (visible_width + 1U) & ~std::size_t{1U},
          (visible_height + 1U) & ~std::size_t{1U},
          buffer.orientation};
}

std::uint8_t blend(std::uint8_t foreground, std::uint8_t background, std::uint8_t alpha) {
  const auto numerator = static_cast<unsigned int>(foreground) * alpha +
                         static_cast<unsigned int>(background) * (255U - alpha) + 127U;
  return static_cast<std::uint8_t>(numerator / 255U);
}

void composite_cursor(CapturedFrame &frame, const CursorMetadataView &cursor) {
  frame.cursor_position = {cursor.x, cursor.y};
  const auto left = static_cast<std::int64_t>(cursor.x) - cursor.hotspot_x -
                    static_cast<std::int64_t>(frame.geometry.source_crop.x);
  const auto top = static_cast<std::int64_t>(cursor.y) - cursor.hotspot_y -
                   static_cast<std::int64_t>(frame.geometry.source_crop.y);
  for (std::size_t cursor_y = 0U; cursor_y < cursor.height; ++cursor_y) {
    const auto destination_y = top + static_cast<std::int64_t>(cursor_y);
    if (destination_y < 0 ||
        destination_y >= static_cast<std::int64_t>(frame.geometry.visible_height)) {
      continue;
    }
    for (std::size_t cursor_x = 0U; cursor_x < cursor.width; ++cursor_x) {
      const auto destination_x = left + static_cast<std::int64_t>(cursor_x);
      if (destination_x < 0 ||
          destination_x >= static_cast<std::int64_t>(frame.geometry.visible_width)) {
        continue;
      }
      const auto source_offset = cursor_y * cursor.pitch + cursor_x * 4U;
      const auto alpha = cursor.rgba[source_offset + 3U];
      const auto destination_offset = static_cast<std::size_t>(destination_y) * frame.pitch +
                                      static_cast<std::size_t>(destination_x) * 4U;
      const std::array<std::size_t, 3U> source_channels = {0U, 1U, 2U};
      const std::array<std::size_t, 3U> destination_channels =
          frame.pixel_order == PackedPixelOrder::rgba ? std::array<std::size_t, 3U>{0U, 1U, 2U}
                                                      : std::array<std::size_t, 3U>{2U, 1U, 0U};
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        auto &destination = frame.pixels[destination_offset + destination_channels[channel]];
        destination =
            blend(cursor.rgba[source_offset + source_channels[channel]], destination, alpha);
      }
      frame.pixels[destination_offset + 3U] = 255U;
    }
  }
}

std::pair<std::size_t, std::size_t>
source_coordinate(std::size_t destination_x, std::size_t destination_y, std::size_t source_width,
                  std::size_t source_height, CaptureOrientation orientation) {
  switch (orientation) {
  case CaptureOrientation::upright:
    return {destination_x, destination_y};
  case CaptureOrientation::rotate90:
    return {destination_y, source_height - 1U - destination_x};
  case CaptureOrientation::rotate180:
    return {source_width - 1U - destination_x, source_height - 1U - destination_y};
  case CaptureOrientation::rotate270:
    return {source_width - 1U - destination_y, destination_x};
  case CaptureOrientation::flipped:
    return {source_width - 1U - destination_x, destination_y};
  case CaptureOrientation::flipped90:
    return {destination_y, destination_x};
  case CaptureOrientation::flipped180:
    return {destination_x, source_height - 1U - destination_y};
  case CaptureOrientation::flipped270:
    return {source_width - 1U - destination_y, source_height - 1U - destination_x};
  }
  throw std::logic_error("unknown capture orientation");
}

DamageRectangle transform_damage(const DamageRectangle &damage, std::size_t source_width,
                                 std::size_t source_height, CaptureOrientation orientation) {
  switch (orientation) {
  case CaptureOrientation::upright:
    return damage;
  case CaptureOrientation::rotate90:
    return {source_height - damage.y - damage.height, damage.x, damage.height, damage.width};
  case CaptureOrientation::rotate180:
    return {source_width - damage.x - damage.width, source_height - damage.y - damage.height,
            damage.width, damage.height};
  case CaptureOrientation::rotate270:
    return {damage.y, source_width - damage.x - damage.width, damage.height, damage.width};
  case CaptureOrientation::flipped:
    return {source_width - damage.x - damage.width, damage.y, damage.width, damage.height};
  case CaptureOrientation::flipped90:
    return {damage.y, damage.x, damage.height, damage.width};
  case CaptureOrientation::flipped180:
    return {damage.x, source_height - damage.y - damage.height, damage.width, damage.height};
  case CaptureOrientation::flipped270:
    return {source_height - damage.y - damage.height, source_width - damage.x - damage.width,
            damage.height, damage.width};
  }
  throw std::logic_error("unknown capture orientation");
}

std::int32_t clamp_cursor_coordinate(std::int64_t coordinate) {
  return static_cast<std::int32_t>(
      std::clamp<std::int64_t>(coordinate, std::numeric_limits<std::int32_t>::min(),
                               std::numeric_limits<std::int32_t>::max()));
}

std::pair<std::int32_t, std::int32_t> transform_cursor_position(const CursorMetadataView &cursor,
                                                                const CaptureCrop &crop,
                                                                CaptureOrientation orientation) {
  const auto source_x = static_cast<std::int64_t>(cursor.x) - static_cast<std::int64_t>(crop.x);
  const auto source_y = static_cast<std::int64_t>(cursor.y) - static_cast<std::int64_t>(crop.y);
  const auto source_width = static_cast<std::int64_t>(crop.width);
  const auto source_height = static_cast<std::int64_t>(crop.height);
  switch (orientation) {
  case CaptureOrientation::upright:
    return {clamp_cursor_coordinate(source_x), clamp_cursor_coordinate(source_y)};
  case CaptureOrientation::rotate90:
    return {clamp_cursor_coordinate(source_height - 1 - source_y),
            clamp_cursor_coordinate(source_x)};
  case CaptureOrientation::rotate180:
    return {clamp_cursor_coordinate(source_width - 1 - source_x),
            clamp_cursor_coordinate(source_height - 1 - source_y)};
  case CaptureOrientation::rotate270:
    return {clamp_cursor_coordinate(source_y),
            clamp_cursor_coordinate(source_width - 1 - source_x)};
  case CaptureOrientation::flipped:
    return {clamp_cursor_coordinate(source_width - 1 - source_x),
            clamp_cursor_coordinate(source_y)};
  case CaptureOrientation::flipped90:
    return {clamp_cursor_coordinate(source_y), clamp_cursor_coordinate(source_x)};
  case CaptureOrientation::flipped180:
    return {clamp_cursor_coordinate(source_x),
            clamp_cursor_coordinate(source_height - 1 - source_y)};
  case CaptureOrientation::flipped270:
    return {clamp_cursor_coordinate(source_height - 1 - source_y),
            clamp_cursor_coordinate(source_width - 1 - source_x)};
  }
  throw std::logic_error("unknown capture orientation");
}

void normalize_orientation(const CapturedFrame &source, CaptureOrientation orientation,
                           CapturedFrame &destination) {
  destination.pitch = destination.geometry.visible_width * 4U;
  destination.pixels.resize(destination.pitch * destination.geometry.visible_height);
  for (std::size_t y = 0U; y < destination.geometry.visible_height; ++y) {
    for (std::size_t x = 0U; x < destination.geometry.visible_width; ++x) {
      const auto [source_x, source_y] = source_coordinate(
          x, y, source.geometry.visible_width, source.geometry.visible_height, orientation);
      const auto source_offset = source_y * source.pitch + source_x * 4U;
      const auto destination_offset = y * destination.pitch + x * 4U;
      std::copy_n(source.pixels.begin() + static_cast<std::ptrdiff_t>(source_offset), 4U,
                  destination.pixels.begin() + static_cast<std::ptrdiff_t>(destination_offset));
    }
  }
}

} // namespace

PortalTransition PortalSelectionStateMachine::transition(CaptureState state, std::string reason) {
  state_ = state;
  return {true, std::move(reason), state_};
}

bool PortalSelectionStateMachine::expected(std::string_view request_handle) const {
  return !request_handle.empty() && request_handle == request_handle_;
}

PortalTransition PortalSelectionStateMachine::begin(std::string request_handle) {
  if (state_ != CaptureState::idle || request_handle.empty()) {
    return {false, "portal_create_request_invalid", state_};
  }
  request_handle_ = std::move(request_handle);
  return transition(CaptureState::creating_session, "portal_create_session_pending");
}

PortalTransition PortalSelectionStateMachine::session_created(std::string_view request_handle,
                                                              std::string session_handle,
                                                              std::string select_request_handle) {
  if (state_ != CaptureState::creating_session || !expected(request_handle) ||
      session_handle.empty() || select_request_handle.empty()) {
    return {false, "portal_create_response_invalid", state_};
  }
  session_handle_ = std::move(session_handle);
  request_handle_ = std::move(select_request_handle);
  return transition(CaptureState::selecting_sources, "portal_select_sources_pending");
}

PortalTransition PortalSelectionStateMachine::sources_selected(std::string_view request_handle,
                                                               std::string start_request_handle) {
  if (state_ != CaptureState::selecting_sources || !expected(request_handle) ||
      start_request_handle.empty()) {
    return {false, "portal_select_response_invalid", state_};
  }
  request_handle_ = std::move(start_request_handle);
  return transition(CaptureState::starting, "portal_start_pending");
}

PortalTransition PortalSelectionStateMachine::started(std::string_view request_handle,
                                                      std::uint32_t pipewire_node_id) {
  if (state_ != CaptureState::starting || !expected(request_handle) || pipewire_node_id == 0U) {
    return {false, "portal_start_response_invalid", state_};
  }
  request_handle_.clear();
  pipewire_node_id_ = pipewire_node_id;
  return transition(CaptureState::streaming, "portal_streaming");
}

PortalTransition PortalSelectionStateMachine::cancel(std::string_view request_handle) {
  if (terminal(state_) || state_ == CaptureState::streaming || !expected(request_handle)) {
    return {false, "portal_cancel_response_invalid", state_};
  }
  request_handle_.clear();
  session_handle_.clear();
  pipewire_node_id_ = 0U;
  return transition(CaptureState::cancelled, "portal_selection_cancelled");
}

PortalTransition PortalSelectionStateMachine::close(CaptureState terminal_state) {
  if (!terminal(terminal_state) || terminal_state == CaptureState::cancelled || terminal(state_)) {
    return {false, "portal_terminal_transition_invalid", state_};
  }
  request_handle_.clear();
  session_handle_.clear();
  pipewire_node_id_ = 0U;
  return transition(terminal_state, "portal_session_terminated");
}

CaptureState PortalSelectionStateMachine::state() const { return state_; }
std::string_view PortalSelectionStateMachine::session_handle() const { return session_handle_; }
std::uint32_t PortalSelectionStateMachine::pipewire_node_id() const { return pipewire_node_id_; }
const PortalSelectionContract &PortalSelectionStateMachine::contract() const { return contract_; }

CapturedFrameLease::CapturedFrameLease(std::shared_ptr<const CapturedFrame> frame,
                                       std::function<void()> release)
    : frame_(std::move(frame)), release_(std::move(release)) {}

CapturedFrameLease::~CapturedFrameLease() { reset(); }

CapturedFrameLease::CapturedFrameLease(CapturedFrameLease &&other) noexcept
    : frame_(std::move(other.frame_)), release_(std::move(other.release_)) {}

CapturedFrameLease &CapturedFrameLease::operator=(CapturedFrameLease &&other) noexcept {
  if (this != &other) {
    reset();
    frame_ = std::move(other.frame_);
    release_ = std::move(other.release_);
  }
  return *this;
}

CapturedFrameLease::operator bool() const { return frame_ != nullptr; }

const CapturedFrame &CapturedFrameLease::frame() const {
  if (frame_ == nullptr) {
    throw std::logic_error("captured frame lease is empty");
  }
  return *frame_;
}

void CapturedFrameLease::reset() {
  if (release_) {
    release_();
  }
  release_ = {};
  frame_.reset();
}

struct SharedMemoryCapturePool::Implementation {
  enum class SlotState { free, copying, ready, leased };
  struct Slot {
    SlotState state = SlotState::free;
    std::uint64_t frame_id = 0U;
    std::shared_ptr<CapturedFrame> frame;
  };

  explicit Implementation(std::size_t capacity) : slots(capacity) {}

  mutable std::mutex mutex;
  std::vector<Slot> slots;
  CaptureGeometry geometry;
  std::uint64_t next_frame_id = 1U;
  CapturePoolDiagnostics diagnostics;
};

SharedMemoryCapturePool::SharedMemoryCapturePool(std::size_t capacity) {
  if (capacity == 0U || capacity > 64U) {
    throw std::invalid_argument("capture pool capacity must be from 1 through 64");
  }
  implementation_ = std::make_shared<Implementation>(capacity);
  implementation_->diagnostics.capacity = capacity;
}

SharedMemoryCapturePool::~SharedMemoryCapturePool() = default;
SharedMemoryCapturePool::SharedMemoryCapturePool(SharedMemoryCapturePool &&) noexcept = default;
SharedMemoryCapturePool &
SharedMemoryCapturePool::operator=(SharedMemoryCapturePool &&) noexcept = default;

CaptureIngressResult
SharedMemoryCapturePool::ingest(const SharedMemoryBufferView &buffer,
                                std::uint64_t monotonic_timestamp_ns,
                                const std::function<void()> &release_on_pipewire_loop) {
  CaptureIngressResult result;
  std::string validation_reason;
  const bool valid = valid_buffer(buffer, validation_reason);
  const auto requeue = [&release_on_pipewire_loop]() {
    try {
      release_on_pipewire_loop();
      return true;
    } catch (...) {
      return false;
    }
  };
  if (!valid) {
    result.requeued = requeue();
    std::scoped_lock invalid_lock(implementation_->mutex);
    ++implementation_->diagnostics.rejected;
    if (result.requeued) {
      ++implementation_->diagnostics.requeued;
      result.reason = validation_reason;
    } else {
      ++implementation_->diagnostics.requeue_failures;
      result.reason = "pipewire_requeue_failed";
    }
    return result;
  }

  std::size_t slot_index = 0U;
  std::uint64_t frame_id = 0U;
  CaptureGeometry geometry;
  {
    std::scoped_lock reservation_lock(implementation_->mutex);
    if (!implementation_->diagnostics.admission_open) {
      result.reason = "capture_admission_closed";
    } else {
      if (implementation_->geometry.epoch == 0U ||
          !same_geometry(implementation_->geometry, buffer)) {
        const auto epoch = implementation_->geometry.epoch + 1U;
        implementation_->geometry = make_geometry(epoch, buffer);
        for (auto &slot : implementation_->slots) {
          if (slot.state == Implementation::SlotState::ready) {
            slot = {};
            ++implementation_->diagnostics.dropped_oldest;
          }
        }
      }
      auto available = std::find_if(
          implementation_->slots.begin(), implementation_->slots.end(),
          [](const auto &slot) { return slot.state == Implementation::SlotState::free; });
      if (available == implementation_->slots.end()) {
        for (auto iterator = implementation_->slots.begin();
             iterator != implementation_->slots.end(); ++iterator) {
          if (iterator->state == Implementation::SlotState::ready &&
              (available == implementation_->slots.end() ||
               iterator->frame_id < available->frame_id)) {
            available = iterator;
          }
        }
      }
      if (available == implementation_->slots.end()) {
        ++implementation_->diagnostics.dropped_starved;
        result.reason = "capture_pool_starved";
      } else {
        if (available->state == Implementation::SlotState::ready) {
          ++implementation_->diagnostics.dropped_oldest;
        }
        slot_index = static_cast<std::size_t>(available - implementation_->slots.begin());
        frame_id = implementation_->next_frame_id++;
        geometry = implementation_->geometry;
        available->state = Implementation::SlotState::copying;
        available->frame_id = frame_id;
        available->frame.reset();
      }
    }
  }

  if (frame_id == 0U) {
    result.requeued = requeue();
    std::scoped_lock rejected_lock(implementation_->mutex);
    ++implementation_->diagnostics.rejected;
    if (result.requeued) {
      ++implementation_->diagnostics.requeued;
    } else {
      ++implementation_->diagnostics.requeue_failures;
      result.reason = "pipewire_requeue_failed";
    }
    return result;
  }

  std::shared_ptr<CapturedFrame> frame;
  try {
    CapturedFrame source_frame;
    source_frame.geometry = geometry;
    source_frame.geometry.visible_width = buffer.crop.width;
    source_frame.geometry.visible_height = buffer.crop.height;
    source_frame.pitch = buffer.crop.width * 4U;
    source_frame.pixel_order = buffer.pixel_order;
    source_frame.cursor_mode = buffer.cursor_mode;
    source_frame.pixels.resize(source_frame.pitch * buffer.crop.height);
    for (std::size_t row = 0U; row < buffer.crop.height; ++row) {
      const auto source_offset = (buffer.crop.y + row) * buffer.pitch + buffer.crop.x * 4U;
      std::copy_n(
          buffer.bytes.begin() + static_cast<std::ptrdiff_t>(source_offset), source_frame.pitch,
          source_frame.pixels.begin() + static_cast<std::ptrdiff_t>(row * source_frame.pitch));
    }
    if (buffer.cursor_mode == CursorMode::metadata && buffer.cursor) {
      composite_cursor(source_frame, *buffer.cursor);
    }
    frame = std::make_shared<CapturedFrame>();
    frame->frame_id = frame_id;
    frame->monotonic_timestamp_ns = monotonic_timestamp_ns;
    frame->geometry = geometry;
    frame->pixel_order = buffer.pixel_order;
    frame->cursor_mode = buffer.cursor_mode;
    frame->damage.reserve(buffer.damage.size());
    for (const auto &damage : buffer.damage) {
      frame->damage.push_back(
          transform_damage(damage, buffer.crop.width, buffer.crop.height, buffer.orientation));
    }
    if (buffer.cursor_mode == CursorMode::metadata && buffer.cursor) {
      frame->cursor_position =
          transform_cursor_position(*buffer.cursor, buffer.crop, buffer.orientation);
    }
    normalize_orientation(source_frame, buffer.orientation, *frame);
  } catch (...) {
  }
  result.requeued = requeue();

  std::scoped_lock completion_lock(implementation_->mutex);
  auto &slot = implementation_->slots[slot_index];
  if (result.requeued) {
    ++implementation_->diagnostics.requeued;
  } else {
    ++implementation_->diagnostics.requeue_failures;
  }
  if (slot.state != Implementation::SlotState::copying || slot.frame_id != frame_id) {
    ++implementation_->diagnostics.rejected;
    result.reason = "capture_slot_ownership_lost";
    return result;
  }
  if (!result.requeued || frame == nullptr || !implementation_->diagnostics.admission_open ||
      implementation_->geometry.epoch != geometry.epoch) {
    slot = {};
    ++implementation_->diagnostics.rejected;
    result.reason = !result.requeued                               ? "pipewire_requeue_failed"
                    : frame == nullptr                             ? "capture_frame_copy_failed"
                    : !implementation_->diagnostics.admission_open ? "capture_admission_closed"
                    : implementation_->geometry.epoch != geometry.epoch
                        ? "capture_geometry_superseded"
                        : "capture_frame_rejected";
    return result;
  }
  slot.state = Implementation::SlotState::ready;
  slot.frame = std::move(frame);
  ++implementation_->diagnostics.accepted;
  result.accepted = true;
  result.reason = "capture_frame_accepted";
  result.frame_id = frame_id;
  result.geometry_epoch = geometry.epoch;
  return result;
}

std::optional<CapturedFrameLease> SharedMemoryCapturePool::take_latest() {
  std::scoped_lock lock(implementation_->mutex);
  auto selected = implementation_->slots.end();
  for (auto iterator = implementation_->slots.begin(); iterator != implementation_->slots.end();
       ++iterator) {
    if (iterator->state == Implementation::SlotState::ready &&
        (selected == implementation_->slots.end() || iterator->frame_id > selected->frame_id)) {
      selected = iterator;
    }
  }
  if (selected == implementation_->slots.end()) {
    return std::nullopt;
  }
  for (auto &slot : implementation_->slots) {
    if (&slot != &*selected && slot.state == Implementation::SlotState::ready) {
      slot = {};
      ++implementation_->diagnostics.dropped_oldest;
    }
  }
  selected->state = Implementation::SlotState::leased;
  const auto index = static_cast<std::size_t>(selected - implementation_->slots.begin());
  const auto frame_id = selected->frame_id;
  std::weak_ptr<Implementation> weak = implementation_;
  return CapturedFrameLease(selected->frame, [weak, index, frame_id]() {
    if (const auto implementation = weak.lock()) {
      std::scoped_lock release_lock(implementation->mutex);
      auto &slot = implementation->slots[index];
      if (slot.state == Implementation::SlotState::leased && slot.frame_id == frame_id) {
        slot = {};
      }
    }
  });
}

void SharedMemoryCapturePool::stop(CaptureState terminal_state) {
  if (!terminal(terminal_state)) {
    throw std::invalid_argument("capture stop requires a terminal state");
  }
  std::scoped_lock lock(implementation_->mutex);
  if (!implementation_->diagnostics.admission_open) {
    return;
  }
  implementation_->diagnostics.admission_open = false;
  implementation_->diagnostics.terminal_state = terminal_state;
  for (auto &slot : implementation_->slots) {
    if (slot.state == Implementation::SlotState::ready) {
      slot = {};
      ++implementation_->diagnostics.dropped_oldest;
    }
  }
}

CapturePoolDiagnostics SharedMemoryCapturePool::diagnostics() const {
  std::scoped_lock lock(implementation_->mutex);
  auto result = implementation_->diagnostics;
  for (const auto &slot : implementation_->slots) {
    result.ready += slot.state == Implementation::SlotState::ready ? 1U : 0U;
    result.leased += slot.state == Implementation::SlotState::leased ? 1U : 0U;
  }
  return result;
}

std::string_view packed_pixel_order_name(PackedPixelOrder order) {
  return order == PackedPixelOrder::bgra ? "bgra" : "rgba";
}

std::string_view cursor_mode_name(CursorMode mode) {
  switch (mode) {
  case CursorMode::hidden:
    return "hidden";
  case CursorMode::embedded:
    return "embedded";
  case CursorMode::metadata:
    return "metadata";
  }
  return "unknown";
}

std::string_view capture_state_name(CaptureState state) {
  switch (state) {
  case CaptureState::idle:
    return "idle";
  case CaptureState::creating_session:
    return "creating_session";
  case CaptureState::selecting_sources:
    return "selecting_sources";
  case CaptureState::starting:
    return "starting";
  case CaptureState::streaming:
    return "streaming";
  case CaptureState::cancelled:
    return "cancelled";
  case CaptureState::closed:
    return "closed";
  case CaptureState::revoked:
    return "revoked";
  case CaptureState::disconnected:
    return "disconnected";
  }
  return "unknown";
}

std::string_view capture_orientation_name(CaptureOrientation orientation) {
  switch (orientation) {
  case CaptureOrientation::upright:
    return "upright";
  case CaptureOrientation::rotate90:
    return "rotate90";
  case CaptureOrientation::rotate180:
    return "rotate180";
  case CaptureOrientation::rotate270:
    return "rotate270";
  case CaptureOrientation::flipped:
    return "flipped";
  case CaptureOrientation::flipped90:
    return "flipped90";
  case CaptureOrientation::flipped180:
    return "flipped180";
  case CaptureOrientation::flipped270:
    return "flipped270";
  }
  return "unknown";
}

} // namespace glyphrelay
