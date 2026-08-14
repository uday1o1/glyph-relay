#include "glyphrelay/recording_profile.hpp"

#include "glyphrelay/sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace glyphrelay {
namespace {

constexpr std::size_t kMaximumSdpBytes = 64U * 1024U;
constexpr std::size_t kMaximumSdpLineBytes = 2048U;

std::string lower_ascii(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                          : character);
  }
  return result;
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1U);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1U);
  }
  return value;
}

std::optional<unsigned int> parse_unsigned(std::string_view value, unsigned int maximum) {
  unsigned int result = 0;
  const auto conversion = std::from_chars(value.data(), value.data() + value.size(), result);
  if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() ||
      result > maximum) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::uint8_t> parse_hex_byte(std::string_view value) {
  if (value.size() != 2U) {
    return std::nullopt;
  }
  unsigned int result = 0;
  const auto conversion = std::from_chars(value.data(), value.data() + value.size(), result, 16);
  if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() ||
      result > 255U) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(result);
}

std::vector<std::string_view> split(std::string_view value, char separator) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const auto position = value.find(separator, start);
    fields.push_back(value.substr(start, position == std::string_view::npos ? value.size() - start
                                                                            : position - start));
    if (position == std::string_view::npos) {
      return fields;
    }
    start = position + 1U;
  }
}

std::string parse_offer(std::string_view sdp, std::vector<H264OfferFormat> &formats) {
  if (sdp.empty() || sdp.size() > kMaximumSdpBytes || sdp.find('\0') != std::string_view::npos) {
    return "sdp_size_or_encoding_invalid";
  }
  std::map<unsigned int, std::string> codecs;
  std::map<unsigned int, std::map<std::string, std::string>> parameters;
  std::set<unsigned int> advertised_video_payloads;
  std::set<unsigned int> current_video_payloads;
  bool in_video_section = false;
  bool saw_video_section = false;
  for (auto raw_line : split(sdp, '\n')) {
    if (!raw_line.empty() && raw_line.back() == '\r') {
      raw_line.remove_suffix(1U);
    }
    if (raw_line.size() > kMaximumSdpLineBytes) {
      return "sdp_line_too_large";
    }
    if (raw_line.starts_with("m=")) {
      in_video_section = raw_line.starts_with("m=video ");
      saw_video_section = saw_video_section || in_video_section;
      current_video_payloads.clear();
      if (in_video_section) {
        const auto fields = split(raw_line, ' ');
        if (fields.size() < 4U) {
          return "sdp_video_media_line_malformed";
        }
        for (std::size_t index = 3U; index < fields.size(); ++index) {
          const auto payload = parse_unsigned(fields[index], 127U);
          if (!payload || !current_video_payloads.insert(*payload).second) {
            return "sdp_video_payload_list_invalid";
          }
          advertised_video_payloads.insert(*payload);
        }
      }
      continue;
    }
    if (!in_video_section) {
      continue;
    }
    if (raw_line.starts_with("a=rtpmap:")) {
      const auto body = raw_line.substr(std::string_view("a=rtpmap:").size());
      const auto space = body.find(' ');
      if (space == std::string_view::npos) {
        return "sdp_rtpmap_malformed";
      }
      const auto payload = parse_unsigned(body.substr(0, space), 127U);
      const auto encoding = lower_ascii(body.substr(space + 1U));
      if (!payload || !current_video_payloads.contains(*payload)) {
        return "sdp_attribute_payload_not_advertised";
      }
      if (!codecs.emplace(*payload, encoding).second) {
        return "sdp_rtpmap_duplicate_or_invalid";
      }
    } else if (raw_line.starts_with("a=fmtp:")) {
      const auto body = raw_line.substr(std::string_view("a=fmtp:").size());
      const auto space = body.find(' ');
      if (space == std::string_view::npos) {
        return "sdp_fmtp_malformed";
      }
      const auto payload = parse_unsigned(body.substr(0, space), 127U);
      if (!payload || !current_video_payloads.contains(*payload)) {
        return "sdp_attribute_payload_not_advertised";
      }
      if (parameters.contains(*payload)) {
        return "sdp_fmtp_duplicate_or_invalid";
      }
      std::map<std::string, std::string> values;
      for (auto item : split(body.substr(space + 1U), ';')) {
        item = trim(item);
        const auto equals = item.find('=');
        if (equals == std::string_view::npos) {
          return "sdp_fmtp_parameter_malformed";
        }
        const auto name = lower_ascii(trim(item.substr(0, equals)));
        const auto value = lower_ascii(trim(item.substr(equals + 1U)));
        if (name.empty() || value.empty() || !values.emplace(name, value).second) {
          return "sdp_fmtp_parameter_duplicate_or_empty";
        }
      }
      parameters.emplace(*payload, std::move(values));
    }
  }
  if (!saw_video_section) {
    return "sdp_video_section_missing";
  }
  if (advertised_video_payloads.empty()) {
    return "sdp_video_payload_list_invalid";
  }
  for (const auto &[payload, codec] : codecs) {
    if (codec != "h264/90000") {
      continue;
    }
    const auto parameter_iterator = parameters.find(payload);
    if (parameter_iterator == parameters.end()) {
      continue;
    }
    const auto &values = parameter_iterator->second;
    const auto profile = values.find("profile-level-id");
    const auto packetization = values.find("packetization-mode");
    const auto asymmetry = values.find("level-asymmetry-allowed");
    if (profile == values.end() || profile->second.size() != 6U || packetization == values.end() ||
        asymmetry == values.end()) {
      continue;
    }
    const auto profile_idc = parse_hex_byte(std::string_view(profile->second).substr(0U, 2U));
    const auto profile_iop = parse_hex_byte(std::string_view(profile->second).substr(2U, 2U));
    const auto level_idc = parse_hex_byte(std::string_view(profile->second).substr(4U, 2U));
    const auto packetization_mode = parse_unsigned(packetization->second, 2U);
    if (!profile_idc || !profile_iop || !level_idc || !packetization_mode ||
        (asymmetry->second != "0" && asymmetry->second != "1")) {
      continue;
    }
    formats.push_back({payload,
                       {*profile_idc, *profile_iop, *level_idc,
                        classify_h264_profile(*profile_idc, *profile_iop)},
                       *packetization_mode,
                       asymmetry->second == "1"});
  }
  return formats.empty() ? "sdp_no_explicit_h264_format" : "sdp_h264_formats_parsed";
}

} // namespace

const std::vector<PresentationProfile> &recording_profile_presentations() {
  static const std::vector<PresentationProfile> profiles = {
      {"1080p30", 1920, 1080, 30, 40},
      {"1080p24", 1920, 1080, 24, 40},
      {"720p24", 1280, 720, 24, 31},
      {"720p15", 1280, 720, 15, 31},
  };
  return profiles;
}

std::string recording_profile_candidate_canonical_json() {
  return "{\"schema_version\":1,\"name\":\"recording_profile_candidate_v1\","
         "\"profile_family\":\"constrained_baseline\",\"maximum_level_idc\":40,"
         "\"packetization_mode\":1,\"level_asymmetry_allowed_required\":true,"
         "\"pixel_format\":\"8_bit_420\",\"nvenc_layout\":\"nv12\","
         "\"openh264_layout\":\"i420\",\"color_primaries\":\"bt709\","
         "\"transfer_characteristics\":\"bt709\",\"matrix_coefficients\":\"bt709\","
         "\"full_range\":false,\"b_frames\":0,\"gop_frames_maximum\":60,"
         "\"parameter_sets\":\"startup_and_every_idr\","
         "\"presentations\":[[\"1080p30\",1920,1080,30,40],"
         "[\"1080p24\",1920,1080,24,40],[\"720p24\",1280,720,24,31],"
         "[\"720p15\",1280,720,15,31]]}";
}

std::string recording_profile_candidate_sha256() {
  return sha256_hex(recording_profile_candidate_canonical_json());
}

RecordingProfileCompatibility evaluate_recording_profile_offer(std::string_view sdp) {
  RecordingProfileCompatibility result;
  result.reason = parse_offer(sdp, result.formats);
  if (result.formats.empty()) {
    return result;
  }
  const auto required_level =
      std::max_element(recording_profile_presentations().begin(),
                       recording_profile_presentations().end(),
                       [](const PresentationProfile &left, const PresentationProfile &right) {
                         return left.minimum_h264_level_idc < right.minimum_h264_level_idc;
                       })
          ->minimum_h264_level_idc;
  for (const auto &format : result.formats) {
    if (format.profile_level.family == H264ProfileFamily::constrained_baseline &&
        format.profile_level.level_idc >= required_level && format.packetization_mode == 1U &&
        format.level_asymmetry_allowed) {
      result.compatible = true;
      result.reason = "recording_profile_offer_compatible";
      return result;
    }
  }
  result.reason = "recording_profile_offer_lacks_level4_constrained_baseline_packetization1";
  return result;
}

RecordingProfileCompatibility validate_recording_profile_sps(const H264SpsInfo &sps,
                                                             std::size_t expected_width,
                                                             std::size_t expected_height) {
  RecordingProfileCompatibility result;
  const auto presentation = std::find_if(
      recording_profile_presentations().begin(), recording_profile_presentations().end(),
      [expected_width, expected_height](const PresentationProfile &profile) {
        return profile.width == expected_width && profile.height == expected_height;
      });
  if (sps.profile_level.family != H264ProfileFamily::constrained_baseline) {
    result.reason = "sps_profile_not_constrained_baseline";
  } else if (presentation == recording_profile_presentations().end()) {
    result.reason = "sps_presentation_not_in_recording_profile";
  } else if (sps.profile_level.level_idc < presentation->minimum_h264_level_idc) {
    result.reason = "sps_level_insufficient_for_presentation";
  } else if (sps.profile_level.level_idc > 40U) {
    result.reason = "sps_level_exceeds_recording_profile";
  } else if (sps.visible_width != expected_width || sps.visible_height != expected_height) {
    result.reason = "sps_visible_geometry_mismatch";
  } else if (!sps.frame_mbs_only) {
    result.reason = "sps_interlacing_not_supported";
  } else if (!sps.vui_present || !sps.video_signal_present || !sps.color_description_present) {
    result.reason = "sps_bt709_vui_required";
  } else if (sps.full_range || sps.color_primaries != 1U || sps.transfer_characteristics != 1U ||
             sps.matrix_coefficients != 1U) {
    result.reason = "sps_color_contract_mismatch";
  } else {
    result.compatible = true;
    result.reason = "recording_profile_sps_compatible";
  }
  return result;
}

} // namespace glyphrelay
