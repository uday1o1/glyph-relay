#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace glyphrelay {

struct AnnexBNalUnit {
  std::size_t start_code_offset = 0;
  std::size_t start_code_size = 0;
  std::size_t payload_offset = 0;
  std::size_t payload_size = 0;
  std::uint8_t nal_ref_idc = 0;
  std::uint8_t unit_type = 0;
};

struct AnnexBAccessUnit {
  std::vector<std::uint8_t> bytes;
  std::vector<AnnexBNalUnit> nal_units;

  std::span<const std::uint8_t> payload(const AnnexBNalUnit &unit) const;
  bool contains(std::uint8_t unit_type) const;
  bool starts_with_parameter_sets_and_idr() const;
};

struct AnnexBParseResult {
  bool passed = false;
  std::string reason;
  AnnexBAccessUnit access_unit;
};

AnnexBParseResult parse_annex_b_access_unit(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> annex_b_nal_to_rbsp(std::span<const std::uint8_t> nal_payload);

} // namespace glyphrelay
