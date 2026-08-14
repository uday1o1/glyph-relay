#include "glyphrelay/annex_b.hpp"
#include "glyphrelay/h264_sps.hpp"
#include "glyphrelay/media_egress.hpp"
#include "glyphrelay/recording_profile.hpp"
#include "glyphrelay_media_handlers.hpp"

#include <rtc/configuration.hpp>
#include <rtc/datachannel.hpp>
#include <rtc/description.hpp>
#include <rtc/message.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/track.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kPresentation = "720p30";
constexpr std::uint32_t kMediaSsrc = 0x47524D30U;
constexpr std::uint64_t kInitialExtendedSequence = 65'534U;
constexpr std::uint64_t kInitialExtendedTimestamp = (std::uint64_t{1U} << 32U) - 3'000U;
constexpr std::uint64_t kMediaEpoch = 1U;
constexpr std::uint64_t kInitialDependencyEpoch = 1U;
constexpr std::size_t kMaximumOfferBytes = 64U * 1024U;
constexpr std::size_t kMaximumFrameCount = 10'000U;
constexpr std::size_t kMaximumAccessUnitBytes = 16U * 1024U * 1024U;
constexpr std::chrono::seconds kSignalingTimeout{15};
constexpr std::chrono::seconds kConnectionTimeout{30};

std::atomic_bool stop_requested = false;

void request_stop(int) { stop_requested.store(true); }

class SenderError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct Arguments {
  std::filesystem::path offer;
  std::filesystem::path stream;
  std::filesystem::path frames;
  std::filesystem::path answer;
  std::filesystem::path summary;
  std::size_t start_frame = 300U;
  std::size_t frame_count = 1'800U;
  unsigned int frames_per_second = 30U;
  std::optional<std::uint64_t> fault_loss_extended_sequence;
  std::optional<std::size_t> inject_pli_after_frame;
};

struct FrameSlice {
  std::size_t frame_index = 0U;
  std::uint64_t offset = 0U;
  std::size_t bytes = 0U;
};

struct SenderState {
  std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<rtc::Track> track;
  std::shared_ptr<rtc::DataChannel> control_channel;
  std::shared_ptr<glyphrelay::rtc_adapter::StrictH264Packetizer> packetizer;
  std::shared_ptr<glyphrelay::rtc_adapter::BoundedNackResponder> recovery;
  std::shared_ptr<rtc::RtpPacketizationConfig> rtp_configuration;
  std::string answer_sdp;
  std::string failure;
  bool gathering_complete = false;
  bool connected = false;
  bool track_open = false;
  bool control_open = false;
};

std::string read_bounded_text(const std::filesystem::path &path, std::size_t maximum_bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw SenderError("input_open_failed:" + path.string());
  }
  input.seekg(0, std::ios::end);
  const auto end = input.tellg();
  if (end <= 0 || static_cast<std::uintmax_t>(end) > maximum_bytes) {
    throw SenderError("input_size_invalid:" + path.string());
  }
  std::string value(static_cast<std::size_t>(end), '\0');
  input.seekg(0, std::ios::beg);
  input.read(value.data(), static_cast<std::streamsize>(value.size()));
  if (!input || value.find('\0') != std::string::npos) {
    throw SenderError("input_read_or_encoding_invalid:" + path.string());
  }
  return value;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t start = 0U;
  while (true) {
    const auto separator = line.find('\t', start);
    fields.push_back(line.substr(start, separator == std::string_view::npos ? line.size() - start
                                                                            : separator - start));
    if (separator == std::string_view::npos) {
      return fields;
    }
    start = separator + 1U;
  }
}

template <typename Integer> Integer parse_integer(std::string_view value, std::string_view reason) {
  Integer parsed{};
  const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (value.empty() || conversion.ec != std::errc{} ||
      conversion.ptr != value.data() + value.size()) {
    throw SenderError(std::string(reason));
  }
  return parsed;
}

std::vector<FrameSlice> read_frame_table(const std::filesystem::path &path,
                                         const std::filesystem::path &stream_path) {
  std::ifstream input(path);
  if (!input) {
    throw SenderError("frame_table_open_failed");
  }
  std::string line;
  if (!std::getline(input, line) ||
      line != "frame_index\tbytes\tlatency_ms\tpending_count\toldest_pending_ms") {
    throw SenderError("frame_table_header_invalid");
  }
  std::vector<FrameSlice> frames;
  std::uint64_t offset = 0U;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto fields = split_tabs(line);
    if (fields.size() != 5U || frames.size() >= kMaximumFrameCount) {
      throw SenderError("frame_table_row_invalid");
    }
    const auto index = parse_integer<std::size_t>(fields[0], "frame_table_index_invalid");
    const auto bytes = parse_integer<std::size_t>(fields[1], "frame_table_size_invalid");
    if (index != frames.size() || bytes == 0U || bytes > kMaximumAccessUnitBytes ||
        offset > std::numeric_limits<std::uint64_t>::max() - bytes) {
      throw SenderError("frame_table_identity_or_size_invalid");
    }
    frames.push_back({index, offset, bytes});
    offset += bytes;
  }
  if (!input.eof() || frames.empty()) {
    throw SenderError("frame_table_read_failed");
  }
  std::error_code error;
  const auto stream_bytes = std::filesystem::file_size(stream_path, error);
  if (error || stream_bytes != offset) {
    throw SenderError("stream_size_does_not_match_frame_table");
  }
  return frames;
}

std::vector<std::uint8_t> read_access_unit(std::ifstream &stream, const FrameSlice &frame) {
  std::vector<std::uint8_t> bytes(frame.bytes);
  stream.clear();
  stream.seekg(static_cast<std::streamoff>(frame.offset), std::ios::beg);
  stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!stream) {
    throw SenderError("stream_access_unit_read_failed");
  }
  return bytes;
}

void validate_first_access_unit(std::span<const std::uint8_t> bytes) {
  const auto parsed = glyphrelay::parse_annex_b_access_unit(bytes);
  if (!parsed.passed || !parsed.access_unit.starts_with_parameter_sets_and_idr()) {
    throw SenderError("sharing_stream_must_start_with_sps_pps_idr");
  }
  const auto sps =
      std::find_if(parsed.access_unit.nal_units.begin(), parsed.access_unit.nal_units.end(),
                   [](const glyphrelay::AnnexBNalUnit &unit) { return unit.unit_type == 7U; });
  if (sps == parsed.access_unit.nal_units.end()) {
    throw SenderError("sharing_stream_sps_missing");
  }
  const auto information = glyphrelay::parse_h264_sps(parsed.access_unit.payload(*sps));
  if (!information.passed) {
    throw SenderError("sharing_stream_sps_invalid:" + information.reason);
  }
  const auto compatibility =
      glyphrelay::validate_recording_profile_sps(information.info, 1280U, 720U);
  if (!compatibility.compatible || information.info.profile_level.level_idc != 31U) {
    throw SenderError("sharing_stream_not_720p_level_3_1");
  }
}

void write_all(int descriptor, std::string_view value) {
  std::size_t offset = 0U;
  while (offset < value.size()) {
    const auto written = ::write(descriptor, value.data() + offset, value.size() - offset);
    if (written <= 0) {
      throw SenderError("evidence_file_write_failed");
    }
    offset += static_cast<std::size_t>(written);
  }
}

void write_exclusive_file(const std::filesystem::path &path, std::string_view value) {
  if (path.empty() || path.filename().empty()) {
    throw SenderError("evidence_output_path_invalid");
  }
  std::error_code error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      throw SenderError("evidence_output_directory_failed");
    }
  }
  auto temporary = path;
  temporary += ".tmp";
  const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    throw SenderError("evidence_output_exists_or_open_failed:" + temporary.string());
  }
  try {
    write_all(descriptor, value);
    if (::fsync(descriptor) != 0 || ::close(descriptor) != 0) {
      throw SenderError("evidence_output_sync_failed");
    }
  } catch (...) {
    static_cast<void>(::close(descriptor));
    std::filesystem::remove(temporary, error);
    throw;
  }
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(temporary, error);
    throw SenderError("evidence_output_exists:" + path.string());
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    throw SenderError("evidence_output_rename_failed");
  }
}

glyphrelay::DatagramProtocol map_protocol(rtc::FinalUdpDatagramProtocol protocol) {
  switch (protocol) {
  case rtc::FinalUdpDatagramProtocol::Srtp:
    return glyphrelay::DatagramProtocol::srtp;
  case rtc::FinalUdpDatagramProtocol::Srtcp:
    return glyphrelay::DatagramProtocol::srtcp;
  case rtc::FinalUdpDatagramProtocol::Dtls:
    return glyphrelay::DatagramProtocol::dtls;
  case rtc::FinalUdpDatagramProtocol::Stun:
    return glyphrelay::DatagramProtocol::stun;
  case rtc::FinalUdpDatagramProtocol::TurnChannelData:
    return glyphrelay::DatagramProtocol::turn_channel_data;
  case rtc::FinalUdpDatagramProtocol::TurnSendIndication:
    return glyphrelay::DatagramProtocol::turn_send_indication;
  case rtc::FinalUdpDatagramProtocol::TurnControl:
    return glyphrelay::DatagramProtocol::turn_control;
  case rtc::FinalUdpDatagramProtocol::UnknownControl:
    return glyphrelay::DatagramProtocol::unknown_control;
  }
  throw SenderError("final_udp_protocol_unknown");
}

class FinalEgressBridge {
public:
  explicit FinalEgressBridge(std::optional<std::uint64_t> fault_sequence)
      : fault_sequence_(fault_sequence) {}

  int send(const rtc::byte *data, std::size_t size, rtc::FinalUdpEgressClass egress_class,
           rtc::FinalUdpDatagramPath path, rtc::FinalUdpDatagramProtocol protocol,
           rtc::FinalUdpIpFamily family, rtc::final_udp_native_send native_send,
           void *native_send_pointer) {
    if (data == nullptr || native_send == nullptr) {
      return -1;
    }
    const auto datagram = std::span(data, size);
    if (fault_sequence_ && egress_class == rtc::FinalUdpEgressClass::Media &&
        path == rtc::FinalUdpDatagramPath::Direct &&
        protocol == rtc::FinalUdpDatagramProtocol::Srtp && size >= 4U) {
      const auto wire_sequence =
          static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(data[2]) << 8U) |
                                     std::to_integer<std::uint16_t>(data[3]));
      if (wire_sequence == static_cast<std::uint16_t>(*fault_sequence_ & 0xFFFFU)) {
        const std::scoped_lock lock(fault_mutex_);
        if (!suppressed_) {
          protected_original_.assign(data, data + size);
          suppressed_ = true;
          return static_cast<int>(size);
        }
        if (!protected_retransmission_observed_) {
          protected_retransmission_observed_ = true;
          protected_retransmission_identical_ =
              protected_original_.size() == size &&
              std::equal(protected_original_.begin(), protected_original_.end(), data);
        }
      }
    }

    glyphrelay::FinalDatagramMetadata metadata{
        .classification = egress_class == rtc::FinalUdpEgressClass::Media
                              ? glyphrelay::DatagramClass::media
                              : glyphrelay::DatagramClass::control,
        .path = path == rtc::FinalUdpDatagramPath::Direct ? glyphrelay::DatagramPath::direct_udp
                                                          : glyphrelay::DatagramPath::turn_udp,
        .ip_family = family == rtc::FinalUdpIpFamily::Ipv4 ? glyphrelay::DatagramIpFamily::ipv4
                                                           : glyphrelay::DatagramIpFamily::ipv6,
        .provenance = provenance(egress_class, protocol),
        .protocol = map_protocol(protocol),
        .media_epoch = egress_class == rtc::FinalUdpEgressClass::Media ? kMediaEpoch : 0U,
    };
    const auto result =
        gate_.send_final(std::as_bytes(datagram), metadata, [native_send, native_send_pointer]() {
          return static_cast<std::ptrdiff_t>(native_send(native_send_pointer));
        });
    return result.native_result < static_cast<std::ptrdiff_t>(std::numeric_limits<int>::min()) ||
                   result.native_result >
                       static_cast<std::ptrdiff_t>(std::numeric_limits<int>::max())
               ? -1
               : static_cast<int>(result.native_result);
  }

  glyphrelay::DatagramEgressSnapshot snapshot() const { return gate_.snapshot(); }

  bool fault_suppressed() const {
    const std::scoped_lock lock(fault_mutex_);
    return suppressed_;
  }

  bool protected_retransmission_observed() const {
    const std::scoped_lock lock(fault_mutex_);
    return protected_retransmission_observed_;
  }

  bool protected_retransmission_identical() const {
    const std::scoped_lock lock(fault_mutex_);
    return protected_retransmission_identical_;
  }

  void stop() { static_cast<void>(gate_.close_media(glyphrelay::MediaBoundaryReason::stop)); }

private:
  static glyphrelay::DatagramProvenance provenance(rtc::FinalUdpEgressClass egress_class,
                                                   rtc::FinalUdpDatagramProtocol protocol) {
    if (egress_class == rtc::FinalUdpEgressClass::Media) {
      return glyphrelay::DatagramProvenance::libdatachannel_media;
    }
    if (protocol == rtc::FinalUdpDatagramProtocol::Stun ||
        protocol == rtc::FinalUdpDatagramProtocol::TurnControl ||
        protocol == rtc::FinalUdpDatagramProtocol::UnknownControl) {
      return glyphrelay::DatagramProvenance::libjuice_generated_control;
    }
    return glyphrelay::DatagramProvenance::libdatachannel_control;
  }

  glyphrelay::MediaEgressGate gate_{kMediaEpoch};
  std::optional<std::uint64_t> fault_sequence_;
  mutable std::mutex fault_mutex_;
  std::vector<rtc::byte> protected_original_;
  bool suppressed_ = false;
  bool protected_retransmission_observed_ = false;
  bool protected_retransmission_identical_ = false;
};

std::uint8_t select_payload_type(const glyphrelay::RecordingProfileCompatibility &offer) {
  const auto selected =
      std::find_if(offer.formats.begin(), offer.formats.end(), [](const auto &format) {
        return format.profile_level.family == glyphrelay::H264ProfileFamily::constrained_baseline &&
               format.profile_level.level_idc >= 31U && format.packetization_mode == 1U &&
               format.level_asymmetry_allowed;
      });
  if (selected == offer.formats.end() || selected->payload_type > 127U) {
    throw SenderError("offer_selected_payload_type_invalid");
  }
  return static_cast<std::uint8_t>(selected->payload_type);
}

void configure_track_description(rtc::Track &track, std::uint8_t payload_type) {
  auto media = track.description();
  if (media.type() != "video" || media.direction() != rtc::Description::Direction::SendOnly) {
    throw SenderError("remote_offer_track_direction_invalid");
  }
  const auto payloads = media.payloadTypes();
  if (std::find(payloads.begin(), payloads.end(), payload_type) == payloads.end()) {
    throw SenderError("selected_payload_missing_from_track");
  }
  for (const auto payload : payloads) {
    if (payload != payload_type) {
      media.removeRtpMap(payload);
    }
  }
  media.clearSSRCs();
  media.addSSRC(kMediaSsrc, "glyphrelay-m0", "glyphrelay-m0-stream", "glyphrelay-m0-video");
  track.setDescription(std::move(media));
}

void set_failure(SenderState &state, std::string reason) {
  const std::scoped_lock lock(state.mutex);
  if (state.failure.empty()) {
    state.failure = std::move(reason);
  }
  state.changed.notify_all();
}

template <typename Predicate>
void wait_for(SenderState &state, std::chrono::seconds timeout, std::string_view timeout_reason,
              Predicate predicate) {
  std::unique_lock lock(state.mutex);
  const bool ready = state.changed.wait_for(lock, timeout, [&] {
    return predicate(state) || !state.failure.empty() || stop_requested.load();
  });
  if (!state.failure.empty()) {
    throw SenderError(state.failure);
  }
  if (!ready || stop_requested.load() || !predicate(state)) {
    throw SenderError(std::string(timeout_reason));
  }
}

std::string summary_json(const Arguments &arguments, const SenderState &state,
                         const FinalEgressBridge &egress, std::size_t sent_frames,
                         std::size_t recovery_frames, std::uint64_t next_extended_sequence,
                         std::uint64_t last_extended_timestamp) {
  const auto snapshot = egress.snapshot();
  const auto diagnostics = state.recovery->diagnostics();
  const auto cache = state.recovery->cache_snapshot();
  std::ostringstream output;
  output << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"protocol\": \"glyphrelay-m0-webrtc-sender-v1\",\n"
         << "  \"status\": \"PASSED\",\n"
         << "  \"presentation\": \"" << kPresentation << "\",\n"
         << "  \"start_frame\": " << arguments.start_frame << ",\n"
         << "  \"requested_frames\": " << arguments.frame_count << ",\n"
         << "  \"sent_frames\": " << sent_frames << ",\n"
         << "  \"recovery_frames\": " << recovery_frames << ",\n"
         << "  \"initial_extended_sequence\": " << kInitialExtendedSequence << ",\n"
         << "  \"next_extended_sequence\": " << next_extended_sequence << ",\n"
         << "  \"last_extended_timestamp\": " << last_extended_timestamp << ",\n"
         << "  \"wire_datagrams\": " << snapshot.datagrams << ",\n"
         << "  \"wire_ip_total_bytes\": " << snapshot.ip_total_bytes << ",\n"
         << "  \"wire_media_ip_total_bytes\": " << snapshot.media_ip_total_bytes << ",\n"
         << "  \"wire_control_ip_total_bytes\": " << snapshot.control_ip_total_bytes << ",\n"
         << "  \"wire_direct_ip_total_bytes\": " << snapshot.direct_ip_total_bytes << ",\n"
         << "  \"wire_turn_ip_total_bytes\": " << snapshot.turn_ip_total_bytes << ",\n"
         << "  \"rejected_datagrams\": " << snapshot.rejected_datagrams << ",\n"
         << "  \"failed_datagrams\": " << snapshot.failed_datagrams << ",\n"
         << "  \"short_datagrams\": " << snapshot.short_datagrams << ",\n"
         << "  \"accounting_overflowed\": " << (snapshot.overflowed ? "true" : "false") << ",\n"
         << "  \"feedback_messages\": " << diagnostics.feedback_messages << ",\n"
         << "  \"distinct_nack_identifiers\": " << diagnostics.distinct_nack_identifiers << ",\n"
         << "  \"idr_requests\": " << diagnostics.idr_requests << ",\n"
         << "  \"cache_retransmissions\": " << cache.retransmissions << ",\n"
         << "  \"inject_pli_after_frame\": ";
  if (arguments.inject_pli_after_frame) {
    output << *arguments.inject_pli_after_frame;
  } else {
    output << "null";
  }
  output << ",\n"
         << "  \"fault_loss_extended_sequence\": ";
  if (arguments.fault_loss_extended_sequence) {
    output << *arguments.fault_loss_extended_sequence;
  } else {
    output << "null";
  }
  output << ",\n"
         << "  \"fault_datagram_suppressed\": " << (egress.fault_suppressed() ? "true" : "false")
         << ",\n"
         << "  \"protected_retransmission_observed\": "
         << (egress.protected_retransmission_observed() ? "true" : "false") << ",\n"
         << "  \"protected_retransmission_identical\": "
         << (egress.protected_retransmission_identical() ? "true" : "false") << "\n"
         << "}\n";
  return output.str();
}

void print_help() {
  std::cout << "Usage: glyphrelay_m0_webrtc_sender --offer FILE --stream FILE --frames FILE "
               "--answer FILE --summary FILE [--start-frame N] [--frame-count N] [--fps N] "
               "[--inject-pli-after-frame N] [--fault-loss-extended-sequence N]\n";
}

Arguments parse_arguments(int argc, char **argv) {
  Arguments result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--help") {
      print_help();
      std::exit(0);
    }
    if (index + 1 >= argc) {
      throw SenderError("sender_argument_value_missing");
    }
    const std::string_view value(argv[++index]);
    if (option == "--offer" && result.offer.empty()) {
      result.offer = value;
    } else if (option == "--stream" && result.stream.empty()) {
      result.stream = value;
    } else if (option == "--frames" && result.frames.empty()) {
      result.frames = value;
    } else if (option == "--answer" && result.answer.empty()) {
      result.answer = value;
    } else if (option == "--summary" && result.summary.empty()) {
      result.summary = value;
    } else if (option == "--start-frame") {
      result.start_frame = parse_integer<std::size_t>(value, "start_frame_invalid");
    } else if (option == "--frame-count") {
      result.frame_count = parse_integer<std::size_t>(value, "frame_count_invalid");
    } else if (option == "--fps") {
      result.frames_per_second = parse_integer<unsigned int>(value, "fps_invalid");
    } else if (option == "--inject-pli-after-frame" && !result.inject_pli_after_frame) {
      result.inject_pli_after_frame =
          parse_integer<std::size_t>(value, "inject_pli_after_frame_invalid");
    } else if (option == "--fault-loss-extended-sequence" && !result.fault_loss_extended_sequence) {
      result.fault_loss_extended_sequence =
          parse_integer<std::uint64_t>(value, "fault_sequence_invalid");
    } else {
      throw SenderError("sender_argument_unknown_or_duplicate:" + std::string(option));
    }
  }
  if (result.offer.empty() || result.stream.empty() || result.frames.empty() ||
      result.answer.empty() || result.summary.empty() || result.frame_count == 0U ||
      result.frames_per_second == 0U || result.frames_per_second > 60U) {
    throw SenderError("sender_required_arguments_invalid");
  }
  if (result.answer == result.summary || std::filesystem::exists(result.answer) ||
      std::filesystem::exists(result.summary)) {
    throw SenderError("sender_output_exists_or_aliases");
  }
  if (result.inject_pli_after_frame && (*result.inject_pli_after_frame == 0U ||
                                        *result.inject_pli_after_frame >= result.frame_count)) {
    throw SenderError("inject_pli_after_frame_out_of_range");
  }
  if (result.fault_loss_extended_sequence && (*result.fault_loss_extended_sequence < 65'534U ||
                                              *result.fault_loss_extended_sequence > 65'536U)) {
    throw SenderError("fault_sequence_outside_rollover_fixture");
  }
  return result;
}

std::size_t find_next_recovery(std::ifstream &stream, std::span<const FrameSlice> frames,
                               std::size_t begin, std::size_t end) {
  for (std::size_t index = begin; index < end; ++index) {
    const auto bytes = read_access_unit(stream, frames[index]);
    const auto parsed = glyphrelay::parse_annex_b_access_unit(bytes);
    if (!parsed.passed) {
      throw SenderError("sharing_stream_access_unit_invalid:" + parsed.reason);
    }
    if (parsed.access_unit.starts_with_parameter_sets_and_idr()) {
      return index;
    }
  }
  throw SenderError("sharing_stream_recovery_idr_unavailable");
}

rtc::message_ptr picture_loss_indication() {
  auto message = rtc::make_message(12U, rtc::Message::Control);
  (*message)[0] = rtc::byte{0x81U};
  (*message)[1] = rtc::byte{206U};
  (*message)[2] = rtc::byte{0U};
  (*message)[3] = rtc::byte{2U};
  (*message)[4] = rtc::byte{0x11U};
  (*message)[5] = rtc::byte{0x22U};
  (*message)[6] = rtc::byte{0x33U};
  (*message)[7] = rtc::byte{0x44U};
  (*message)[8] = static_cast<rtc::byte>(kMediaSsrc >> 24U);
  (*message)[9] = static_cast<rtc::byte>((kMediaSsrc >> 16U) & 0xFFU);
  (*message)[10] = static_cast<rtc::byte>((kMediaSsrc >> 8U) & 0xFFU);
  (*message)[11] = static_cast<rtc::byte>(kMediaSsrc & 0xFFU);
  return message;
}

int run_sender(const Arguments &arguments) {
  const auto offer_sdp = read_bounded_text(arguments.offer, kMaximumOfferBytes);
  const auto compatibility = glyphrelay::evaluate_recording_profile_offer(offer_sdp, kPresentation);
  if (!compatibility.compatible) {
    throw SenderError("offer_incompatible:" + compatibility.reason);
  }
  const auto payload_type = select_payload_type(compatibility);
  const auto frames = read_frame_table(arguments.frames, arguments.stream);
  if (arguments.start_frame >= frames.size() ||
      arguments.frame_count > frames.size() - arguments.start_frame) {
    throw SenderError("selected_frame_range_invalid");
  }
  std::ifstream stream(arguments.stream, std::ios::binary);
  if (!stream) {
    throw SenderError("stream_open_failed");
  }
  validate_first_access_unit(read_access_unit(stream, frames[arguments.start_frame]));

  FinalEgressBridge egress(arguments.fault_loss_extended_sequence);
  SenderState state;
  std::atomic_bool recovery_requested = false;
  std::atomic_bool terminate_session = false;
  const auto sender_started = Clock::now();

  rtc::Configuration configuration;
  configuration.bindAddress = "127.0.0.1";
  configuration.enableIceTcp = false;
  configuration.enableIceUdpMux = false;
  configuration.disableAutoNegotiation = true;
  configuration.finalUdpSendCallback =
      [&egress](const rtc::byte *data, std::size_t size, rtc::FinalUdpEgressClass egress_class,
                rtc::FinalUdpDatagramPath path, rtc::FinalUdpDatagramProtocol protocol,
                rtc::FinalUdpIpFamily family, rtc::final_udp_native_send native_send,
                void *native_send_pointer) {
        return egress.send(data, size, egress_class, path, protocol, family, native_send,
                           native_send_pointer);
      };
  rtc::PeerConnection peer(configuration);
  peer.onStateChange([&state](rtc::PeerConnection::State connection_state) {
    {
      const std::scoped_lock lock(state.mutex);
      if (connection_state == rtc::PeerConnection::State::Connected) {
        state.connected = true;
      } else if ((connection_state == rtc::PeerConnection::State::Failed ||
                  connection_state == rtc::PeerConnection::State::Closed) &&
                 state.failure.empty()) {
        state.failure = "peer_connection_failed_or_closed";
      }
    }
    state.changed.notify_all();
  });
  peer.onGatheringStateChange([&state, &peer](rtc::PeerConnection::GatheringState gathering) {
    if (gathering != rtc::PeerConnection::GatheringState::Complete) {
      return;
    }
    const auto description = peer.localDescription();
    {
      const std::scoped_lock lock(state.mutex);
      if (!description) {
        state.failure = "local_answer_missing_after_gathering";
      } else {
        state.answer_sdp = static_cast<std::string>(*description);
        state.gathering_complete = true;
      }
    }
    state.changed.notify_all();
  });
  peer.onDataChannel([&state](std::shared_ptr<rtc::DataChannel> channel) {
    if (!channel || channel->label() != "glyphrelay-control-v1") {
      set_failure(state, "control_channel_label_invalid");
      return;
    }
    channel->onOpen([&state] {
      {
        const std::scoped_lock lock(state.mutex);
        state.control_open = true;
      }
      state.changed.notify_all();
    });
    channel->onMessage([](rtc::binary) {},
                       [&state](std::string message) {
                         if (message.size() > 4'096U) {
                           set_failure(state, "control_message_too_large");
                         }
                       });
    {
      const std::scoped_lock lock(state.mutex);
      state.control_channel = std::move(channel);
    }
    state.changed.notify_all();
  });
  peer.onTrack([&](std::shared_ptr<rtc::Track> track) {
    try {
      if (!track) {
        throw SenderError("remote_offer_track_missing");
      }
      configure_track_description(*track, payload_type);
      auto rtp_configuration = std::make_shared<rtc::RtpPacketizationConfig>(
          kMediaSsrc, "glyphrelay-m0", payload_type, 90'000U);
      rtp_configuration->setExtendedSequenceNumber(kInitialExtendedSequence);
      rtp_configuration->colorRange = 0U;
      rtp_configuration->colorPrimaries = 1U;
      rtp_configuration->colorTransfer = 1U;
      rtp_configuration->colorMatrix = 1U;
      auto packetizer =
          std::make_shared<glyphrelay::rtc_adapter::StrictH264Packetizer>(rtp_configuration);
      packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_configuration));
      auto recovery = std::make_shared<glyphrelay::rtc_adapter::BoundedNackResponder>(
          kMediaSsrc,
          [sender_started] {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sender_started)
                    .count());
          },
          [&recovery_requested] { recovery_requested.store(true); },
          [&terminate_session] {
            terminate_session.store(true);
            stop_requested.store(true);
          });
      packetizer->addToChain(recovery);
      track->setMediaHandler(packetizer);
      track->onOpen([&state] {
        {
          const std::scoped_lock lock(state.mutex);
          state.track_open = true;
        }
        state.changed.notify_all();
      });
      {
        const std::scoped_lock lock(state.mutex);
        if (state.track) {
          throw SenderError("multiple_video_tracks_not_supported");
        }
        state.track = std::move(track);
        state.packetizer = std::move(packetizer);
        state.recovery = std::move(recovery);
        state.rtp_configuration = std::move(rtp_configuration);
      }
      state.changed.notify_all();
    } catch (const std::exception &error) {
      set_failure(state, error.what());
    }
  });

  peer.setRemoteDescription(rtc::Description(offer_sdp, "offer"));
  {
    const std::scoped_lock lock(state.mutex);
    if (!state.failure.empty()) {
      throw SenderError(state.failure);
    }
    if (!state.track || !state.recovery) {
      throw SenderError("remote_offer_did_not_create_video_track");
    }
  }
  peer.setLocalDescription(rtc::Description::Type::Answer);
  wait_for(state, kSignalingTimeout, "answer_gathering_timeout",
           [](const SenderState &value) { return value.gathering_complete; });
  std::string answer_sdp;
  {
    const std::scoped_lock lock(state.mutex);
    answer_sdp = state.answer_sdp;
  }
  if (answer_sdp.find("a=candidate:") == std::string::npos ||
      answer_sdp.find(" 127.0.0.1 ") == std::string::npos ||
      answer_sdp.find("profile-level-id=42e01f") == std::string::npos) {
    throw SenderError("loopback_answer_profile_or_candidate_invalid");
  }
  write_exclusive_file(arguments.answer, answer_sdp);
  wait_for(state, kConnectionTimeout, "peer_connection_timeout",
           [](const SenderState &value) { return value.connected && value.track_open; });

  std::shared_ptr<rtc::Track> track;
  std::shared_ptr<glyphrelay::rtc_adapter::BoundedNackResponder> recovery;
  {
    const std::scoped_lock lock(state.mutex);
    track = state.track;
    recovery = state.recovery;
  }
  if (!recovery->begin_epoch(kMediaEpoch, kInitialDependencyEpoch,
                             glyphrelay::RecoveryTrigger::startup)) {
    throw SenderError("startup_recovery_epoch_not_admitted");
  }
  recovery_requested.store(false);

  const auto frame_interval =
      std::chrono::nanoseconds(1'000'000'000ULL / arguments.frames_per_second);
  std::uint64_t dependency_epoch = kInitialDependencyEpoch;
  std::uint64_t extended_timestamp = kInitialExtendedTimestamp;
  std::uint64_t access_unit_id = 0U;
  std::size_t sent_frames = 0U;
  std::size_t recovery_frames = 0U;
  bool pli_injected = false;
  std::size_t table_index = arguments.start_frame;
  const auto end_index = arguments.start_frame + arguments.frame_count;
  auto next_deadline = Clock::now();
  while (table_index < end_index && !stop_requested.load()) {
    if (arguments.inject_pli_after_frame && !pli_injected &&
        sent_frames == *arguments.inject_pli_after_frame) {
      rtc::message_vector feedback = {picture_loss_indication()};
      recovery->incoming(feedback, [](rtc::message_ptr) {
        throw SenderError("pli_injection_unexpectedly_emitted_packet");
      });
      pli_injected = true;
    }
    if (recovery_requested.exchange(false)) {
      table_index = find_next_recovery(stream, frames, table_index, end_index);
      ++dependency_epoch;
      if (!recovery->begin_epoch(kMediaEpoch, dependency_epoch,
                                 glyphrelay::RecoveryTrigger::dependency_epoch_transition)) {
        throw SenderError("recovery_dependency_epoch_not_admitted");
      }
      recovery_requested.store(false);
      ++recovery_frames;
    }
    auto bytes = read_access_unit(stream, frames[table_index]);
    auto message = rtc::make_message(bytes.size(), rtc::Message::Binary);
    std::memcpy(message->data(), bytes.data(), bytes.size());
    message->frameId = frames[table_index].frame_index;
    message->mediaEpoch = kMediaEpoch;
    message->accessUnitId = ++access_unit_id;
    message->dependencyEpoch = dependency_epoch;
    message->extendedTimestamp = extended_timestamp;
    if (!track->sendMessage(std::move(message))) {
      throw SenderError("track_send_failed");
    }
    ++sent_frames;
    ++table_index;
    extended_timestamp += 90'000U / arguments.frames_per_second;
    next_deadline += frame_interval;
    std::this_thread::sleep_until(next_deadline);
  }
  if (terminate_session.load()) {
    throw SenderError("feedback_flood_terminated_session");
  }
  if (stop_requested.load() || sent_frames == 0U) {
    throw SenderError("sender_interrupted_or_sent_no_frames");
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));
  const auto recovery_diagnostics = recovery->diagnostics();
  if (arguments.inject_pli_after_frame &&
      (!pli_injected || recovery_frames == 0U || recovery_diagnostics.idr_requests < 2U)) {
    throw SenderError("injected_pli_did_not_produce_recovery_idr");
  }
  if (arguments.fault_loss_extended_sequence &&
      (!egress.fault_suppressed() || !egress.protected_retransmission_observed() ||
       !egress.protected_retransmission_identical() ||
       recovery->cache_snapshot().retransmissions == 0U)) {
    throw SenderError("rollover_loss_did_not_recover_identical_protected_packet");
  }
  egress.stop();
  recovery->stop();
  std::shared_ptr<rtc::DataChannel> control;
  {
    const std::scoped_lock lock(state.mutex);
    control = state.control_channel;
  }
  if (control && control->isOpen()) {
    static_cast<void>(
        control->send(std::string("{\"protocolVersion\":\"glyphrelay-control-v1\","
                                  "\"sessionId\":\"m0-loopback-session\",\"sequence\":1,"
                                  "\"type\":\"SESSION_ENDED\"}")));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto next_extended_sequence = state.rtp_configuration->extendedSequenceNumber;
  track->close();
  peer.resetCallbacks();
  peer.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto summary =
      summary_json(arguments, state, egress, sent_frames, recovery_frames, next_extended_sequence,
                   extended_timestamp - 90'000U / arguments.frames_per_second);
  write_exclusive_file(arguments.summary, summary);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
  try {
    return run_sender(parse_arguments(argc, argv));
  } catch (const std::exception &error) {
    std::cerr << "m0 WebRTC sender failed: " << error.what() << '\n';
    return 8;
  }
}
