#include "glyphrelay/m0_browser_source.hpp"
#include "glyphrelay/sha256.hpp"

#include <array>
#include <cstddef>
#include <iostream>

int main() {
  glyphrelay::M0BrowserSyntheticSource source;
  constexpr std::array<std::size_t, 3U> frames = {
      0U,
      glyphrelay::M0BrowserSourceGeometry::warmup_frames,
      glyphrelay::M0BrowserSourceGeometry::frame_count - 1U,
  };
  std::cout << "frame_index\tsha256\n";
  for (const auto frame_index : frames) {
    std::cout << frame_index << '\t' << glyphrelay::sha256_hex(source.generate(frame_index))
              << '\n';
  }
}
