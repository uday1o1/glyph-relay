#include "glyphrelay/preprocess_pool.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

void complete(glyphrelay::PreprocessOwnershipRing &ring,
              const glyphrelay::PreprocessSlotToken &token) {
  require(ring.source_upload_complete(token).passed,
          "host-to-device completion must advance source ownership");
  require(ring.map_copy_pending(token).passed,
          "surface conversion must advance to map-copy ownership");
  require(ring.source_read_complete(token).passed,
          "the source-read event must independently release the packed source");
  require(ring.ready_to_submit(token).passed,
          "the map-copy event must make the surface submit-ready");
  require(ring.submitted(token).passed, "submission must retain the NV12 surface");
  require(ring.encoder_input_released(token).passed,
          "the encoder release must advance without freeing early");
  require(ring.release_surface(token).passed,
          "only the final encoder release may return the surface to free");
}

void test_separate_bounded_ownership_cycles() {
  glyphrelay::PreprocessOwnershipRing ring(2U, 2U);
  const auto reserved = ring.reserve(11U, 3U, {0x1000U, 0x400U}, {0x8000U, 0x1000U});
  require(reserved.passed &&
              ring.source_state(reserved.token.source_slot) ==
                  glyphrelay::DeviceSourceState::host_to_device_pending &&
              ring.surface_state(reserved.token.surface_slot) ==
                  glyphrelay::EncoderSurfaceState::cuda_writing,
          "reservation must occupy distinct bounded source and surface slots");
  require(ring.source_upload_complete(reserved.token).passed &&
              ring.map_copy_pending(reserved.token).passed &&
              ring.source_read_complete(reserved.token).passed,
          "source-read completion must release only the packed source");
  const auto independent = ring.diagnostics();
  require(independent.active_sources == 0U && independent.active_surfaces == 1U &&
              ring.surface_state(reserved.token.surface_slot) ==
                  glyphrelay::EncoderSurfaceState::map_copy_pending,
          "source reuse must be independent of the NVENC surface lifetime");
  require(!ring.release_surface(reserved.token).passed,
          "a surface may not skip map-ready, submit, and encoder-release ownership");
  require(ring.ready_to_submit(reserved.token).passed && ring.submitted(reserved.token).passed &&
              ring.encoder_input_released(reserved.token).passed &&
              ring.release_surface(reserved.token).passed && ring.all_free(),
          "the declared encoder-surface cycle must return every slot to free");
}

void test_alias_stale_token_exhaustion_and_shutdown() {
  glyphrelay::PreprocessOwnershipRing ring(2U, 2U);
  require(glyphrelay::device_ranges_overlap({0x1000U, 0x100U}, {0x1080U, 0x100U}) &&
              !glyphrelay::device_ranges_overlap({0x1000U, 0x100U}, {0x1100U, 0x100U}),
          "device allocation overlap must use half-open address ranges");
  require(!ring.reserve(1U, 1U, {0x1000U, 0x400U}, {0x1200U, 0x800U}).passed,
          "packed-RGB and NV12 allocations may never alias");

  const auto first = ring.reserve(1U, 1U, {0x1000U, 0x400U}, {0x8000U, 0x1000U});
  const auto second = ring.reserve(2U, 1U, {0x2000U, 0x400U}, {0xA000U, 0x1000U});
  require(first.passed && second.passed &&
              !ring.reserve(3U, 1U, {0x3000U, 0x400U}, {0xC000U, 0x1000U}).passed,
          "the pool must reject a third in-flight frame instead of growing or blocking");
  complete(ring, first.token);
  const auto reused = ring.reserve(3U, 2U, {0x1000U, 0x400U}, {0x8000U, 0x1000U});
  require(reused.passed && !ring.source_upload_complete(first.token).passed,
          "a released slot must reject a stale frame and geometry token after reuse");
  complete(ring, second.token);
  complete(ring, reused.token);

  const auto cross_alias = ring.reserve(4U, 2U, {0x8000U, 0x400U}, {0xC000U, 0x1000U});
  require(!cross_alias.passed && cross_alias.reason == "preprocess_source_surface_alias",
          "a future packed source may not reuse any registered NV12 surface allocation");
  ring.close_admission();
  require(!ring.reserve(5U, 2U, {0x1000U, 0x400U}, {0x8000U, 0x1000U}).passed && ring.all_free() &&
              !ring.diagnostics().admission_open && ring.diagnostics().reservations == 3U &&
              ring.diagnostics().rejected == 4U,
          "closed admission must preserve bounded diagnostics and fully drained resources");
}

void test_capacity_validation() {
  bool zero_rejected = false;
  try {
    glyphrelay::PreprocessOwnershipRing invalid(0U, 2U);
  } catch (const std::invalid_argument &) {
    zero_rejected = true;
  }
  bool oversized_rejected = false;
  try {
    glyphrelay::PreprocessOwnershipRing invalid(65U, 2U);
  } catch (const std::invalid_argument &) {
    oversized_rejected = true;
  }
  require(zero_rejected && oversized_rejected,
          "zero or oversized preprocessing pools must fail before allocating slots");
}

void test_abort_releases_only_matching_owners() {
  glyphrelay::PreprocessOwnershipRing ring(2U, 2U);
  const auto first = ring.reserve(1U, 1U, {0x1000U, 0x400U}, {0x8000U, 0x1000U});
  const auto second = ring.reserve(2U, 1U, {0x2000U, 0x400U}, {0xA000U, 0x1000U});
  require(first.passed && second.passed && ring.abort(first.token).passed,
          "fatal preprocessing cleanup must release the matching source and surface");
  auto stale = first.token;
  ++stale.frame_id;
  require(!ring.abort(stale).passed && ring.diagnostics().active_sources == 1U &&
              ring.diagnostics().active_surfaces == 1U,
          "a stale abort token may not release another frame's allocations");
  require(ring.abort(second.token).passed && ring.all_free(),
          "aborting every matching owner must drain the bounded rings");
}

} // namespace

int main() {
  test_separate_bounded_ownership_cycles();
  test_alias_stale_token_exhaustion_and_shutdown();
  test_capacity_validation();
  test_abort_releases_only_matching_owners();
  return 0;
}
