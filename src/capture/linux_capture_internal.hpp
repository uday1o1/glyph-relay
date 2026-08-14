#pragma once

#include "glyphrelay/capture.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/raw.h>

#include <cstdint>
#include <functional>

namespace glyphrelay::detail {

pw_stream_flags capture_stream_flags();

CaptureIngressResult
ingest_pipewire_shm_buffer(pw_buffer &pipewire_buffer, const spa_video_info_raw &video_info,
                           CursorMode cursor_mode, SharedMemoryCapturePool &capture_pool,
                           std::uint64_t monotonic_timestamp_ns,
                           const std::function<void()> &release_on_pipewire_loop);

} // namespace glyphrelay::detail
