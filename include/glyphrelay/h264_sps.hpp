#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace glyphrelay {

enum class H264ProfileFamily {
  constrained_baseline,
  baseline,
  main,
  extended,
  high,
  constrained_high,
  unknown,
};

struct H264ProfileLevelId {
  std::uint8_t profile_idc = 0;
  std::uint8_t profile_iop = 0;
  std::uint8_t level_idc = 0;
  H264ProfileFamily family = H264ProfileFamily::unknown;
};

struct H264SpsInfo {
  H264ProfileLevelId profile_level;
  std::uint32_t sps_id = 0;
  std::size_t coded_width = 0;
  std::size_t coded_height = 0;
  std::size_t visible_width = 0;
  std::size_t visible_height = 0;
  bool frame_mbs_only = false;
  bool vui_present = false;
  bool video_signal_present = false;
  bool full_range = false;
  bool color_description_present = false;
  std::uint8_t color_primaries = 2;
  std::uint8_t transfer_characteristics = 2;
  std::uint8_t matrix_coefficients = 2;
};

struct H264SpsParseResult {
  bool passed = false;
  std::string reason;
  H264SpsInfo info;
};

H264ProfileFamily classify_h264_profile(std::uint8_t profile_idc, std::uint8_t profile_iop);
std::string h264_profile_family_name(H264ProfileFamily family);
H264SpsParseResult parse_h264_sps(std::span<const std::uint8_t> nal_payload);

} // namespace glyphrelay
