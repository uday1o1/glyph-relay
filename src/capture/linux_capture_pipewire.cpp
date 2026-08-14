#include "glyphrelay/linux_capture.hpp"
#include "linux_capture_internal.hpp"

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace glyphrelay {
namespace {

bool terminal_state(CaptureState state) {
  return state == CaptureState::cancelled || state == CaptureState::closed ||
         state == CaptureState::revoked || state == CaptureState::disconnected;
}

std::uint64_t monotonic_raw_nanoseconds() {
  timespec timestamp{};
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp) != 0) {
    return 0U;
  }
  return static_cast<std::uint64_t>(timestamp.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(timestamp.tv_nsec);
}

std::optional<PackedPixelOrder> pixel_order(std::uint32_t format) {
  if (format == SPA_VIDEO_FORMAT_BGRA || format == SPA_VIDEO_FORMAT_BGRx) {
    return PackedPixelOrder::bgra;
  }
  if (format == SPA_VIDEO_FORMAT_RGBA || format == SPA_VIDEO_FORMAT_RGBx) {
    return PackedPixelOrder::rgba;
  }
  return std::nullopt;
}

std::optional<CaptureOrientation> capture_orientation(std::uint32_t transform) {
  switch (transform) {
  case SPA_META_TRANSFORMATION_None:
    return CaptureOrientation::upright;
  case SPA_META_TRANSFORMATION_90:
    return CaptureOrientation::rotate90;
  case SPA_META_TRANSFORMATION_180:
    return CaptureOrientation::rotate180;
  case SPA_META_TRANSFORMATION_270:
    return CaptureOrientation::rotate270;
  case SPA_META_TRANSFORMATION_Flipped:
    return CaptureOrientation::flipped;
  case SPA_META_TRANSFORMATION_Flipped90:
    return CaptureOrientation::flipped90;
  case SPA_META_TRANSFORMATION_Flipped180:
    return CaptureOrientation::flipped180;
  case SPA_META_TRANSFORMATION_Flipped270:
    return CaptureOrientation::flipped270;
  default:
    return std::nullopt;
  }
}

const spa_pod *build_raw_video_format(spa_pod_builder &builder, std::uint32_t video_format) {
  spa_pod_frame frame{};
  const bool push_failed = spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format,
                                                       SPA_PARAM_EnumFormat) < 0;
  const bool property_failed =
      !push_failed && (spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0U) < 0 ||
                       spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_video) < 0 ||
                       spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0U) < 0 ||
                       spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw) < 0 ||
                       spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_format, 0U) < 0 ||
                       spa_pod_builder_id(&builder, video_format) < 0);
  const auto *result = static_cast<const spa_pod *>(spa_pod_builder_pop(&builder, &frame));
  return push_failed || property_failed ? nullptr : result;
}

const spa_meta *find_meta(const spa_buffer &buffer, std::uint32_t type) {
  for (std::uint32_t index = 0U; index < buffer.n_metas; ++index) {
    if (buffer.metas[index].type == type) {
      return &buffer.metas[index];
    }
  }
  return nullptr;
}

std::optional<CaptureCrop> capture_crop(const spa_buffer &buffer, std::size_t width,
                                        std::size_t height) {
  const auto *meta = find_meta(buffer, SPA_META_VideoCrop);
  if (meta == nullptr) {
    return CaptureCrop{0U, 0U, width, height};
  }
  if (meta->data == nullptr || meta->size < sizeof(spa_meta_region)) {
    return std::nullopt;
  }
  const auto &region = *static_cast<const spa_meta_region *>(meta->data);
  if (region.region.position.x < 0 || region.region.position.y < 0) {
    return std::nullopt;
  }
  return CaptureCrop{static_cast<std::size_t>(region.region.position.x),
                     static_cast<std::size_t>(region.region.position.y),
                     static_cast<std::size_t>(region.region.size.width),
                     static_cast<std::size_t>(region.region.size.height)};
}

std::optional<CaptureOrientation> capture_transform(const spa_buffer &buffer) {
  const auto *meta = find_meta(buffer, SPA_META_VideoTransform);
  if (meta == nullptr) {
    return CaptureOrientation::upright;
  }
  if (meta->data == nullptr || meta->size < sizeof(spa_meta_videotransform)) {
    return std::nullopt;
  }
  const auto &transform = *static_cast<const spa_meta_videotransform *>(meta->data);
  return capture_orientation(transform.transform);
}

std::vector<DamageRectangle> capture_damage(const spa_buffer &buffer, const CaptureCrop &crop) {
  std::vector<DamageRectangle> result;
  const auto *meta = find_meta(buffer, SPA_META_VideoDamage);
  if (meta == nullptr || meta->data == nullptr) {
    return result;
  }
  const auto count = static_cast<std::size_t>(meta->size) / sizeof(spa_meta_region);
  const auto *regions = static_cast<const spa_meta_region *>(meta->data);
  const auto crop_right = crop.x + crop.width;
  const auto crop_bottom = crop.y + crop.height;
  for (std::size_t index = 0U; index < count; ++index) {
    if (!spa_meta_region_is_valid(&regions[index])) {
      break;
    }
    if (result.size() == 64U) {
      throw std::runtime_error("pipewire_damage_count_exceeded");
    }
    const auto &region = regions[index].region;
    if (region.position.x < 0 || region.position.y < 0) {
      continue;
    }
    const auto left = static_cast<std::size_t>(region.position.x);
    const auto top = static_cast<std::size_t>(region.position.y);
    const auto right = left + static_cast<std::size_t>(region.size.width);
    const auto bottom = top + static_cast<std::size_t>(region.size.height);
    const auto clipped_left = std::max(left, crop.x);
    const auto clipped_top = std::max(top, crop.y);
    const auto clipped_right = std::min(right, crop_right);
    const auto clipped_bottom = std::min(bottom, crop_bottom);
    if (clipped_left < clipped_right && clipped_top < clipped_bottom) {
      result.push_back({clipped_left - crop.x, clipped_top - crop.y, clipped_right - clipped_left,
                        clipped_bottom - clipped_top});
    }
  }
  return result;
}

struct OwnedCursor {
  std::vector<std::uint8_t> rgba;
  CursorMetadataView view;
};

std::optional<OwnedCursor> capture_cursor(const spa_buffer &buffer) {
  const auto *meta = find_meta(buffer, SPA_META_Cursor);
  if (meta == nullptr) {
    return std::nullopt;
  }
  if (meta->data == nullptr || meta->size < sizeof(spa_meta_cursor)) {
    throw std::runtime_error("pipewire_cursor_meta_invalid");
  }
  const auto *cursor = static_cast<const spa_meta_cursor *>(meta->data);
  if (!spa_meta_cursor_is_valid(cursor) || cursor->bitmap_offset == 0U) {
    return std::nullopt;
  }
  if (cursor->bitmap_offset < sizeof(spa_meta_cursor) || cursor->bitmap_offset > meta->size ||
      sizeof(spa_meta_bitmap) > meta->size - cursor->bitmap_offset) {
    throw std::runtime_error("pipewire_cursor_bitmap_offset_invalid");
  }
  const auto *bitmap = SPA_MEMBER(cursor, cursor->bitmap_offset, const spa_meta_bitmap);
  const auto bitmap_offset = static_cast<std::size_t>(cursor->bitmap_offset) + bitmap->offset;
  if (!spa_meta_bitmap_is_valid(bitmap) || bitmap->offset < sizeof(spa_meta_bitmap) ||
      bitmap_offset > meta->size || bitmap->stride <= 0 || bitmap->size.width == 0U ||
      bitmap->size.height == 0U) {
    throw std::runtime_error("pipewire_cursor_bitmap_invalid");
  }
  const auto order = pixel_order(bitmap->format);
  if (!order) {
    throw std::runtime_error("pipewire_cursor_format_unsupported");
  }
  const auto width = static_cast<std::size_t>(bitmap->size.width);
  const auto height = static_cast<std::size_t>(bitmap->size.height);
  const auto stride = static_cast<std::size_t>(bitmap->stride);
  if (width > std::numeric_limits<std::size_t>::max() / 4U || stride < width * 4U ||
      height - 1U > std::numeric_limits<std::size_t>::max() / stride ||
      (height - 1U) * stride + width * 4U > meta->size - bitmap_offset) {
    throw std::runtime_error("pipewire_cursor_bitmap_bounds_invalid");
  }
  const auto *source = SPA_MEMBER(bitmap, bitmap->offset, const std::uint8_t);
  OwnedCursor output;
  output.rgba.resize(width * height * 4U);
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      const auto source_offset = y * stride + x * 4U;
      const auto output_offset = (y * width + x) * 4U;
      output.rgba[output_offset] =
          (*order == PackedPixelOrder::rgba) ? source[source_offset] : source[source_offset + 2U];
      output.rgba[output_offset + 1U] = source[source_offset + 1U];
      output.rgba[output_offset + 2U] =
          (*order == PackedPixelOrder::rgba) ? source[source_offset + 2U] : source[source_offset];
      output.rgba[output_offset + 3U] = source[source_offset + 3U];
    }
  }
  output.view = {.x = cursor->position.x,
                 .y = cursor->position.y,
                 .hotspot_x = cursor->hotspot.x,
                 .hotspot_y = cursor->hotspot.y,
                 .width = width,
                 .height = height,
                 .pitch = width * 4U,
                 .rgba = output.rgba};
  return output;
}

} // namespace

pw_stream_flags detail::capture_stream_flags() {
  return static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                      PW_STREAM_FLAG_DONT_RECONNECT);
}

CaptureIngressResult
detail::ingest_pipewire_shm_buffer(pw_buffer &pipewire_buffer, const spa_video_info_raw &video_info,
                                   CursorMode cursor_mode, SharedMemoryCapturePool &capture_pool,
                                   std::uint64_t monotonic_timestamp_ns,
                                   const std::function<void()> &release_on_pipewire_loop) {
  const auto reject = [&release_on_pipewire_loop](std::string reason) {
    CaptureIngressResult result;
    try {
      release_on_pipewire_loop();
      result.requeued = true;
      result.reason = std::move(reason);
    } catch (...) {
      result.reason = "pipewire_requeue_failed";
    }
    return result;
  };
  if (pipewire_buffer.buffer == nullptr) {
    return {false, false, "pipewire_buffer_missing", 0U, 0U};
  }
  auto &buffer = *pipewire_buffer.buffer;
  if (buffer.n_datas == 0U || buffer.datas == nullptr) {
    return reject("pipewire_shm_buffer_invalid");
  }
  auto &data = buffer.datas[0];
  if (data.type == SPA_DATA_DmaBuf) {
    return reject("pipewire_dmabuf_requires_fallback");
  }
  if (data.data == nullptr || data.chunk == nullptr || data.chunk->stride <= 0 ||
      data.chunk->offset > data.maxsize || data.chunk->size > data.maxsize - data.chunk->offset) {
    return reject("pipewire_shm_buffer_invalid");
  }
  const auto order = pixel_order(video_info.format);
  const auto crop = capture_crop(buffer, video_info.size.width, video_info.size.height);
  const auto orientation = capture_transform(buffer);
  if (!order || !crop || !orientation) {
    return reject(!orientation ? "pipewire_transform_unsupported" : "pipewire_metadata_invalid");
  }
  std::optional<OwnedCursor> cursor;
  std::vector<DamageRectangle> damage;
  try {
    if (cursor_mode == CursorMode::metadata) {
      cursor = capture_cursor(buffer);
    }
    damage = capture_damage(buffer, *crop);
  } catch (const std::exception &exception) {
    return reject(exception.what());
  }
  const auto *bytes = static_cast<const std::uint8_t *>(data.data) + data.chunk->offset;
  SharedMemoryBufferView view = {
      .bytes = std::span<const std::uint8_t>(bytes, data.chunk->size),
      .width = video_info.size.width,
      .height = video_info.size.height,
      .pitch = static_cast<std::size_t>(data.chunk->stride),
      .crop = *crop,
      .pixel_order = *order,
      .orientation = *orientation,
      .cursor_mode = cursor_mode,
      .cursor = cursor ? std::optional<CursorMetadataView>(cursor->view) : std::nullopt,
      .damage = damage,
  };
  return capture_pool.ingest(view, monotonic_timestamp_ns, release_on_pipewire_loop);
}

struct LinuxPipeWireCapture::Implementation {
  pw_thread_loop *thread_loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_stream *stream = nullptr;
  spa_hook listener{};
  SharedMemoryCapturePool *capture_pool = nullptr;
  CaptureEventCallback event_callback;
  CursorMode cursor_mode = CursorMode::hidden;
  spa_video_info_raw video_info{};
  std::atomic<bool> format_ready = false;
  std::atomic<bool> active = false;
  std::atomic<bool> stopping = false;
  bool loop_started = false;
  std::mutex stop_mutex;

  ~Implementation() { destroy(); }

  void emit(std::string reason) {
    if (event_callback) {
      try {
        event_callback(std::move(reason));
      } catch (...) {
      }
    }
  }

  void fail(std::string reason) {
    active = false;
    if (!stopping && capture_pool != nullptr) {
      capture_pool->stop(CaptureState::disconnected);
    }
    emit(std::move(reason));
  }

  static void state_changed(void *data, pw_stream_state, pw_stream_state state, const char *) {
    auto &self = *static_cast<Implementation *>(data);
    if (state == PW_STREAM_STATE_STREAMING) {
      self.active = true;
      self.emit("pipewire_streaming");
    } else if (state == PW_STREAM_STATE_ERROR) {
      self.fail("pipewire_stream_error");
    } else if (state == PW_STREAM_STATE_UNCONNECTED && !self.stopping) {
      self.fail("pipewire_stream_disconnected");
    }
  }

  static void parameter_changed(void *data, std::uint32_t identifier, const spa_pod *parameter) {
    auto &self = *static_cast<Implementation *>(data);
    if (parameter == nullptr || identifier != SPA_PARAM_Format) {
      return;
    }
    spa_video_info_raw parsed{};
    if (spa_format_video_raw_parse(parameter, &parsed) < 0 || parsed.size.width == 0U ||
        parsed.size.height == 0U || !pixel_order(parsed.format)) {
      self.format_ready = false;
      self.fail("pipewire_video_format_unsupported");
      return;
    }
    self.video_info = parsed;
    self.format_ready = true;
    self.emit("pipewire_video_format_negotiated");
  }

  void requeue(pw_buffer *buffer) {
    if (pw_stream_queue_buffer(stream, buffer) < 0) {
      throw std::runtime_error("pipewire_buffer_requeue_failed");
    }
  }

  void process_buffer(pw_buffer *pipewire_buffer) {
    if (pipewire_buffer == nullptr || pipewire_buffer->buffer == nullptr) {
      emit("pipewire_buffer_missing");
      return;
    }
    if (!format_ready || capture_pool == nullptr) {
      requeue(pipewire_buffer);
      emit("pipewire_buffer_before_format");
      return;
    }
    const auto result = detail::ingest_pipewire_shm_buffer(
        *pipewire_buffer, video_info, cursor_mode, *capture_pool, monotonic_raw_nanoseconds(),
        [this, pipewire_buffer]() { requeue(pipewire_buffer); });
    if (!result.accepted) {
      emit(result.reason);
    }
  }

  static void process(void *data) {
    auto &self = *static_cast<Implementation *>(data);
    auto *buffer = pw_stream_dequeue_buffer(self.stream);
    if (buffer == nullptr) {
      self.emit("pipewire_dequeue_empty");
      return;
    }
    try {
      self.process_buffer(buffer);
    } catch (const std::exception &exception) {
      try {
        self.requeue(buffer);
      } catch (...) {
      }
      self.fail(exception.what());
    }
  }

  static const pw_stream_events &events() {
    static const pw_stream_events value = []() {
      pw_stream_events configured{};
      configured.version = PW_VERSION_STREAM_EVENTS;
      configured.state_changed = state_changed;
      configured.param_changed = parameter_changed;
      configured.process = process;
      return configured;
    }();
    return value;
  }

  void destroy() {
    std::scoped_lock lock(stop_mutex);
    stopping = true;
    active = false;
    if (thread_loop != nullptr && loop_started) {
      pw_thread_loop_stop(thread_loop);
      loop_started = false;
    }
    if (stream != nullptr) {
      spa_hook_remove(&listener);
      pw_stream_destroy(stream);
      stream = nullptr;
    }
    if (core != nullptr) {
      pw_core_disconnect(core);
      core = nullptr;
    }
    if (context != nullptr) {
      pw_context_destroy(context);
      context = nullptr;
    }
    if (thread_loop != nullptr) {
      pw_thread_loop_destroy(thread_loop);
      thread_loop = nullptr;
    }
    capture_pool = nullptr;
    event_callback = {};
    format_ready = false;
  }
};

LinuxPipeWireCapture::LinuxPipeWireCapture()
    : implementation_(std::make_unique<Implementation>()) {}

LinuxPipeWireCapture::~LinuxPipeWireCapture() = default;

LinuxPipeWireCapture::LinuxPipeWireCapture(LinuxPipeWireCapture &&) noexcept = default;
LinuxPipeWireCapture &LinuxPipeWireCapture::operator=(LinuxPipeWireCapture &&) noexcept = default;

CaptureOperationResult LinuxPipeWireCapture::start(PortalWindowGrant grant,
                                                   SharedMemoryCapturePool &capture_pool,
                                                   CaptureEventCallback event_callback) {
  auto &implementation = *implementation_;
  if (implementation.thread_loop != nullptr) {
    return {false, false, "pipewire_stream_already_started"};
  }
  if (!grant) {
    return {false, false, "pipewire_portal_grant_invalid"};
  }
  const auto node_id = grant.pipewire_node_id();
  implementation.cursor_mode = grant.cursor_mode();
  int remote_fd = grant.release_pipewire_remote_fd();
  implementation.capture_pool = &capture_pool;
  implementation.event_callback = std::move(event_callback);
  implementation.stopping = false;
  static std::once_flag pipewire_initialized;
  std::call_once(pipewire_initialized, []() { pw_init(nullptr, nullptr); });

  implementation.thread_loop = pw_thread_loop_new("glyphrelay-capture", nullptr);
  if (implementation.thread_loop == nullptr) {
    ::close(remote_fd);
    implementation.destroy();
    return {false, false, "pipewire_thread_loop_create_failed"};
  }
  implementation.context =
      pw_context_new(pw_thread_loop_get_loop(implementation.thread_loop), nullptr, 0U);
  if (implementation.context == nullptr) {
    ::close(remote_fd);
    implementation.destroy();
    return {false, false, "pipewire_context_create_failed"};
  }
  implementation.core = pw_context_connect_fd(implementation.context, remote_fd, nullptr, 0U);
  if (implementation.core == nullptr) {
    implementation.destroy();
    return {false, false, "pipewire_remote_connect_failed"};
  }
  implementation.stream =
      pw_stream_new(implementation.core, "GlyphRelay window capture",
                    pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                      PW_KEY_MEDIA_ROLE, "Screen", nullptr));
  if (implementation.stream == nullptr) {
    implementation.destroy();
    return {false, false, "pipewire_stream_create_failed"};
  }
  pw_stream_add_listener(implementation.stream, &implementation.listener, &Implementation::events(),
                         &implementation);
  std::array<std::uint8_t, 1024U> pod_buffer{};
  spa_pod_builder builder{};
  spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
  const spa_pod *parameters[2] = {build_raw_video_format(builder, SPA_VIDEO_FORMAT_BGRA),
                                  build_raw_video_format(builder, SPA_VIDEO_FORMAT_RGBA)};
  if (parameters[0] == nullptr || parameters[1] == nullptr) {
    implementation.destroy();
    return {false, false, "pipewire_format_builder_failed"};
  }
  const auto flags = detail::capture_stream_flags();
  if (pw_stream_connect(implementation.stream, PW_DIRECTION_INPUT, node_id, flags, parameters, 2U) <
      0) {
    implementation.destroy();
    return {false, false, "pipewire_stream_connect_failed"};
  }
  if (pw_thread_loop_start(implementation.thread_loop) < 0) {
    implementation.destroy();
    return {false, false, "pipewire_thread_loop_start_failed"};
  }
  implementation.loop_started = true;
  return {true, false, "pipewire_stream_started"};
}

CaptureOperationResult LinuxPipeWireCapture::stop(CaptureState terminal) {
  if (!terminal_state(terminal)) {
    throw std::invalid_argument("PipeWire stop requires a terminal capture state");
  }
  auto &implementation = *implementation_;
  if (implementation.capture_pool != nullptr) {
    implementation.capture_pool->stop(terminal);
  }
  implementation.destroy();
  return {true, false, "pipewire_stream_stopped"};
}

bool LinuxPipeWireCapture::running() const {
  return implementation_ != nullptr && implementation_->active;
}

std::string_view linux_capture_backend_version() { return pw_get_library_version(); }

} // namespace glyphrelay
