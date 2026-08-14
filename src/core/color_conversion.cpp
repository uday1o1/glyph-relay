#include "glyphrelay/color_conversion.hpp"

#include <algorithm>
#include <array>
#include <limits>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define GLYPHRELAY_COLOR_NEON 1
#else
#define GLYPHRELAY_COLOR_NEON 0
#endif

#if defined(__SSE2__)
#include <emmintrin.h>
#define GLYPHRELAY_COLOR_SSE2 1
#else
#define GLYPHRELAY_COLOR_SSE2 0
#endif

namespace glyphrelay {
namespace {

struct Rgb {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
};

std::int64_t round_divide_ties_even(std::int64_t numerator, std::int64_t denominator) {
  const bool negative = numerator < 0;
  const auto magnitude = static_cast<std::uint64_t>(negative ? -numerator : numerator);
  const auto divisor = static_cast<std::uint64_t>(denominator);
  auto quotient = magnitude / divisor;
  const auto remainder = magnitude % divisor;
  if (remainder > divisor / 2U || (remainder * 2U == divisor && (quotient & 1U) != 0U)) {
    ++quotient;
  }
  const auto result = static_cast<std::int64_t>(quotient);
  return negative ? -result : result;
}

std::uint8_t clamp_code(std::int64_t value) {
  return static_cast<std::uint8_t>(std::clamp<std::int64_t>(value, 0, 255));
}

Rgb read_pixel(const CapturedFrame &frame, std::size_t x, std::size_t y) {
  const auto offset = y * frame.pitch + x * 4U;
  if (frame.pixel_order == PackedPixelOrder::rgba) {
    return {frame.pixels[offset], frame.pixels[offset + 1U], frame.pixels[offset + 2U]};
  }
  return {frame.pixels[offset + 2U], frame.pixels[offset + 1U], frame.pixels[offset]};
}

std::uint8_t luma(Rgb rgb, ColorRange range) {
  constexpr std::int64_t denominator = 10'000LL * 255LL;
  const auto scale = range == ColorRange::limited ? 219LL : 255LL;
  const auto offset = range == ColorRange::limited ? 16LL : 0LL;
  const auto weighted = 2'126LL * rgb.red + 7'152LL * rgb.green + 722LL * rgb.blue;
  return clamp_code(offset + round_divide_ties_even(scale * weighted, denominator));
}

std::array<std::uint8_t, 2U> chroma(std::uint64_t red_sum, std::uint64_t green_sum,
                                    std::uint64_t blue_sum, ColorRange range) {
  const auto scale = range == ColorRange::limited ? 224LL : 255LL;
  constexpr std::int64_t denominator = 1'000'000LL * 255LL * 4LL;
  const auto cb = -114'572LL * static_cast<std::int64_t>(red_sum) -
                  385'428LL * static_cast<std::int64_t>(green_sum) +
                  500'000LL * static_cast<std::int64_t>(blue_sum);
  const auto cr = 500'000LL * static_cast<std::int64_t>(red_sum) -
                  454'153LL * static_cast<std::int64_t>(green_sum) -
                  45'847LL * static_cast<std::int64_t>(blue_sum);
  return {clamp_code(128LL + round_divide_ties_even(scale * cb, denominator)),
          clamp_code(128LL + round_divide_ties_even(scale * cr, denominator))};
}

bool validate_frame(const CapturedFrame &frame, std::string &reason) {
  const auto width = frame.geometry.visible_width;
  const auto height = frame.geometry.visible_height;
  if (width == 0U || height == 0U || width > std::numeric_limits<std::size_t>::max() / 4U ||
      frame.pitch < width * 4U) {
    reason = "color_frame_geometry_invalid";
    return false;
  }
  if (height - 1U > std::numeric_limits<std::size_t>::max() / frame.pitch ||
      width * 4U > std::numeric_limits<std::size_t>::max() - (height - 1U) * frame.pitch ||
      frame.pixels.size() < (height - 1U) * frame.pitch + width * 4U) {
    reason = "color_frame_buffer_too_small";
    return false;
  }
  return true;
}

void scalar_luma(const CapturedFrame &frame, ColorRange range, Nv12Image &output) {
  for (std::size_t y = 0U; y < output.coded_height; ++y) {
    const auto source_y = std::min(y, output.visible_height - 1U);
    for (std::size_t x = 0U; x < output.coded_width; ++x) {
      const auto source_x = std::min(x, output.visible_width - 1U);
      output.bytes[y * output.pitch + x] = luma(read_pixel(frame, source_x, source_y), range);
    }
  }
}

void simd_luma(const CapturedFrame &frame, ColorRange range, Nv12Image &output) {
  for (std::size_t y = 0U; y < output.visible_height; ++y) {
    std::size_t x = 0U;
#if GLYPHRELAY_COLOR_NEON
    for (; x + 8U <= output.visible_width; x += 8U) {
      const auto *source = frame.pixels.data() + y * frame.pitch + x * 4U;
      const auto channels = vld4_u8(source);
      std::array<std::uint8_t, 8U> first{};
      std::array<std::uint8_t, 8U> second{};
      std::array<std::uint8_t, 8U> third{};
      vst1_u8(first.data(), channels.val[0]);
      vst1_u8(second.data(), channels.val[1]);
      vst1_u8(third.data(), channels.val[2]);
      for (std::size_t lane = 0U; lane < 8U; ++lane) {
        const Rgb rgb = frame.pixel_order == PackedPixelOrder::rgba
                            ? Rgb{first[lane], second[lane], third[lane]}
                            : Rgb{third[lane], second[lane], first[lane]};
        output.bytes[y * output.pitch + x + lane] = luma(rgb, range);
      }
    }
#elif GLYPHRELAY_COLOR_SSE2
    for (; x + 4U <= output.visible_width; x += 4U) {
      const auto *source = frame.pixels.data() + y * frame.pitch + x * 4U;
      const auto packed = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source));
      alignas(16) std::array<std::uint32_t, 4U> pixels{};
      _mm_store_si128(reinterpret_cast<__m128i *>(pixels.data()), packed);
      for (std::size_t lane = 0U; lane < 4U; ++lane) {
        const auto value = pixels[lane];
        const auto first = static_cast<std::uint8_t>(value & 0xFFU);
        const auto second = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
        const auto third = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
        const Rgb rgb = frame.pixel_order == PackedPixelOrder::rgba ? Rgb{first, second, third}
                                                                    : Rgb{third, second, first};
        output.bytes[y * output.pitch + x + lane] = luma(rgb, range);
      }
    }
#endif
    for (; x < output.visible_width; ++x) {
      output.bytes[y * output.pitch + x] = luma(read_pixel(frame, x, y), range);
    }
    for (; x < output.coded_width; ++x) {
      output.bytes[y * output.pitch + x] =
          output.bytes[y * output.pitch + output.visible_width - 1U];
    }
  }
  if (output.coded_height > output.visible_height) {
    std::copy_n(output.bytes.begin() +
                    static_cast<std::ptrdiff_t>((output.visible_height - 1U) * output.pitch),
                output.pitch,
                output.bytes.begin() +
                    static_cast<std::ptrdiff_t>(output.visible_height * output.pitch));
  }
}

void convert_chroma(const CapturedFrame &frame, ColorRange range, Nv12Image &output) {
  for (std::size_t y = 0U; y < output.coded_height; y += 2U) {
    for (std::size_t x = 0U; x < output.coded_width; x += 2U) {
      std::uint64_t red = 0U;
      std::uint64_t green = 0U;
      std::uint64_t blue = 0U;
      for (std::size_t dy = 0U; dy < 2U; ++dy) {
        for (std::size_t dx = 0U; dx < 2U; ++dx) {
          const auto rgb = read_pixel(frame, std::min(x + dx, output.visible_width - 1U),
                                      std::min(y + dy, output.visible_height - 1U));
          red += rgb.red;
          green += rgb.green;
          blue += rgb.blue;
        }
      }
      const auto codes = chroma(red, green, blue, range);
      const auto offset = output.chroma_offset + y / 2U * output.pitch + x;
      output.bytes[offset] = codes[0];
      output.bytes[offset + 1U] = codes[1];
    }
  }
}

} // namespace

bool color_conversion_simd_available() {
  return GLYPHRELAY_COLOR_NEON != 0 || GLYPHRELAY_COLOR_SSE2 != 0;
}

ColorConversionResult convert_bgra_or_rgba_to_nv12(const CapturedFrame &frame, ColorRange range,
                                                   ColorConversionBackend backend) {
  ColorConversionResult result;
  if (!validate_frame(frame, result.reason)) {
    return result;
  }
  if (backend == ColorConversionBackend::simd && !color_conversion_simd_available()) {
    result.reason = "color_simd_backend_unavailable";
    return result;
  }
  result.backend = backend == ColorConversionBackend::automatic
                       ? (color_conversion_simd_available() ? ColorConversionBackend::simd
                                                            : ColorConversionBackend::scalar)
                       : backend;
  auto &output = result.image;
  output.visible_width = frame.geometry.visible_width;
  output.visible_height = frame.geometry.visible_height;
  output.coded_width = (output.visible_width + 1U) & ~std::size_t{1U};
  output.coded_height = (output.visible_height + 1U) & ~std::size_t{1U};
  output.pitch = output.coded_width;
  if (output.coded_height > std::numeric_limits<std::size_t>::max() / output.pitch) {
    result.reason = "color_output_size_overflow";
    return result;
  }
  output.chroma_offset = output.pitch * output.coded_height;
  const auto chroma_bytes = output.pitch * output.coded_height / 2U;
  if (chroma_bytes > std::numeric_limits<std::size_t>::max() - output.chroma_offset) {
    result.reason = "color_output_size_overflow";
    return result;
  }
  output.bytes.resize(output.chroma_offset + chroma_bytes);
  if (result.backend == ColorConversionBackend::simd) {
    simd_luma(frame, range, output);
  } else {
    scalar_luma(frame, range, output);
  }
  convert_chroma(frame, range, output);
  result.passed = true;
  result.reason = "color_conversion_complete";
  return result;
}

std::string_view color_range_name(ColorRange range) {
  return range == ColorRange::limited ? "limited" : "full";
}

std::string_view color_conversion_backend_name(ColorConversionBackend backend) {
  switch (backend) {
  case ColorConversionBackend::automatic:
    return "automatic";
  case ColorConversionBackend::scalar:
    return "scalar";
  case ColorConversionBackend::simd:
    return "simd";
  }
  return "unknown";
}

} // namespace glyphrelay
