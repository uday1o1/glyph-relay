#include "glyphrelay/linux_capture.hpp"
#include "linux_capture_internal.hpp"

#include <spa/buffer/meta.h>
#include <spa/param/video/raw.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

std::vector<std::uint8_t> source_pixels() {
  const std::array<std::uint8_t, 6U> red = {1U, 2U, 3U, 4U, 5U, 6U};
  std::vector<std::uint8_t> pixels(red.size() * 4U, 0U);
  for (std::size_t index = 0U; index < red.size(); ++index) {
    pixels[index * 4U + 2U] = red[index];
    pixels[index * 4U + 3U] = 255U;
  }
  return pixels;
}

struct CursorBlob {
  spa_meta_cursor cursor{};
  spa_meta_bitmap bitmap{};
  std::array<std::uint8_t, 4U> pixels{};
};

void test_real_spa_metadata_ingress() {
  auto pixels = source_pixels();
  spa_chunk chunk = {.offset = 0U,
                     .size = static_cast<std::uint32_t>(pixels.size()),
                     .stride = 8,
                     .flags = SPA_CHUNK_FLAG_NONE};
  spa_data data = {.type = SPA_DATA_MemPtr,
                   .flags = SPA_DATA_FLAG_READABLE,
                   .fd = -1,
                   .mapoffset = 0U,
                   .maxsize = static_cast<std::uint32_t>(pixels.size()),
                   .data = pixels.data(),
                   .chunk = &chunk};
  spa_meta_region crop{};
  crop.region.position = {0, 0};
  crop.region.size = {2U, 3U};
  spa_meta_region damage{};
  damage.region.position = {0, 0};
  damage.region.size = {1U, 2U};
  spa_meta_videotransform transform{SPA_META_TRANSFORMATION_Flipped90};
  CursorBlob cursor;
  cursor.cursor.id = 1U;
  cursor.cursor.position = {0, 0};
  cursor.cursor.hotspot = {0, 0};
  cursor.cursor.bitmap_offset = static_cast<std::uint32_t>(offsetof(CursorBlob, bitmap));
  cursor.bitmap.format = SPA_VIDEO_FORMAT_RGBA;
  cursor.bitmap.size = {1U, 1U};
  cursor.bitmap.stride = 4;
  cursor.bitmap.offset =
      static_cast<std::uint32_t>(offsetof(CursorBlob, pixels) - offsetof(CursorBlob, bitmap));
  cursor.pixels = {255U, 0U, 0U, 255U};
  std::array<spa_meta, 4U> metas = {
      spa_meta{SPA_META_VideoCrop, static_cast<std::uint32_t>(sizeof(crop)), &crop},
      spa_meta{SPA_META_VideoDamage, static_cast<std::uint32_t>(sizeof(damage)), &damage},
      spa_meta{SPA_META_VideoTransform, static_cast<std::uint32_t>(sizeof(transform)), &transform},
      spa_meta{SPA_META_Cursor, static_cast<std::uint32_t>(sizeof(cursor)), &cursor},
  };
  spa_buffer buffer = {.n_metas = static_cast<std::uint32_t>(metas.size()),
                       .n_datas = 1U,
                       .metas = metas.data(),
                       .datas = &data};
  pw_buffer pipewire_buffer{};
  pipewire_buffer.buffer = &buffer;
  spa_video_info_raw video_info{};
  video_info.format = SPA_VIDEO_FORMAT_BGRA;
  video_info.size = {2U, 3U};
  glyphrelay::SharedMemoryCapturePool pool(2U);
  std::size_t requeues = 0U;
  const auto result = glyphrelay::detail::ingest_pipewire_shm_buffer(
      pipewire_buffer, video_info, glyphrelay::CursorMode::metadata, pool, 42U,
      [&requeues]() { ++requeues; });
  require(result.accepted && result.requeued && requeues == 1U,
          "real SPA shared-memory metadata must copy and requeue exactly once");
  auto lease = pool.take_latest();
  require(lease && lease->frame().monotonic_timestamp_ns == 42U &&
              lease->frame().geometry.visible_width == 3U &&
              lease->frame().geometry.visible_height == 2U &&
              lease->frame().geometry.source_orientation ==
                  glyphrelay::CaptureOrientation::flipped90 &&
              lease->frame().cursor_position ==
                  std::optional<std::pair<std::int32_t, std::int32_t>>{{0, 0}} &&
              lease->frame().pixels[2U] == 255U && lease->frame().damage.size() == 1U &&
              lease->frame().damage[0].x == 0U && lease->frame().damage[0].y == 0U &&
              lease->frame().damage[0].width == 2U && lease->frame().damage[0].height == 1U,
          "SPA crop, reflected orientation, cursor, and damage must share one transform");
  lease->reset();

  data.type = SPA_DATA_DmaBuf;
  const auto dma_buf = glyphrelay::detail::ingest_pipewire_shm_buffer(
      pipewire_buffer, video_info, glyphrelay::CursorMode::hidden, pool, 43U,
      [&requeues]() { ++requeues; });
  require(!dma_buf.accepted && dma_buf.requeued && requeues == 2U &&
              dma_buf.reason == "pipewire_dmabuf_requires_fallback",
          "mandatory SHM ingress must reject DMA-BUF with an explicit fallback reason");
}

} // namespace

int main() {
  require(glyphrelay::linux_capture_backend_available(),
          "Linux build must include the GDBus and PipeWire capture backend");
  require(!glyphrelay::linux_capture_backend_version().empty() &&
              glyphrelay::linux_capture_backend_version() != "unavailable",
          "Linux capture backend must expose its linked PipeWire version");
  const auto stream_flags = glyphrelay::detail::capture_stream_flags();
  require((stream_flags & PW_STREAM_FLAG_MAP_BUFFERS) != 0 &&
              (stream_flags & PW_STREAM_FLAG_DONT_RECONNECT) != 0 &&
              (stream_flags & PW_STREAM_FLAG_RT_PROCESS) == 0,
          "copying capture callbacks must map buffers, fail closed, and stay off the RT thread");
  test_real_spa_metadata_ingress();

  glyphrelay::SharedMemoryCapturePool pool(2U);
  glyphrelay::LinuxPipeWireCapture pipewire;
  const auto invalid_grant = pipewire.start({}, pool);
  require(!invalid_grant.passed && invalid_grant.reason == "pipewire_portal_grant_invalid" &&
              !pipewire.running(),
          "PipeWire capture must reject a stream not authorized by a portal grant");
  bool invalid_terminal_rejected = false;
  try {
    static_cast<void>(pipewire.stop(glyphrelay::CaptureState::streaming));
  } catch (const std::invalid_argument &) {
    invalid_terminal_rejected = true;
  }
  require(invalid_terminal_rejected,
          "PipeWire teardown must accept only an explicit terminal capture state");

  glyphrelay::LinuxPortalClient portal;
  const auto result = portal.open_window({}, 100U);
  require(!result.passed && !result.grant,
          "a headless build container must fail closed without fabricating a portal grant");
  require(result.reason == "portal_session_bus_unavailable" ||
              result.reason == "portal_capability_query_failed" ||
              result.reason == "portal_request_call_failed",
          "headless portal failure must use one bounded infrastructure reason");
  require(portal.close().passed && !portal.poll_terminal(),
          "closing an unopened portal client must be idempotent and resource-safe");
  return 0;
}
