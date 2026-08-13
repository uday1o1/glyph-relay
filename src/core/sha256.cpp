#include "glyphrelay/sha256.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace glyphrelay {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32U - count));
}

} // namespace

void Sha256::transform(const std::uint8_t *block) {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16U; ++index) {
    const std::size_t offset = index * 4U;
    words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                   (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                   (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                   static_cast<std::uint32_t>(block[offset + 3U]);
  }
  for (std::size_t index = 16U; index < words.size(); ++index) {
    const auto s0 = rotate_right(words[index - 15U], 7U) ^ rotate_right(words[index - 15U], 18U) ^
                    (words[index - 15U] >> 3U);
    const auto s1 = rotate_right(words[index - 2U], 17U) ^ rotate_right(words[index - 2U], 19U) ^
                    (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
  }

  auto a = state_[0];
  auto b = state_[1];
  auto c = state_[2];
  auto d = state_[3];
  auto e = state_[4];
  auto f = state_[5];
  auto g = state_[6];
  auto h = state_[7];
  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
    const auto choice = (e & f) ^ ((~e) & g);
    const auto temporary1 = h + sum1 + choice + kRoundConstants[index] + words[index];
    const auto sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    const auto temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> bytes) {
  if (finalized_) {
    throw std::logic_error("cannot update a finalized SHA-256 state");
  }
  total_bytes_ += static_cast<std::uint64_t>(bytes.size());
  std::size_t offset = 0;
  if (buffered_ != 0U) {
    const auto count = std::min(buffer_.size() - buffered_, bytes.size());
    std::copy_n(bytes.data(), count, buffer_.data() + buffered_);
    buffered_ += count;
    offset += count;
    if (buffered_ == buffer_.size()) {
      transform(buffer_.data());
      buffered_ = 0;
    }
  }
  while (offset + buffer_.size() <= bytes.size()) {
    transform(bytes.data() + offset);
    offset += buffer_.size();
  }
  if (offset < bytes.size()) {
    buffered_ = bytes.size() - offset;
    std::copy_n(bytes.data() + offset, buffered_, buffer_.data());
  }
}

Sha256::Digest Sha256::finalize() {
  if (finalized_) {
    throw std::logic_error("cannot finalize SHA-256 more than once");
  }
  finalized_ = true;
  const std::uint64_t bit_count = total_bytes_ * 8U;
  buffer_[buffered_++] = 0x80U;
  if (buffered_ > 56U) {
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), 0U);
    transform(buffer_.data());
    buffered_ = 0;
  }
  std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin() + 56, 0U);
  for (std::size_t index = 0; index < 8U; ++index) {
    buffer_[63U - index] = static_cast<std::uint8_t>(bit_count >> (index * 8U));
  }
  transform(buffer_.data());

  Digest result{};
  for (std::size_t index = 0; index < state_.size(); ++index) {
    result[index * 4U] = static_cast<std::uint8_t>(state_[index] >> 24U);
    result[index * 4U + 1U] = static_cast<std::uint8_t>(state_[index] >> 16U);
    result[index * 4U + 2U] = static_cast<std::uint8_t>(state_[index] >> 8U);
    result[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index]);
  }
  return result;
}

Sha256::Digest Sha256::digest(std::span<const std::uint8_t> bytes) {
  Sha256 hash;
  hash.update(bytes);
  return hash.finalize();
}

Sha256::Digest Sha256::digest(std::string_view value) {
  return digest(std::span(reinterpret_cast<const std::uint8_t *>(value.data()), value.size()));
}

std::string sha256_hex(const Sha256::Digest &digest) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(digest.size() * 2U, '0');
  for (std::size_t index = 0; index < digest.size(); ++index) {
    result[index * 2U] = digits[digest[index] >> 4U];
    result[index * 2U + 1U] = digits[digest[index] & 0x0FU];
  }
  return result;
}

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
  return sha256_hex(Sha256::digest(bytes));
}

std::string sha256_hex(std::string_view value) { return sha256_hex(Sha256::digest(value)); }

std::string sha256_file_hex(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open file for SHA-256: " + path.string());
  }
  Sha256 hash;
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  while (input) {
    input.read(reinterpret_cast<char *>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      hash.update(std::span(buffer.data(), static_cast<std::size_t>(count)));
    }
  }
  if (!input.eof()) {
    throw std::runtime_error("failed while hashing file: " + path.string());
  }
  return sha256_hex(hash.finalize());
}

} // namespace glyphrelay
