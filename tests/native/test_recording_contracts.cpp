#include "glyphrelay/openh264_encoder.hpp"
#include "glyphrelay/recording.hpp"
#include "glyphrelay/recording_profile.hpp"
#include "glyphrelay/synthetic_source.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

struct TemporaryDirectory {
  TemporaryDirectory() {
    std::array<char, 64U> pattern{};
    constexpr std::string_view prefix = "/tmp/glyphrelay-recording-contract-XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    char *created = ::mkdtemp(pattern.data());
    require(created != nullptr, "temporary recording directory must be created");
    path = created;
    require(::chmod(path.c_str(), 0700) == 0,
            "temporary recording directory must have private permissions");
  }

  ~TemporaryDirectory() { std::filesystem::remove_all(path); }

  std::filesystem::path path;
};

std::vector<glyphrelay::RecordedAccessUnit> encoded_fixture() {
  glyphrelay::OpenH264Encoder encoder({
      .width = glyphrelay::M0SourceGeometry::visible_width,
      .height = glyphrelay::M0SourceGeometry::visible_height,
      .frames_per_second = glyphrelay::M0SourceGeometry::frames_per_second,
      .target_bitrate_bps = 4'000'000U,
      .maximum_bitrate_bps = 4'000'000U,
      .gop_frames = 60U,
      .level_idc = 40U,
  });
  require(encoder.available(), encoder.initialization_reason().c_str());
  glyphrelay::M0SyntheticSource source;
  std::vector<glyphrelay::RecordedAccessUnit> result;
  for (std::size_t source_index = 0U; source_index < 60U && result.size() < 3U; ++source_index) {
    const auto nv12 = source.generate(source_index);
    const auto i420 = glyphrelay::nv12_to_i420(
        nv12, glyphrelay::M0SourceGeometry::coded_width, glyphrelay::M0SourceGeometry::coded_height,
        glyphrelay::M0SourceGeometry::coded_width, glyphrelay::M0SourceGeometry::coded_width,
        glyphrelay::M0SourceGeometry::visible_width, glyphrelay::M0SourceGeometry::visible_height);
    const auto encoded = encoder.encode(i420, result.size() == 2U);
    require(encoded.passed, encoded.reason.c_str());
    if (encoded.skipped) {
      continue;
    }
    const auto bytes = std::make_shared<const std::vector<std::uint8_t>>(encoded.access_unit.bytes);
    const auto index = static_cast<std::uint64_t>(result.size());
    result.push_back({
        .bytes = bytes,
        .media_epoch = 1U,
        .dependency_epoch = 1U,
        .geometry_epoch = 1U,
        .encoder_configuration_epoch = 1U,
        .configuration_sha256 = glyphrelay::recording_profile_candidate_sha256(),
        .source_frame_id = index + 1U,
        .extended_rtp_timestamp = 90'000U + index * 3'000U,
        .picture_type = encoded.keyframe ? glyphrelay::RecordingPictureType::idr
                                         : glyphrelay::RecordingPictureType::predicted,
        .keyframe = encoded.keyframe,
        .parameter_sets_present =
            encoded.access_unit.contains(7U) && encoded.access_unit.contains(8U),
        .presentation_timestamp_ns = 1U + index * 33'333'333U,
    });
  }
  require(result.size() == 3U, "OpenH264 fixture must emit three access units");
  require(result.front().keyframe && result.front().parameter_sets_present,
          "recording fixture must begin with SPS, PPS, and IDR");
  require(!result[1U].keyframe,
          "recording fixture must provide a predicted access unit for commit tests");
  require(result.back().keyframe && result.back().parameter_sets_present,
          "recording fixture must contain a forced recovery IDR");
  return result;
}

glyphrelay::RecorderConfig config(const std::filesystem::path &output) {
  return {
      .output_path = output,
      .session_id = "recording_contract_session",
      .recording_profile_sha256 = glyphrelay::recording_profile_candidate_sha256(),
  };
}

void remove_completed_fixture(const std::filesystem::path &output) {
  std::filesystem::remove(output);
  std::filesystem::remove(output.string() + ".json");
  std::filesystem::remove(output.string() + ".complete");
  std::filesystem::remove(output.string() + ".journal");
}

void test_complete_recording(const std::filesystem::path &output,
                             const std::vector<glyphrelay::RecordedAccessUnit> &fixture) {
  remove_completed_fixture(output);
  std::vector<glyphrelay::RecordingEvent> events;
  auto recorder_config = config(output);
  recorder_config.event_callback = [&events](glyphrelay::RecordingEvent event) {
    events.push_back(event);
  };
  glyphrelay::DurableRecorder recorder(std::move(recorder_config));
  require(recorder.ready() && recorder.initialization_reason() == "RECORDER_READY",
          "durable recorder must pass its prepared barrier before admission");
  for (const auto &access_unit : fixture) {
    const auto result = recorder.enqueue(access_unit);
    require(result.accepted && !result.failed, "valid encoded access units must be admitted");
  }
  const auto finalized = recorder.finalize();
  require(finalized.passed && finalized.reason == "RECORDER_COMPLETED",
          "clean finalization must publish a complete recording");
  const auto diagnostics = recorder.diagnostics();
  require(diagnostics.completed && diagnostics.accepted_access_units == fixture.size() &&
              diagnostics.committed_access_units == fixture.size() &&
              diagnostics.maximum_queue_bytes == 64U * 1024U * 1024U &&
              diagnostics.maximum_queue_age_ns == 2'000'000'000ULL,
          "recorder diagnostics must expose asserted queue bounds and durable counts");
  const auto inspected = glyphrelay::inspect_recording(output);
  require(inspected.passed && inspected.state == glyphrelay::RecordingInspectionState::complete &&
              inspected.committed_access_units == fixture.size() &&
              inspected.committed_media_bytes > 0U,
          "completion marker, sidecar, hashes, and media must validate together");
  require(!std::filesystem::exists(output.string() + ".journal"),
          "the redundant journal may be removed only after marker publication");
  std::ifstream sidecar(output.string() + ".json", std::ios::binary);
  const std::string sidecar_content((std::istreambuf_iterator<char>(sidecar)),
                                    std::istreambuf_iterator<char>());
  require(sidecar_content.find("\"sourceFrameId\"") != std::string::npos &&
              sidecar_content.find("\"configurationSha256\"") != std::string::npos &&
              sidecar_content.find("\"payloadSha256\"") != std::string::npos &&
              sidecar_content.find("\"accessUnitCount\": 3") != std::string::npos,
          "complete sidecar must retain every required access-unit identity and checksum");

  const auto position = [&events](glyphrelay::RecordingEvent event) {
    const auto iterator = std::find(events.begin(), events.end(), event);
    require(iterator != events.end(), "successful recording must emit every durability event");
    return static_cast<std::size_t>(iterator - events.begin());
  };
  require(position(glyphrelay::RecordingEvent::journal_header_synced) <
                  position(glyphrelay::RecordingEvent::prepared_directory_synced) &&
              position(glyphrelay::RecordingEvent::media_group_synced) <
                  position(glyphrelay::RecordingEvent::journal_group_synced) &&
              position(glyphrelay::RecordingEvent::journal_group_synced) <
                  position(glyphrelay::RecordingEvent::sidecar_group_synced) &&
              position(glyphrelay::RecordingEvent::publication_directory_synced) <
                  position(glyphrelay::RecordingEvent::marker_renamed) &&
              position(glyphrelay::RecordingEvent::marker_directory_synced) <
                  position(glyphrelay::RecordingEvent::journal_removed),
          "durability events must preserve prepared, group-commit, and publication ordering");

  glyphrelay::DurableRecorder no_replace(config(output));
  require(!no_replace.ready() && no_replace.initialization_reason() == "OUTPUT_EXISTS",
          "a completed output must never be overwritten");
}

void test_admission_and_no_clobber(const std::vector<glyphrelay::RecordedAccessUnit> &fixture) {
  TemporaryDirectory temporary;
  const auto incomplete_path = temporary.path / "incomplete.h264";
  glyphrelay::DurableRecorder first(config(incomplete_path));
  require(first.ready(), "first recorder must reserve its deterministic journal");
  glyphrelay::DurableRecorder second(config(incomplete_path));
  require(!second.ready() && second.initialization_reason() == "OUTPUT_INCOMPLETE_EXISTS",
          "an incomplete journal must reserve the output path");

  const auto predicted_bytes = std::make_shared<const std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{0U, 0U, 0U, 1U, 0x41U, 0x80U});
  auto predicted = fixture.front();
  predicted.bytes = predicted_bytes;
  predicted.picture_type = glyphrelay::RecordingPictureType::predicted;
  predicted.keyframe = false;
  predicted.parameter_sets_present = false;
  require(first.enqueue(predicted).reason == "RECORDER_AWAITING_RECOVERY_IDR",
          "a new recording must admit nothing before a complete recovery IDR");

  const auto empty_path = temporary.path / "empty.h264";
  glyphrelay::DurableRecorder empty(config(empty_path));
  require(empty.ready() && !empty.finalize().passed &&
              empty.diagnostics().reason == "RECORDER_EMPTY",
          "a recording with no admitted recovery access unit must remain incomplete");

  const auto overloaded_path = temporary.path / "overloaded.h264";
  auto overloaded_config = config(overloaded_path);
  overloaded_config.maximum_queue_bytes = fixture.front().bytes->size() - 1U;
  glyphrelay::DurableRecorder overloaded(std::move(overloaded_config));
  require(overloaded.ready(), "small bounded queue configuration must initialize");
  const auto overload = overloaded.enqueue(fixture.front());
  require(!overload.accepted && overload.failed && overload.reason == "RECORDER_QUEUE_OVERLOADED" &&
              overloaded.diagnostics().overload_failures == 1U,
          "recorder overload must fail its branch without blocking or growing the queue");

  const auto target = temporary.path / "target";
  std::ofstream(target) << "preserve";
  const auto symlink_output = temporary.path / "symlink.h264";
  std::filesystem::create_symlink(target, symlink_output);
  glyphrelay::DurableRecorder symlink_recorder(config(symlink_output));
  require(!symlink_recorder.ready() && symlink_recorder.initialization_reason() == "OUTPUT_EXISTS",
          "a symbolic-link output must be preserved and rejected");
  std::ifstream target_input(target);
  std::string target_value;
  target_input >> target_value;
  require(target_value == "preserve", "rejected output links must not modify their target");
}

void test_sender_time_and_configuration_commits(
    const std::vector<glyphrelay::RecordedAccessUnit> &fixture) {
  TemporaryDirectory temporary;
  const auto output = temporary.path / "commit-boundaries.h264";
  std::vector<glyphrelay::RecordingEvent> events;
  auto recorder_config = config(output);
  recorder_config.event_callback = [&events](glyphrelay::RecordingEvent event) {
    events.push_back(event);
  };
  glyphrelay::DurableRecorder recorder(std::move(recorder_config));
  require(recorder.ready(), "commit-boundary recorder must initialize");
  require(recorder.enqueue(fixture.front()).accepted,
          "commit-boundary recorder must admit its initial recovery access unit");
  for (std::uint64_t index = 1U; index <= 14U; ++index) {
    auto predicted = fixture[1U];
    predicted.source_frame_id = index + 1U;
    predicted.extended_rtp_timestamp = 90'000U + index * 3'000U;
    predicted.presentation_timestamp_ns = 1U + index * 33'333'333U;
    predicted.encoder_configuration_epoch = index < 3U ? 1U : 2U;
    require(recorder.enqueue(std::move(predicted)).accepted,
            "commit-boundary predicted access unit must enqueue");
  }
  require(recorder.finalize().passed, "commit-boundary recording must finalize");
  const auto commit_count = static_cast<std::size_t>(
      std::count(events.begin(), events.end(), glyphrelay::RecordingEvent::journal_group_synced));
  require(commit_count >= 4U,
          "IDR, configuration change, sender-time interval, and stop must each force commits");
}

void test_complete_corruption_is_detected(
    const std::vector<glyphrelay::RecordedAccessUnit> &fixture) {
  TemporaryDirectory temporary;
  const auto output = temporary.path / "tampered.h264";
  glyphrelay::DurableRecorder recorder(config(output));
  require(recorder.ready(), "tamper fixture recorder must initialize");
  require(recorder.enqueue(fixture.front()).accepted,
          "tamper fixture recovery access unit must enqueue");
  require(recorder.finalize().passed, "tamper fixture must complete before mutation");
  std::ofstream marker(output.string() + ".complete", std::ios::app | std::ios::binary);
  marker << "x";
  marker.close();
  const auto inspected = glyphrelay::inspect_recording(output);
  require(!inspected.passed && inspected.state == glyphrelay::RecordingInspectionState::corrupt &&
              inspected.reason == "RECORDING_COMPLETION_MARKER_INVALID",
          "completion marker mutation must never be hidden as a valid recording");
}

void test_every_crash_boundary_is_inspectable(
    const std::vector<glyphrelay::RecordedAccessUnit> &fixture) {
  TemporaryDirectory temporary;
  constexpr std::array events = {
      glyphrelay::RecordingEvent::journal_created,
      glyphrelay::RecordingEvent::media_temporary_created,
      glyphrelay::RecordingEvent::sidecar_temporary_created,
      glyphrelay::RecordingEvent::marker_temporary_created,
      glyphrelay::RecordingEvent::journal_header_written,
      glyphrelay::RecordingEvent::journal_header_synced,
      glyphrelay::RecordingEvent::media_temporary_synced,
      glyphrelay::RecordingEvent::sidecar_temporary_synced,
      glyphrelay::RecordingEvent::marker_temporary_synced,
      glyphrelay::RecordingEvent::prepared_directory_synced,
      glyphrelay::RecordingEvent::media_access_unit_written,
      glyphrelay::RecordingEvent::media_group_synced,
      glyphrelay::RecordingEvent::journal_group_written,
      glyphrelay::RecordingEvent::journal_group_synced,
      glyphrelay::RecordingEvent::sidecar_group_written,
      glyphrelay::RecordingEvent::sidecar_group_synced,
      glyphrelay::RecordingEvent::sidecar_written,
      glyphrelay::RecordingEvent::sidecar_synced,
      glyphrelay::RecordingEvent::media_renamed,
      glyphrelay::RecordingEvent::sidecar_renamed,
      glyphrelay::RecordingEvent::publication_directory_synced,
      glyphrelay::RecordingEvent::marker_written,
      glyphrelay::RecordingEvent::marker_synced,
      glyphrelay::RecordingEvent::marker_renamed,
      glyphrelay::RecordingEvent::marker_directory_synced,
      glyphrelay::RecordingEvent::journal_removed,
      glyphrelay::RecordingEvent::cleanup_directory_synced,
  };
  for (std::size_t index = 0U; index < events.size(); ++index) {
    const auto directory = temporary.path / ("crash-" + std::to_string(index));
    std::filesystem::create_directory(directory);
    require(::chmod(directory.c_str(), 0700) == 0, "crash fixture directory must be private");
    const auto output = directory / "recording.h264";
    const pid_t child = ::fork();
    require(child >= 0, "crash fixture process must fork");
    if (child == 0) {
      auto crash_config = config(output);
      crash_config.event_callback = [event = events[index]](glyphrelay::RecordingEvent observed) {
        if (observed == event) {
          ::_exit(86);
        }
      };
      glyphrelay::DurableRecorder recorder(std::move(crash_config));
      if (!recorder.ready() || !recorder.enqueue(fixture.front()).accepted ||
          !recorder.finalize().passed) {
        ::_exit(87);
      }
      ::_exit(88);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
            "each declared crash event must be reached exactly once");
    const auto inspected = glyphrelay::inspect_recording(output);
    require(inspected.state != glyphrelay::RecordingInspectionState::absent,
            "every injected crash must leave a deterministic inspectable anchor or marker");
    if (inspected.state == glyphrelay::RecordingInspectionState::complete) {
      require(inspected.passed,
              "a crash-visible complete artifact must pass marker and companion validation");
    } else {
      require(std::filesystem::exists(output.string() + ".journal"),
              "every incomplete crash artifact must remain journal anchored");
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "recording contracts require one durable output path");
  require(glyphrelay::durable_recording_available(),
          "Linux contract build must provide the durable recorder");
  const auto fixture = encoded_fixture();
  test_complete_recording(argv[1], fixture);
  test_admission_and_no_clobber(fixture);
  test_sender_time_and_configuration_commits(fixture);
  test_complete_corruption_is_detected(fixture);
  test_every_crash_boundary_is_inspectable(fixture);
  return 0;
}
