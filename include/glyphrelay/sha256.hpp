#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace glyphrelay {

class Sha256 {
public:
  using Digest = std::array<std::uint8_t, 32>;

  void update(std::span<const std::uint8_t> bytes);
  Digest finalize();

  static Digest digest(std::span<const std::uint8_t> bytes);
  static Digest digest(std::string_view value);

private:
  void transform(const std::uint8_t *block);

  std::array<std::uint32_t, 8> state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                         0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finalized_ = false;
};

std::string sha256_hex(const Sha256::Digest &digest);
std::string sha256_hex(std::span<const std::uint8_t> bytes);
std::string sha256_hex(std::string_view value);
std::string sha256_file_hex(const std::filesystem::path &path);

} // namespace glyphrelay
