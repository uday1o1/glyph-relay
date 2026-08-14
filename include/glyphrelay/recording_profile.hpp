#pragma once

#include "glyphrelay/h264_sps.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace glyphrelay {

struct PresentationProfile {
  std::string name;
  std::size_t width;
  std::size_t height;
  unsigned int frames_per_second;
  std::uint8_t minimum_h264_level_idc;
};

struct H264OfferFormat {
  unsigned int payload_type = 0;
  H264ProfileLevelId profile_level;
  unsigned int packetization_mode = 0;
  bool level_asymmetry_allowed = false;
};

struct RecordingProfileCompatibility {
  bool compatible = false;
  std::string reason;
  std::vector<H264OfferFormat> formats;
};

const std::vector<PresentationProfile> &recording_profile_presentations();
const std::vector<PresentationProfile> &sharing_profile_presentations();
std::string recording_profile_candidate_canonical_json();
std::string recording_profile_candidate_sha256();
RecordingProfileCompatibility evaluate_recording_profile_offer(std::string_view sdp,
                                                               std::string_view presentation_name);
RecordingProfileCompatibility validate_recording_profile_sps(const H264SpsInfo &sps,
                                                             std::size_t expected_width,
                                                             std::size_t expected_height);

} // namespace glyphrelay
