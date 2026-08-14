#include "glyphrelay/annex_b.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace glyphrelay {
namespace {

constexpr std::size_t kMaximumAccessUnitBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumNalBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumNalUnits = 1024U;

struct StartCode {
  std::size_t offset;
  std::size_t size;
};

StartCode find_start_code(std::span<const std::uint8_t> bytes, std::size_t start) {
  for (std::size_t index = start; index + 2U < bytes.size(); ++index) {
    if (bytes[index] != 0U || bytes[index + 1U] != 0U) {
      continue;
    }
    if (bytes[index + 2U] == 1U) {
      return {index, 3U};
    }
    if (index + 3U < bytes.size() && bytes[index + 2U] == 0U && bytes[index + 3U] == 1U) {
      return {index, 4U};
    }
  }
  return {bytes.size(), 0U};
}

bool valid_ebsp(std::span<const std::uint8_t> payload) {
  if (payload.size() <= 1U) {
    return false;
  }
  unsigned int zero_count = 0;
  for (std::size_t index = 1U; index < payload.size(); ++index) {
    const auto byte = payload[index];
    if (zero_count >= 2U) {
      if (byte <= 2U) {
        return false;
      }
      if (byte == 3U && (index + 1U >= payload.size() || payload[index + 1U] > 3U)) {
        return false;
      }
    }
    zero_count = byte == 0U ? zero_count + 1U : 0U;
  }
  return true;
}

} // namespace

std::span<const std::uint8_t> AnnexBAccessUnit::payload(const AnnexBNalUnit &unit) const {
  if (unit.payload_offset > bytes.size() ||
      unit.payload_size > bytes.size() - unit.payload_offset) {
    throw std::out_of_range("Annex B NAL metadata exceeds its access unit");
  }
  return std::span(bytes).subspan(unit.payload_offset, unit.payload_size);
}

bool AnnexBAccessUnit::contains(std::uint8_t unit_type) const {
  return std::any_of(nal_units.begin(), nal_units.end(), [unit_type](const AnnexBNalUnit &unit) {
    return unit.unit_type == unit_type;
  });
}

bool AnnexBAccessUnit::starts_with_parameter_sets_and_idr() const {
  bool sps = false;
  bool pps = false;
  for (const auto &unit : nal_units) {
    if (unit.unit_type == 7U) {
      sps = true;
    } else if (unit.unit_type == 8U) {
      pps = sps;
    } else if (unit.unit_type == 5U) {
      return sps && pps;
    } else if (unit.unit_type >= 1U && unit.unit_type <= 5U) {
      return false;
    }
  }
  return false;
}

AnnexBParseResult parse_annex_b_access_unit(std::span<const std::uint8_t> bytes) {
  AnnexBParseResult result;
  if (bytes.empty()) {
    result.reason = "annex_b_empty";
    return result;
  }
  if (bytes.size() > kMaximumAccessUnitBytes) {
    result.reason = "annex_b_access_unit_too_large";
    return result;
  }
  const auto first = find_start_code(bytes, 0U);
  if (first.size == 0U ||
      !std::all_of(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(first.offset),
                   [](std::uint8_t byte) { return byte == 0U; })) {
    result.reason = "annex_b_missing_initial_start_code";
    return result;
  }

  result.access_unit.bytes.assign(bytes.begin(), bytes.end());
  StartCode current = first;
  while (current.size != 0U) {
    if (result.access_unit.nal_units.size() >= kMaximumNalUnits) {
      result.reason = "annex_b_too_many_nal_units";
      result.access_unit = {};
      return result;
    }
    const auto payload_offset = current.offset + current.size;
    const auto next = find_start_code(bytes, payload_offset);
    std::size_t payload_end = next.size == 0U ? bytes.size() : next.offset;
    while (payload_end > payload_offset && bytes[payload_end - 1U] == 0U) {
      --payload_end;
    }
    if (payload_end == payload_offset) {
      result.reason = "annex_b_empty_nal_unit";
      result.access_unit = {};
      return result;
    }
    const auto payload_size = payload_end - payload_offset;
    if (payload_size > kMaximumNalBytes) {
      result.reason = "annex_b_nal_unit_too_large";
      result.access_unit = {};
      return result;
    }
    const auto header = bytes[payload_offset];
    const auto unit_type = static_cast<std::uint8_t>(header & 0x1FU);
    if ((header & 0x80U) != 0U) {
      result.reason = "annex_b_forbidden_zero_bit_set";
      result.access_unit = {};
      return result;
    }
    if (unit_type == 0U || unit_type > 23U) {
      result.reason = "annex_b_unsupported_nal_unit_type";
      result.access_unit = {};
      return result;
    }
    if (!valid_ebsp(bytes.subspan(payload_offset, payload_size))) {
      result.reason = "annex_b_invalid_emulation_prevention";
      result.access_unit = {};
      return result;
    }
    result.access_unit.nal_units.push_back(
        {current.offset, current.size, payload_offset, payload_size,
         static_cast<std::uint8_t>((header >> 5U) & 0x03U), unit_type});
    current = next;
  }
  if (!std::any_of(
          result.access_unit.nal_units.begin(), result.access_unit.nal_units.end(),
          [](const AnnexBNalUnit &unit) { return unit.unit_type >= 1U && unit.unit_type <= 5U; })) {
    result.reason = "annex_b_access_unit_has_no_vcl";
    result.access_unit = {};
    return result;
  }
  result.passed = true;
  result.reason = "annex_b_access_unit_valid";
  return result;
}

std::vector<std::uint8_t> annex_b_nal_to_rbsp(std::span<const std::uint8_t> nal_payload) {
  if (nal_payload.empty() || (nal_payload.front() & 0x80U) != 0U) {
    throw std::invalid_argument("NAL payload has an invalid header");
  }
  if (!valid_ebsp(nal_payload)) {
    throw std::invalid_argument("NAL payload has invalid emulation prevention");
  }
  std::vector<std::uint8_t> rbsp;
  rbsp.reserve(nal_payload.size() - 1U);
  unsigned int zero_count = 0;
  for (std::size_t index = 1U; index < nal_payload.size(); ++index) {
    const auto byte = nal_payload[index];
    if (zero_count >= 2U && byte == 3U) {
      zero_count = 0U;
      continue;
    }
    rbsp.push_back(byte);
    zero_count = byte == 0U ? zero_count + 1U : 0U;
  }
  return rbsp;
}

} // namespace glyphrelay
