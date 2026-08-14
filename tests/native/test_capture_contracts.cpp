#include "glyphrelay/capture.hpp"
#include "glyphrelay/color_conversion.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
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

glyphrelay::SharedMemoryBufferView view(std::vector<std::uint8_t> &pixels, std::size_t width,
                                        std::size_t height, std::size_t pitch) {
  return {.bytes = pixels,
          .width = width,
          .height = height,
          .pitch = pitch,
          .crop = {0U, 0U, width, height},
          .pixel_order = glyphrelay::PackedPixelOrder::bgra};
}

glyphrelay::CapturedFrame frame(std::size_t width, std::size_t height,
                                glyphrelay::PackedPixelOrder order, std::size_t pitch,
                                std::vector<std::uint8_t> pixels) {
  glyphrelay::CapturedFrame result;
  result.frame_id = 1U;
  result.geometry = {1U,
                     width,
                     height,
                     {0U, 0U, width, height},
                     width,
                     height,
                     (width + 1U) & ~std::size_t{1U},
                     (height + 1U) & ~std::size_t{1U}};
  result.pixel_order = order;
  result.pitch = pitch;
  result.pixels = std::move(pixels);
  return result;
}

std::vector<std::uint8_t> uniform(std::size_t width, std::size_t height, std::size_t pitch,
                                  glyphrelay::PackedPixelOrder order, std::uint8_t red,
                                  std::uint8_t green, std::uint8_t blue) {
  std::vector<std::uint8_t> pixels(pitch * height, 0xCDU);
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      const auto offset = y * pitch + x * 4U;
      if (order == glyphrelay::PackedPixelOrder::rgba) {
        pixels[offset] = red;
        pixels[offset + 1U] = green;
        pixels[offset + 2U] = blue;
      } else {
        pixels[offset] = blue;
        pixels[offset + 1U] = green;
        pixels[offset + 2U] = red;
      }
      pixels[offset + 3U] = 255U;
    }
  }
  return pixels;
}

void test_portal_contract_and_lifecycle() {
  glyphrelay::PortalSelectionStateMachine portal;
  const auto &contract = portal.contract();
  require(contract.window_sources_only && !contract.multiple && contract.persist_mode == 0U,
          "portal contract must authorize one nonpersistent window only");
  require(contract.cursor_preference ==
              std::vector<glyphrelay::CursorMode>{glyphrelay::CursorMode::metadata,
                                                  glyphrelay::CursorMode::embedded,
                                                  glyphrelay::CursorMode::hidden},
          "portal cursor preference must be metadata, embedded, then hidden");
  require(portal.begin("/request/create").passed,
          "portal selection must begin through a request handle");
  require(!portal.session_created("/request/spoofed", "/session/one", "/request/select").passed,
          "a stale portal request handle must fail closed");
  require(portal.session_created("/request/create", "/session/one", "/request/select").passed,
          "a matching create response must advance selection");
  require(portal.sources_selected("/request/select", "/request/start").passed,
          "the selected window must advance to portal start");
  require(portal.started("/request/start", 73U).passed &&
              portal.state() == glyphrelay::CaptureState::streaming &&
              portal.pipewire_node_id() == 73U,
          "a valid portal start response must expose its PipeWire node");
  require(!portal.cancel("/request/start").passed,
          "a streaming session cannot be rewritten as a selection cancellation");
  require(portal.close(glyphrelay::CaptureState::revoked).passed &&
              portal.session_handle().empty() && portal.pipewire_node_id() == 0U,
          "permission revocation must clear every portal handle");

  glyphrelay::PortalSelectionStateMachine cancelled;
  require(cancelled.begin("/request/create-cancel").passed &&
              cancelled
                  .session_created("/request/create-cancel", "/session/cancel",
                                   "/request/select-cancel")
                  .passed &&
              cancelled.cancel("/request/select-cancel").passed &&
              cancelled.state() == glyphrelay::CaptureState::cancelled &&
              cancelled.session_handle().empty() && cancelled.pipewire_node_id() == 0U,
          "the matching portal cancellation must clear all granted session authority");
  glyphrelay::PortalSelectionStateMachine invalid_close;
  require(invalid_close.begin("/request/close").passed &&
              !invalid_close.close(glyphrelay::CaptureState::cancelled).passed,
          "generic teardown must not bypass request-fenced portal cancellation");
}

void test_prompt_copy_requeue_latest_frame_and_bounds() {
  glyphrelay::SharedMemoryCapturePool pool(2U);
  auto pixels = uniform(2U, 2U, 12U, glyphrelay::PackedPixelOrder::bgra, 9U, 8U, 7U);
  auto buffer = view(pixels, 2U, 2U, 12U);
  bool requeued = false;
  const auto first = pool.ingest(buffer, 100U, [&]() {
    requeued = true;
    std::fill(pixels.begin(), pixels.end(), 0U);
  });
  require(first.accepted && first.requeued && requeued,
          "SHM ingress must copy and promptly requeue the PipeWire buffer");
  auto first_lease = pool.take_latest();
  require(first_lease && first_lease->frame().pixels[0] == 7U &&
              first_lease->frame().pixels[2] == 9U,
          "owned capture bytes must remain valid after PipeWire requeue");

  auto second_pixels = uniform(2U, 2U, 8U, glyphrelay::PackedPixelOrder::bgra, 20U, 0U, 0U);
  auto third_pixels = uniform(2U, 2U, 8U, glyphrelay::PackedPixelOrder::bgra, 30U, 0U, 0U);
  auto second_view = view(second_pixels, 2U, 2U, 8U);
  auto third_view = view(third_pixels, 2U, 2U, 8U);
  require(pool.ingest(second_view, 200U, []() {}).accepted,
          "one free capture slot must accept a second frame");
  auto second_lease = pool.take_latest();
  require(second_lease && second_lease->frame().monotonic_timestamp_ns == 200U,
          "a second consumer lease must occupy the remaining bounded slot");
  require(!pool.ingest(third_view, 300U, []() {}).accepted,
          "all-leased capture slots must drop rather than grow or block");
  require(pool.diagnostics().dropped_starved == 1U,
          "capture pool starvation must have a bounded diagnostic");
  first_lease->reset();
  require(pool.ingest(third_view, 300U, []() {}).accepted,
          "releasing a lease must return its bounded slot");
  auto latest = pool.take_latest();
  require(latest && latest->frame().monotonic_timestamp_ns == 300U,
          "capture consumption must select the freshest ready frame");
  second_lease->reset();
  const auto diagnostics = pool.diagnostics();
  require(diagnostics.capacity == 2U && diagnostics.leased == 1U && diagnostics.ready == 0U,
          "capture diagnostics must expose every bounded slot state");
  latest->reset();

  pool.stop(glyphrelay::CaptureState::disconnected);
  bool closed_requeue = false;
  const auto closed = pool.ingest(second_view, 400U, [&]() { closed_requeue = true; });
  require(!closed.accepted && closed.requeued && closed_requeue &&
              closed.reason == "capture_admission_closed",
          "closed capture admission must still promptly return a PipeWire buffer");
}

void test_geometry_epoch_cursor_and_validation() {
  glyphrelay::SharedMemoryCapturePool pool(3U);
  auto pixels = uniform(4U, 3U, 20U, glyphrelay::PackedPixelOrder::bgra, 0U, 0U, 0U);
  auto buffer = view(pixels, 4U, 3U, 20U);
  buffer.crop = {1U, 0U, 3U, 3U};
  const std::array<std::uint8_t, 4U> cursor = {255U, 0U, 0U, 128U};
  buffer.cursor_mode = glyphrelay::CursorMode::metadata;
  buffer.cursor = glyphrelay::CursorMetadataView{
      .x = 2, .y = 1, .width = 1U, .height = 1U, .pitch = 4U, .rgba = cursor};
  const auto accepted = pool.ingest(buffer, 1U, []() {});
  require(accepted.accepted && accepted.geometry_epoch == 1U,
          "first accepted geometry must create epoch one");
  auto lease = pool.take_latest();
  require(lease && lease->frame().geometry.visible_width == 3U &&
              lease->frame().geometry.coded_width == 4U &&
              lease->frame().pixels[1U * 12U + 1U * 4U + 2U] == 128U,
          "metadata cursor composition must use source-visible crop coordinates");

  auto resized_pixels = uniform(2U, 2U, 8U, glyphrelay::PackedPixelOrder::rgba, 1U, 2U, 3U);
  auto resized = view(resized_pixels, 2U, 2U, 8U);
  require(pool.ingest(resized, 2U, []() {}).geometry_epoch == 2U,
          "resolution change must create a new immutable geometry epoch");
  auto resized_lease = pool.take_latest();
  require(resized_lease && resized_lease->frame().geometry.epoch == 2U,
          "latest-frame selection must not substitute the old geometry epoch");

  resized.pitch = 7U;
  bool invalid_requeued = false;
  const auto invalid = pool.ingest(resized, 3U, [&]() { invalid_requeued = true; });
  require(!invalid.accepted && invalid_requeued && invalid.reason == "capture_pitch_invalid",
          "malformed SHM metadata must fail closed and still requeue exactly once");
}

void test_color_goldens_and_padded_pitch() {
  struct Golden {
    std::array<std::uint8_t, 3U> rgb;
    std::array<std::uint8_t, 3U> limited;
    std::array<std::uint8_t, 3U> full;
  };
  const std::array<Golden, 5U> goldens = {
      Golden{{0U, 0U, 0U}, {16U, 128U, 128U}, {0U, 128U, 128U}},
      Golden{{255U, 255U, 255U}, {235U, 128U, 128U}, {255U, 128U, 128U}},
      Golden{{255U, 0U, 0U}, {63U, 102U, 240U}, {54U, 99U, 255U}},
      Golden{{0U, 255U, 0U}, {173U, 42U, 26U}, {182U, 30U, 12U}},
      Golden{{0U, 0U, 255U}, {32U, 240U, 118U}, {18U, 255U, 116U}},
  };
  for (const auto &golden : goldens) {
    for (const auto order :
         {glyphrelay::PackedPixelOrder::bgra, glyphrelay::PackedPixelOrder::rgba}) {
      auto pixels = uniform(2U, 2U, 12U, order, golden.rgb[0], golden.rgb[1], golden.rgb[2]);
      const auto source = frame(2U, 2U, order, 12U, std::move(pixels));
      const auto limited = glyphrelay::convert_bgra_or_rgba_to_nv12(
          source, glyphrelay::ColorRange::limited, glyphrelay::ColorConversionBackend::scalar);
      const auto full = glyphrelay::convert_bgra_or_rgba_to_nv12(
          source, glyphrelay::ColorRange::full, glyphrelay::ColorConversionBackend::scalar);
      require(limited.passed && full.passed && limited.image.bytes[0] == golden.limited[0] &&
                  limited.image.bytes[limited.image.chroma_offset] == golden.limited[1] &&
                  limited.image.bytes[limited.image.chroma_offset + 1U] == golden.limited[2] &&
                  full.image.bytes[0] == golden.full[0] &&
                  full.image.bytes[full.image.chroma_offset] == golden.full[1] &&
                  full.image.bytes[full.image.chroma_offset + 1U] == golden.full[2],
              "BT.709 scalar conversion must match the hand-calculated code values");
    }
  }

  auto odd_pixels = uniform(3U, 3U, 16U, glyphrelay::PackedPixelOrder::bgra, 50U, 100U, 150U);
  const auto odd = frame(3U, 3U, glyphrelay::PackedPixelOrder::bgra, 16U, std::move(odd_pixels));
  const auto converted = glyphrelay::convert_bgra_or_rgba_to_nv12(
      odd, glyphrelay::ColorRange::limited, glyphrelay::ColorConversionBackend::scalar);
  require(converted.passed && converted.image.coded_width == 4U &&
              converted.image.coded_height == 4U && converted.image.pitch == 4U &&
              converted.image.bytes.size() == 24U &&
              converted.image.bytes[3U] == converted.image.bytes[2U] &&
              converted.image.bytes[3U * 4U] == converted.image.bytes[2U * 4U],
          "odd visible crops and padded input pitch must clamp safely into even NV12 geometry");
}

void test_scalar_and_simd_are_byte_identical() {
  std::mt19937 random(0x47524C59U);
  std::uniform_int_distribution<unsigned int> byte(0U, 255U);
  for (std::size_t trial = 0U; trial < 100U; ++trial) {
    const auto width = 1U + trial % 31U;
    const auto height = 1U + trial * 7U % 23U;
    const auto pitch = width * 4U + trial % 13U;
    const auto order =
        trial % 2U == 0U ? glyphrelay::PackedPixelOrder::rgba : glyphrelay::PackedPixelOrder::bgra;
    const auto range =
        trial % 3U == 0U ? glyphrelay::ColorRange::full : glyphrelay::ColorRange::limited;
    std::vector<std::uint8_t> pixels(pitch * height);
    for (auto &value : pixels) {
      value = static_cast<std::uint8_t>(byte(random));
    }
    const auto source = frame(width, height, order, pitch, pixels);
    const auto scalar = glyphrelay::convert_bgra_or_rgba_to_nv12(
        source, range, glyphrelay::ColorConversionBackend::scalar);
    const auto automatic = glyphrelay::convert_bgra_or_rgba_to_nv12(
        source, range, glyphrelay::ColorConversionBackend::automatic);
    require(scalar.passed && automatic.passed && scalar.image.bytes == automatic.image.bytes,
            "automatic color conversion must remain byte-identical to the scalar reference");
    if (glyphrelay::color_conversion_simd_available()) {
      const auto simd = glyphrelay::convert_bgra_or_rgba_to_nv12(
          source, range, glyphrelay::ColorConversionBackend::simd);
      require(simd.passed && simd.image.bytes == scalar.image.bytes,
              "SIMD-assisted conversion must be byte-identical to the scalar reference");
    }
  }

  constexpr std::size_t width = 17U;
  auto invalid = frame(width, 2U, glyphrelay::PackedPixelOrder::rgba, width * 4U,
                       std::vector<std::uint8_t>(width * 4U * 2U));
  invalid.pitch = width * 4U - 1U;
  require(
      !glyphrelay::convert_bgra_or_rgba_to_nv12(invalid, glyphrelay::ColorRange::limited).passed,
      "an undersized packed-RGB pitch must fail before conversion");
}

} // namespace

int main() {
  test_portal_contract_and_lifecycle();
  test_prompt_copy_requeue_latest_frame_and_bounds();
  test_geometry_epoch_cursor_and_validation();
  test_color_goldens_and_padded_pitch();
  test_scalar_and_simd_are_byte_identical();
  return 0;
}
