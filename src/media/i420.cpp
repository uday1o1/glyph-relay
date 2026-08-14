#include "glyphrelay/i420.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace glyphrelay {
namespace {

std::size_t checked_product(std::size_t left, std::size_t right) {
  if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::overflow_error("frame geometry overflows size_t");
  }
  return left * right;
}

std::size_t chroma_bytes(const I420Frame &frame) {
  return checked_product(frame.width / 2U, frame.height / 2U);
}

} // namespace

std::span<std::uint8_t> I420Frame::y_plane() {
  return std::span(bytes).first(checked_product(width, height));
}

std::span<std::uint8_t> I420Frame::u_plane() {
  return std::span(bytes).subspan(checked_product(width, height), chroma_bytes(*this));
}

std::span<std::uint8_t> I420Frame::v_plane() { return std::span(bytes).last(chroma_bytes(*this)); }

std::span<const std::uint8_t> I420Frame::y_plane() const {
  return std::span(bytes).first(checked_product(width, height));
}

std::span<const std::uint8_t> I420Frame::u_plane() const {
  return std::span(bytes).subspan(checked_product(width, height), chroma_bytes(*this));
}

std::span<const std::uint8_t> I420Frame::v_plane() const {
  return std::span(bytes).last(chroma_bytes(*this));
}

I420Frame nv12_to_i420(std::span<const std::uint8_t> nv12, std::size_t coded_width,
                       std::size_t coded_height, std::size_t y_stride, std::size_t uv_stride,
                       std::size_t visible_width, std::size_t visible_height) {
  if (coded_width == 0U || coded_height == 0U || visible_width == 0U || visible_height == 0U ||
      (coded_width & 1U) != 0U || (coded_height & 1U) != 0U || (visible_width & 1U) != 0U ||
      (visible_height & 1U) != 0U || visible_width > coded_width || visible_height > coded_height ||
      y_stride < coded_width || uv_stride < coded_width) {
    throw std::invalid_argument("NV12 geometry or stride is invalid");
  }
  const auto y_storage = checked_product(y_stride, coded_height);
  const auto uv_storage = checked_product(uv_stride, coded_height / 2U);
  if (y_storage > nv12.size() || uv_storage > nv12.size() - y_storage) {
    throw std::invalid_argument("NV12 source is smaller than its coded planes");
  }
  I420Frame result;
  result.width = visible_width;
  result.height = visible_height;
  result.y_stride = visible_width;
  result.u_stride = visible_width / 2U;
  result.v_stride = visible_width / 2U;
  const auto visible_luma_bytes = checked_product(visible_width, visible_height);
  const auto visible_chroma_bytes = checked_product(visible_width / 2U, visible_height / 2U);
  result.bytes.resize(visible_luma_bytes + visible_chroma_bytes * 2U);

  auto y = result.y_plane();
  for (std::size_t row = 0; row < visible_height; ++row) {
    std::copy_n(nv12.data() + row * y_stride, visible_width, y.data() + row * visible_width);
  }
  auto u = result.u_plane();
  auto v = result.v_plane();
  const auto uv_offset = y_storage;
  for (std::size_t row = 0; row < visible_height / 2U; ++row) {
    for (std::size_t column = 0; column < visible_width / 2U; ++column) {
      u[row * result.u_stride + column] = nv12[uv_offset + row * uv_stride + column * 2U];
      v[row * result.v_stride + column] = nv12[uv_offset + row * uv_stride + column * 2U + 1U];
    }
  }
  return result;
}

} // namespace glyphrelay
