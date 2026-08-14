#include "glyphrelay/h264_sps.hpp"

#include "glyphrelay/annex_b.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace glyphrelay {
namespace {

class BitReader {
public:
  explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  bool read_bit(bool &value) {
    std::uint32_t bit = 0;
    if (!read_bits(1U, bit)) {
      return false;
    }
    value = bit != 0U;
    return true;
  }

  bool read_bits(unsigned int count, std::uint32_t &value) {
    if (count > 32U || count > remaining_bits()) {
      return false;
    }
    value = 0;
    for (unsigned int index = 0; index < count; ++index) {
      const auto byte = bytes_[bit_offset_ / 8U];
      const auto shift = 7U - static_cast<unsigned int>(bit_offset_ % 8U);
      value = (value << 1U) | ((byte >> shift) & 1U);
      ++bit_offset_;
    }
    return true;
  }

  bool read_ue(std::uint32_t &value) {
    unsigned int leading_zeroes = 0;
    bool bit = false;
    while (true) {
      if (!read_bit(bit)) {
        return false;
      }
      if (bit) {
        break;
      }
      if (++leading_zeroes >= 31U) {
        return false;
      }
    }
    std::uint32_t suffix = 0;
    if (leading_zeroes != 0U && !read_bits(leading_zeroes, suffix)) {
      return false;
    }
    value = ((1U << leading_zeroes) - 1U) + suffix;
    return true;
  }

  bool read_se(std::int32_t &value) {
    std::uint32_t code = 0;
    if (!read_ue(code)) {
      return false;
    }
    const auto magnitude = static_cast<std::int32_t>((code + 1U) / 2U);
    value = (code & 1U) != 0U ? magnitude : -magnitude;
    return true;
  }

private:
  std::size_t remaining_bits() const { return bytes_.size() * 8U - bit_offset_; }

  std::span<const std::uint8_t> bytes_;
  std::size_t bit_offset_ = 0;
};

bool skip_scaling_list(BitReader &reader, std::size_t count) {
  std::int32_t last_scale = 8;
  std::int32_t next_scale = 8;
  for (std::size_t index = 0; index < count; ++index) {
    if (next_scale != 0) {
      std::int32_t delta_scale = 0;
      if (!reader.read_se(delta_scale)) {
        return false;
      }
      next_scale = (last_scale + delta_scale + 256) % 256;
    }
    last_scale = next_scale == 0 ? last_scale : next_scale;
  }
  return true;
}

bool parse_high_profile_fields(BitReader &reader, std::uint8_t profile_idc,
                               std::uint32_t &chroma_format_idc, bool &separate_color_plane) {
  constexpr std::array high_profiles = {100U, 110U, 122U, 244U, 44U,  83U, 86U,
                                        118U, 128U, 138U, 139U, 134U, 135U};
  if (std::find(high_profiles.begin(), high_profiles.end(), profile_idc) == high_profiles.end()) {
    return true;
  }
  if (!reader.read_ue(chroma_format_idc) || chroma_format_idc > 3U) {
    return false;
  }
  if (chroma_format_idc == 3U && !reader.read_bit(separate_color_plane)) {
    return false;
  }
  std::uint32_t bit_depth_luma_minus8 = 0;
  std::uint32_t bit_depth_chroma_minus8 = 0;
  bool qpprime_bypass = false;
  bool scaling_matrix = false;
  if (!reader.read_ue(bit_depth_luma_minus8) || bit_depth_luma_minus8 > 6U ||
      !reader.read_ue(bit_depth_chroma_minus8) || bit_depth_chroma_minus8 > 6U ||
      !reader.read_bit(qpprime_bypass) || !reader.read_bit(scaling_matrix)) {
    return false;
  }
  if (scaling_matrix) {
    const std::size_t count = chroma_format_idc == 3U ? 12U : 8U;
    for (std::size_t index = 0; index < count; ++index) {
      bool present = false;
      if (!reader.read_bit(present)) {
        return false;
      }
      if (present && !skip_scaling_list(reader, index < 6U ? 16U : 64U)) {
        return false;
      }
    }
  }
  return true;
}

bool skip_hrd(BitReader &reader) {
  std::uint32_t cpb_count_minus1 = 0;
  std::uint32_t ignored = 0;
  if (!reader.read_ue(cpb_count_minus1) || cpb_count_minus1 > 31U ||
      !reader.read_bits(4U, ignored) || !reader.read_bits(4U, ignored)) {
    return false;
  }
  for (std::uint32_t index = 0; index <= cpb_count_minus1; ++index) {
    bool cbr = false;
    if (!reader.read_ue(ignored) || !reader.read_ue(ignored) || !reader.read_bit(cbr)) {
      return false;
    }
  }
  return reader.read_bits(5U, ignored) && reader.read_bits(5U, ignored) &&
         reader.read_bits(5U, ignored) && reader.read_bits(5U, ignored);
}

bool parse_vui(BitReader &reader, H264SpsInfo &info) {
  bool present = false;
  std::uint32_t value = 0;
  if (!reader.read_bit(present)) {
    return false;
  }
  if (present) {
    if (!reader.read_bits(8U, value)) {
      return false;
    }
    if (value == 255U && (!reader.read_bits(16U, value) || !reader.read_bits(16U, value))) {
      return false;
    }
  }
  if (!reader.read_bit(present) || (present && !reader.read_bit(present))) {
    return false;
  }
  if (!reader.read_bit(info.video_signal_present)) {
    return false;
  }
  if (info.video_signal_present) {
    if (!reader.read_bits(3U, value) || !reader.read_bit(info.full_range) ||
        !reader.read_bit(info.color_description_present)) {
      return false;
    }
    if (info.color_description_present) {
      if (!reader.read_bits(8U, value)) {
        return false;
      }
      info.color_primaries = static_cast<std::uint8_t>(value);
      if (!reader.read_bits(8U, value)) {
        return false;
      }
      info.transfer_characteristics = static_cast<std::uint8_t>(value);
      if (!reader.read_bits(8U, value)) {
        return false;
      }
      info.matrix_coefficients = static_cast<std::uint8_t>(value);
    }
  }
  if (!reader.read_bit(present)) {
    return false;
  }
  if (present && (!reader.read_ue(value) || !reader.read_ue(value))) {
    return false;
  }
  if (!reader.read_bit(present)) {
    return false;
  }
  if (present) {
    bool fixed = false;
    if (!reader.read_bits(32U, value) || !reader.read_bits(32U, value) || !reader.read_bit(fixed)) {
      return false;
    }
  }
  bool nal_hrd = false;
  bool vcl_hrd = false;
  if (!reader.read_bit(nal_hrd) || (nal_hrd && !skip_hrd(reader)) || !reader.read_bit(vcl_hrd) ||
      (vcl_hrd && !skip_hrd(reader))) {
    return false;
  }
  if ((nal_hrd || vcl_hrd) && !reader.read_bit(present)) {
    return false;
  }
  if (!reader.read_bit(present) || !reader.read_bit(present)) {
    return false;
  }
  if (present) {
    if (!reader.read_bit(present) || !reader.read_ue(value) || !reader.read_ue(value) ||
        !reader.read_ue(value) || !reader.read_ue(value) || !reader.read_ue(value) ||
        !reader.read_ue(value)) {
      return false;
    }
  }
  return true;
}

} // namespace

H264ProfileFamily classify_h264_profile(std::uint8_t profile_idc, std::uint8_t profile_iop) {
  if ((profile_idc == 66U && (profile_iop & 0x4FU) == 0x40U) ||
      (profile_idc == 77U && (profile_iop & 0x8FU) == 0x80U) ||
      (profile_idc == 88U && (profile_iop & 0xCFU) == 0xC0U)) {
    return H264ProfileFamily::constrained_baseline;
  }
  if (profile_idc == 66U && (profile_iop & 0x4FU) == 0U) {
    return H264ProfileFamily::baseline;
  }
  if (profile_idc == 77U && (profile_iop & 0xAFU) == 0U) {
    return H264ProfileFamily::main;
  }
  if (profile_idc == 88U && (profile_iop & 0xCFU) == 0U) {
    return H264ProfileFamily::extended;
  }
  if (profile_idc == 100U && (profile_iop & 0xFFU) == 0x0CU) {
    return H264ProfileFamily::constrained_high;
  }
  if (profile_idc == 100U) {
    return H264ProfileFamily::high;
  }
  return H264ProfileFamily::unknown;
}

std::string h264_profile_family_name(H264ProfileFamily family) {
  switch (family) {
  case H264ProfileFamily::constrained_baseline:
    return "constrained_baseline";
  case H264ProfileFamily::baseline:
    return "baseline";
  case H264ProfileFamily::main:
    return "main";
  case H264ProfileFamily::extended:
    return "extended";
  case H264ProfileFamily::high:
    return "high";
  case H264ProfileFamily::constrained_high:
    return "constrained_high";
  case H264ProfileFamily::unknown:
    return "unknown";
  }
  return "unknown";
}

H264SpsParseResult parse_h264_sps(std::span<const std::uint8_t> nal_payload) {
  H264SpsParseResult result;
  if (nal_payload.empty() || (nal_payload.front() & 0x1FU) != 7U) {
    result.reason = "sps_nal_unit_required";
    return result;
  }
  std::vector<std::uint8_t> rbsp;
  try {
    rbsp = annex_b_nal_to_rbsp(nal_payload);
  } catch (const std::exception &) {
    result.reason = "sps_invalid_ebsp";
    return result;
  }
  BitReader reader(rbsp);
  std::uint32_t profile_idc = 0;
  std::uint32_t profile_iop = 0;
  std::uint32_t level_idc = 0;
  if (!reader.read_bits(8U, profile_idc) || !reader.read_bits(8U, profile_iop) ||
      (profile_iop & 0x03U) != 0U || !reader.read_bits(8U, level_idc) ||
      !reader.read_ue(result.info.sps_id) || result.info.sps_id > 31U) {
    result.reason = "sps_invalid_profile_or_identifier";
    return result;
  }
  result.info.profile_level = {static_cast<std::uint8_t>(profile_idc),
                               static_cast<std::uint8_t>(profile_iop),
                               static_cast<std::uint8_t>(level_idc),
                               classify_h264_profile(static_cast<std::uint8_t>(profile_idc),
                                                     static_cast<std::uint8_t>(profile_iop))};

  std::uint32_t chroma_format_idc = 1U;
  bool separate_color_plane = false;
  if (!parse_high_profile_fields(reader, static_cast<std::uint8_t>(profile_idc), chroma_format_idc,
                                 separate_color_plane)) {
    result.reason = "sps_invalid_high_profile_fields";
    return result;
  }
  std::uint32_t log2_max_frame_num_minus4 = 0;
  std::uint32_t pic_order_count_type = 0;
  if (!reader.read_ue(log2_max_frame_num_minus4) || log2_max_frame_num_minus4 > 12U ||
      !reader.read_ue(pic_order_count_type) || pic_order_count_type > 2U) {
    result.reason = "sps_invalid_frame_numbering";
    return result;
  }
  if (pic_order_count_type == 0U) {
    std::uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;
    if (!reader.read_ue(log2_max_pic_order_cnt_lsb_minus4) ||
        log2_max_pic_order_cnt_lsb_minus4 > 12U) {
      result.reason = "sps_invalid_picture_order_count";
      return result;
    }
  } else if (pic_order_count_type == 1U) {
    bool ignored_flag = false;
    std::int32_t ignored_signed = 0;
    std::uint32_t cycle = 0;
    if (!reader.read_bit(ignored_flag) || !reader.read_se(ignored_signed) ||
        !reader.read_se(ignored_signed) || !reader.read_ue(cycle) || cycle > 255U) {
      result.reason = "sps_invalid_picture_order_cycle";
      return result;
    }
    for (std::uint32_t index = 0; index < cycle; ++index) {
      if (!reader.read_se(ignored_signed)) {
        result.reason = "sps_truncated_picture_order_cycle";
        return result;
      }
    }
  }
  std::uint32_t max_num_ref_frames = 0;
  bool gaps_allowed = false;
  std::uint32_t width_in_mbs_minus1 = 0;
  std::uint32_t height_in_map_units_minus1 = 0;
  if (!reader.read_ue(max_num_ref_frames) || max_num_ref_frames > 16U ||
      !reader.read_bit(gaps_allowed) || !reader.read_ue(width_in_mbs_minus1) ||
      width_in_mbs_minus1 > 1023U || !reader.read_ue(height_in_map_units_minus1) ||
      height_in_map_units_minus1 > 1023U || !reader.read_bit(result.info.frame_mbs_only)) {
    result.reason = "sps_invalid_geometry";
    return result;
  }
  if (!result.info.frame_mbs_only) {
    bool mb_adaptive_frame_field = false;
    if (!reader.read_bit(mb_adaptive_frame_field)) {
      result.reason = "sps_truncated_interlace_flag";
      return result;
    }
  }
  bool direct_8x8_inference = false;
  bool frame_cropping = false;
  if (!reader.read_bit(direct_8x8_inference) || !reader.read_bit(frame_cropping)) {
    result.reason = "sps_truncated_crop_flags";
    return result;
  }
  std::uint32_t crop_left = 0;
  std::uint32_t crop_right = 0;
  std::uint32_t crop_top = 0;
  std::uint32_t crop_bottom = 0;
  if (frame_cropping && (!reader.read_ue(crop_left) || !reader.read_ue(crop_right) ||
                         !reader.read_ue(crop_top) || !reader.read_ue(crop_bottom))) {
    result.reason = "sps_invalid_crop";
    return result;
  }
  if (!reader.read_bit(result.info.vui_present) ||
      (result.info.vui_present && !parse_vui(reader, result.info))) {
    result.reason = "sps_invalid_vui";
    return result;
  }

  const std::size_t frame_factor = result.info.frame_mbs_only ? 1U : 2U;
  result.info.coded_width = (static_cast<std::size_t>(width_in_mbs_minus1) + 1U) * 16U;
  result.info.coded_height =
      (static_cast<std::size_t>(height_in_map_units_minus1) + 1U) * 16U * frame_factor;
  const std::size_t chroma_array_type = separate_color_plane ? 0U : chroma_format_idc;
  const std::size_t crop_unit_x = chroma_array_type == 0U || chroma_array_type == 3U ? 1U : 2U;
  const std::size_t sub_height = chroma_array_type == 1U ? 2U : 1U;
  const std::size_t crop_unit_y = (chroma_array_type == 0U ? 1U : sub_height) * frame_factor;
  const auto horizontal_crop =
      (static_cast<std::size_t>(crop_left) + static_cast<std::size_t>(crop_right)) * crop_unit_x;
  const auto vertical_crop =
      (static_cast<std::size_t>(crop_top) + static_cast<std::size_t>(crop_bottom)) * crop_unit_y;
  if (horizontal_crop >= result.info.coded_width || vertical_crop >= result.info.coded_height) {
    result.reason = "sps_crop_exceeds_coded_geometry";
    return result;
  }
  result.info.visible_width = result.info.coded_width - horizontal_crop;
  result.info.visible_height = result.info.coded_height - vertical_crop;
  result.passed = true;
  result.reason = "sps_valid";
  return result;
}

} // namespace glyphrelay
