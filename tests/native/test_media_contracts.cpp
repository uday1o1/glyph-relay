#include "glyphrelay/annex_b.hpp"
#include "glyphrelay/h264_sps.hpp"
#include "glyphrelay/i420.hpp"
#include "glyphrelay/recording_profile.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

class BitWriter {
public:
  void bit(bool value) {
    if (bit_offset_ == 0U) {
      bytes_.push_back(0U);
    }
    if (value) {
      bytes_.back() |= static_cast<std::uint8_t>(1U << (7U - bit_offset_));
    }
    bit_offset_ = (bit_offset_ + 1U) % 8U;
  }

  void bits(std::uint32_t value, unsigned int count) {
    for (unsigned int index = count; index > 0U; --index) {
      bit(((value >> (index - 1U)) & 1U) != 0U);
    }
  }

  void ue(std::uint32_t value) {
    const std::uint32_t code = value + 1U;
    unsigned int bits_required = 0U;
    for (std::uint32_t remaining = code; remaining != 0U; remaining >>= 1U) {
      ++bits_required;
    }
    for (unsigned int index = 1U; index < bits_required; ++index) {
      bit(false);
    }
    bits(code, bits_required);
  }

  std::vector<std::uint8_t> finish_rbsp() {
    bit(true);
    while (bit_offset_ != 0U) {
      bit(false);
    }
    return bytes_;
  }

private:
  std::vector<std::uint8_t> bytes_;
  unsigned int bit_offset_ = 0U;
};

std::vector<std::uint8_t> escape_rbsp(const std::vector<std::uint8_t> &rbsp) {
  std::vector<std::uint8_t> ebsp;
  unsigned int zero_count = 0U;
  for (const auto byte : rbsp) {
    if (zero_count >= 2U && byte <= 3U) {
      ebsp.push_back(3U);
      zero_count = 0U;
    }
    ebsp.push_back(byte);
    zero_count = byte == 0U ? zero_count + 1U : 0U;
  }
  return ebsp;
}

std::vector<std::uint8_t> baseline_1080p_sps() {
  BitWriter writer;
  writer.bits(66U, 8U);
  writer.bits(0xC0U, 8U);
  writer.bits(40U, 8U);
  writer.ue(0U);
  writer.ue(0U);
  writer.ue(0U);
  writer.ue(0U);
  writer.ue(1U);
  writer.bit(false);
  writer.ue(119U);
  writer.ue(67U);
  writer.bit(true);
  writer.bit(true);
  writer.bit(true);
  writer.ue(0U);
  writer.ue(0U);
  writer.ue(0U);
  writer.ue(4U);
  writer.bit(true);
  writer.bit(true);
  writer.bits(1U, 8U);
  writer.bit(false);
  writer.bit(true);
  writer.bits(5U, 3U);
  writer.bit(false);
  writer.bit(true);
  writer.bits(1U, 8U);
  writer.bits(1U, 8U);
  writer.bits(1U, 8U);
  writer.bit(false);
  writer.bit(true);
  writer.bits(1U, 32U);
  writer.bits(60U, 32U);
  writer.bit(true);
  writer.bit(false);
  writer.bit(false);
  writer.bit(false);
  writer.bit(false);

  auto payload = escape_rbsp(writer.finish_rbsp());
  payload.insert(payload.begin(), 0x67U);
  return payload;
}

std::vector<std::uint8_t> valid_access_unit() {
  const auto sps = baseline_1080p_sps();
  std::vector<std::uint8_t> access_unit = {0U, 0U, 0U, 1U};
  access_unit.insert(access_unit.end(), sps.begin(), sps.end());
  const std::vector<std::uint8_t> pps = {0U, 0U, 1U, 0x68U, 0xCEU, 0x06U, 0xE2U};
  const std::vector<std::uint8_t> idr = {0U, 0U, 0U, 1U, 0x65U, 0x88U, 0x84U};
  access_unit.insert(access_unit.end(), pps.begin(), pps.end());
  access_unit.insert(access_unit.end(), idr.begin(), idr.end());
  return access_unit;
}

std::string offer(std::string profile_level_id, std::string packetization_mode = "1",
                  std::string asymmetry = "1") {
  return "v=0\r\n"
         "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
         "a=rtpmap:111 opus/48000/2\r\n"
         "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
         "a=rtpmap:96 VP8/90000\r\n"
         "a=rtpmap:97 H264/90000\r\n"
         "a=fmtp:97 profile-level-id=" +
         profile_level_id + ";packetization-mode=" + packetization_mode +
         ";level-asymmetry-allowed=" + asymmetry + "\r\n";
}

void test_annex_b_and_sps() {
  const auto bytes = valid_access_unit();
  const auto parsed = glyphrelay::parse_annex_b_access_unit(bytes);
  require(parsed.passed, "three-byte and four-byte Annex B start codes must parse");
  require(parsed.access_unit.nal_units.size() == 3U,
          "the access unit must preserve NAL boundaries");
  require(parsed.access_unit.starts_with_parameter_sets_and_idr(),
          "the recovery access unit must begin with SPS, PPS, and IDR");

  const auto sps =
      glyphrelay::parse_h264_sps(parsed.access_unit.payload(parsed.access_unit.nal_units[0]));
  require(sps.passed, "the constrained-baseline SPS fixture must parse");
  require(sps.info.profile_level.family == glyphrelay::H264ProfileFamily::constrained_baseline,
          "the SPS must classify constrained baseline semantically");
  require(sps.info.profile_level.level_idc == 40U, "the SPS must declare Level 4.0");
  require(sps.info.coded_width == 1920U && sps.info.coded_height == 1088U,
          "the SPS must preserve coded macroblock geometry");
  require(sps.info.visible_width == 1920U && sps.info.visible_height == 1080U,
          "the SPS crop must declare the visible geometry");
  require(sps.info.vui_present && sps.info.video_signal_present &&
              sps.info.color_description_present && !sps.info.full_range &&
              sps.info.color_primaries == 1U && sps.info.transfer_characteristics == 1U &&
              sps.info.matrix_coefficients == 1U,
          "the SPS must declare limited-range BT.709 VUI");
  require(glyphrelay::validate_recording_profile_sps(sps.info, 1920U, 1080U).compatible,
          "the SPS must pass the candidate recording profile");
  auto insufficient_level = sps.info;
  insufficient_level.profile_level.level_idc = 31U;
  require(!glyphrelay::validate_recording_profile_sps(insufficient_level, 1920U, 1080U).compatible,
          "a Level 3.1 SPS must not claim the 1080p presentation");

  require(!glyphrelay::parse_annex_b_access_unit({}).passed,
          "an empty Annex B access unit must be rejected");
  require(!glyphrelay::parse_annex_b_access_unit(std::vector<std::uint8_t>{0x65U, 0x80U}).passed,
          "an Annex B access unit without a start code must be rejected");
  require(
      !glyphrelay::parse_annex_b_access_unit(std::vector<std::uint8_t>{0U, 0U, 1U, 0xE5U, 0x80U})
           .passed,
      "a forbidden-zero-bit violation must be rejected");
  require(
      !glyphrelay::parse_annex_b_access_unit(std::vector<std::uint8_t>{0U, 0U, 1U, 0x78U, 0x80U})
           .passed,
      "an unsupported aggregation NAL type must be rejected");
  require(
      !glyphrelay::parse_annex_b_access_unit(std::vector<std::uint8_t>{0U, 0U, 1U, 0x65U}).passed,
      "a header-only NAL unit must be rejected");
  require(!glyphrelay::parse_annex_b_access_unit(
               std::vector<std::uint8_t>{0U, 0U, 1U, 0U, 0U, 1U, 0x65U, 0x80U})
               .passed,
          "an empty NAL unit between start codes must be rejected");
  require(
      !glyphrelay::parse_annex_b_access_unit(std::vector<std::uint8_t>{0U, 0U, 1U, 0x67U, 0x80U})
           .passed,
      "an access unit without a VCL NAL must be rejected");
  require(!glyphrelay::parse_h264_sps(std::vector<std::uint8_t>{0x67U, 0x42U}).passed,
          "a truncated SPS must be rejected");

  bool invalid_prevention_rejected = false;
  try {
    static_cast<void>(
        glyphrelay::annex_b_nal_to_rbsp(std::vector<std::uint8_t>{0x67U, 0U, 0U, 2U, 0x80U}));
  } catch (const std::invalid_argument &) {
    invalid_prevention_rejected = true;
  }
  require(invalid_prevention_rejected, "invalid emulation prevention must be rejected");
}

void test_profile_and_sdp() {
  require(glyphrelay::classify_h264_profile(66U, 0xC0U) ==
              glyphrelay::H264ProfileFamily::constrained_baseline,
          "42c0 must classify as constrained baseline");
  require(glyphrelay::classify_h264_profile(66U, 0xE0U) ==
              glyphrelay::H264ProfileFamily::constrained_baseline,
          "42e0 must classify as the same RFC 6184 subprofile");
  require(glyphrelay::evaluate_recording_profile_offer(offer("42e028")).compatible,
          "a Level 4.0 constrained-baseline browser offer must pass");
  require(glyphrelay::evaluate_recording_profile_offer(offer("42c028")).compatible,
          "equivalent constrained-baseline constraint bytes must pass");

  const auto level_31 = glyphrelay::evaluate_recording_profile_offer(offer("42e01f"));
  require(!level_31.compatible &&
              level_31.reason ==
                  "recording_profile_offer_lacks_level4_constrained_baseline_packetization1",
          "a Level 3.1 offer must fail the declared 1080p30 requirement");
  require(!glyphrelay::evaluate_recording_profile_offer(offer("42e028", "0")).compatible,
          "packetization mode zero must fail");
  require(!glyphrelay::evaluate_recording_profile_offer(offer("42e028", "1", "0")).compatible,
          "an offer without level asymmetry must fail");

  const std::string spoofed =
      "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "a=rtpmap:97 H264/90000\r\n"
      "a=fmtp:97 profile-level-id=42e028;packetization-mode=1;level-asymmetry-allowed=1\r\n";
  const auto spoofed_result = glyphrelay::evaluate_recording_profile_offer(spoofed);
  require(!spoofed_result.compatible &&
              spoofed_result.reason == "sdp_attribute_payload_not_advertised",
          "an H.264 attribute for an unadvertised payload must be rejected");

  const std::string duplicate =
      offer("42e028") +
      "a=fmtp:97 profile-level-id=42e028;packetization-mode=1;level-asymmetry-allowed=1\r\n";
  require(!glyphrelay::evaluate_recording_profile_offer(duplicate).compatible,
          "duplicate format parameters must fail closed");
  const auto candidate_hash = glyphrelay::recording_profile_candidate_sha256();
  require(candidate_hash == "7a8d11250e043c52b7089a375485bef3916414c6d81d6f1350f53dbcc56b04e1",
          "the candidate profile must keep its exact canonical identity");
}

void test_nv12_to_i420() {
  const std::vector<std::uint8_t> nv12 = {
      1U,  2U,  3U,  4U,  90U, 90U, 5U,  6U,  7U,  8U,  90U, 90U, 9U,  10U, 11U, 12U, 90U, 90U,
      13U, 14U, 15U, 16U, 90U, 90U, 21U, 31U, 22U, 32U, 90U, 90U, 23U, 33U, 24U, 34U, 90U, 90U,
  };
  const auto frame = glyphrelay::nv12_to_i420(nv12, 4U, 4U, 6U, 6U, 4U, 4U);
  const std::vector<std::uint8_t> expected_y = {1U, 2U,  3U,  4U,  5U,  6U,  7U,  8U,
                                                9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U};
  const std::vector<std::uint8_t> expected_u = {21U, 22U, 23U, 24U};
  const std::vector<std::uint8_t> expected_v = {31U, 32U, 33U, 34U};
  require(std::ranges::equal(frame.y_plane(), expected_y),
          "NV12 luma planarization must exclude row padding");
  require(std::ranges::equal(frame.u_plane(), expected_u), "NV12 U samples must planarize exactly");
  require(std::ranges::equal(frame.v_plane(), expected_v), "NV12 V samples must planarize exactly");

  bool invalid_stride_rejected = false;
  try {
    static_cast<void>(glyphrelay::nv12_to_i420(nv12, 4U, 4U, 3U, 6U, 4U, 4U));
  } catch (const std::invalid_argument &) {
    invalid_stride_rejected = true;
  }
  require(invalid_stride_rejected, "an undersized NV12 stride must be rejected");
}

} // namespace

int main() {
  test_annex_b_and_sps();
  test_profile_and_sdp();
  test_nv12_to_i420();
  return 0;
}
