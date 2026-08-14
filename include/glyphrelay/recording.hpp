#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace glyphrelay {

enum class RecordingEvent {
  journal_created,
  journal_header_written,
  journal_header_synced,
  media_temporary_created,
  sidecar_temporary_created,
  marker_temporary_created,
  media_temporary_synced,
  sidecar_temporary_synced,
  marker_temporary_synced,
  prepared_directory_synced,
  media_access_unit_written,
  media_group_synced,
  journal_group_written,
  journal_group_synced,
  sidecar_group_written,
  sidecar_group_synced,
  sidecar_written,
  sidecar_synced,
  media_renamed,
  sidecar_renamed,
  publication_directory_synced,
  marker_written,
  marker_synced,
  marker_renamed,
  marker_directory_synced,
  journal_removed,
  cleanup_directory_synced,
};

using RecordingEventCallback = std::function<void(RecordingEvent)>;

enum class RecordingPictureType : std::uint8_t { idr = 1, predicted = 2 };

struct RecordedAccessUnit {
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
  std::uint64_t media_epoch = 0;
  std::uint64_t dependency_epoch = 0;
  std::uint64_t geometry_epoch = 0;
  std::uint64_t encoder_configuration_epoch = 0;
  std::string configuration_sha256;
  std::uint64_t source_frame_id = 0;
  std::uint64_t extended_rtp_timestamp = 0;
  RecordingPictureType picture_type = RecordingPictureType::predicted;
  bool keyframe = false;
  bool parameter_sets_present = false;
  std::uint64_t presentation_timestamp_ns = 0;
};

struct RecorderConfig {
  std::filesystem::path output_path;
  std::string session_id;
  std::string recording_profile_sha256;
  std::size_t maximum_queue_bytes = 64U * 1024U * 1024U;
  std::uint64_t maximum_queue_age_ns = 2'000'000'000ULL;
  std::uint64_t group_commit_interval_ns = 250'000'000ULL;
  RecordingEventCallback event_callback;
};

struct RecorderEnqueueResult {
  bool accepted = false;
  bool failed = false;
  std::string reason;
};

struct RecorderFinalizeResult {
  bool passed = false;
  std::string reason;
};

struct RecorderDiagnostics {
  bool ready = false;
  bool failed = false;
  bool completed = false;
  std::string reason;
  std::string recording_id;
  std::size_t queue_access_units = 0;
  std::size_t queue_bytes = 0;
  std::size_t maximum_queue_bytes = 0;
  std::uint64_t maximum_queue_age_ns = 0;
  std::uint64_t accepted_access_units = 0;
  std::uint64_t committed_access_units = 0;
  std::uint64_t committed_media_bytes = 0;
  std::uint64_t overload_failures = 0;
};

class DurableRecorder {
public:
  explicit DurableRecorder(RecorderConfig config);
  ~DurableRecorder();

  DurableRecorder(DurableRecorder &&) noexcept;
  DurableRecorder &operator=(DurableRecorder &&) noexcept;
  DurableRecorder(const DurableRecorder &) = delete;
  DurableRecorder &operator=(const DurableRecorder &) = delete;

  bool ready() const;
  std::string initialization_reason() const;
  RecorderEnqueueResult enqueue(RecordedAccessUnit access_unit);
  RecorderFinalizeResult finalize();
  RecorderDiagnostics diagnostics() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

enum class RecordingInspectionState {
  absent,
  prepared_incomplete,
  incomplete,
  complete,
  corrupt,
};

struct RecordingInspection {
  bool passed = false;
  RecordingInspectionState state = RecordingInspectionState::absent;
  std::string reason;
  std::string session_id;
  std::string recording_id;
  std::filesystem::path media_path;
  std::filesystem::path sidecar_path;
  std::filesystem::path marker_path;
  std::uint64_t committed_access_units = 0;
  std::uint64_t committed_media_bytes = 0;
};

RecordingInspection inspect_recording(const std::filesystem::path &recording_path);
std::string recording_inspection_json(const RecordingInspection &inspection);
std::string_view recording_inspection_state_name(RecordingInspectionState state);
std::string_view recording_event_name(RecordingEvent event);
bool durable_recording_available();

} // namespace glyphrelay
