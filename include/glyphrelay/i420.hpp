#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace glyphrelay {

struct I420Frame {
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t y_stride = 0;
  std::size_t u_stride = 0;
  std::size_t v_stride = 0;
  std::vector<std::uint8_t> bytes;

  std::span<std::uint8_t> y_plane();
  std::span<std::uint8_t> u_plane();
  std::span<std::uint8_t> v_plane();
  std::span<const std::uint8_t> y_plane() const;
  std::span<const std::uint8_t> u_plane() const;
  std::span<const std::uint8_t> v_plane() const;
};

I420Frame nv12_to_i420(std::span<const std::uint8_t> nv12, std::size_t coded_width,
                       std::size_t coded_height, std::size_t y_stride, std::size_t uv_stride,
                       std::size_t visible_width, std::size_t visible_height);

} // namespace glyphrelay
