#include "glyphrelay/recording.hpp"

#include <sstream>

namespace glyphrelay {
namespace {

std::string json_escape(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 8U);
  for (const char value_character : value) {
    const auto character = static_cast<unsigned char>(value_character);
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20U) {
        constexpr char digits[] = "0123456789abcdef";
        output += "\\u00";
        output.push_back(digits[character >> 4U]);
        output.push_back(digits[character & 0x0FU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  return output;
}

} // namespace

std::string_view recording_inspection_state_name(RecordingInspectionState state) {
  switch (state) {
  case RecordingInspectionState::absent:
    return "ABSENT";
  case RecordingInspectionState::prepared_incomplete:
    return "PREPARED_INCOMPLETE";
  case RecordingInspectionState::incomplete:
    return "INCOMPLETE";
  case RecordingInspectionState::complete:
    return "COMPLETE";
  case RecordingInspectionState::corrupt:
    return "CORRUPT";
  }
  return "CORRUPT";
}

std::string recording_inspection_json(const RecordingInspection &inspection) {
  std::ostringstream output;
  output << "{\"schemaVersion\":1,\"passed\":" << (inspection.passed ? "true" : "false")
         << ",\"state\":\"" << recording_inspection_state_name(inspection.state)
         << "\",\"reason\":\"" << json_escape(inspection.reason) << "\",\"sessionId\":\""
         << json_escape(inspection.session_id) << "\",\"recordingId\":\""
         << json_escape(inspection.recording_id) << "\",\"mediaPath\":\""
         << json_escape(inspection.media_path.string()) << "\",\"sidecarPath\":\""
         << json_escape(inspection.sidecar_path.string()) << "\",\"markerPath\":\""
         << json_escape(inspection.marker_path.string())
         << "\",\"committedAccessUnits\":" << inspection.committed_access_units
         << ",\"committedMediaBytes\":" << inspection.committed_media_bytes << "}\n";
  return output.str();
}

std::string_view recording_event_name(RecordingEvent event) {
  switch (event) {
  case RecordingEvent::journal_created:
    return "journal_created";
  case RecordingEvent::journal_header_written:
    return "journal_header_written";
  case RecordingEvent::journal_header_synced:
    return "journal_header_synced";
  case RecordingEvent::media_temporary_created:
    return "media_temporary_created";
  case RecordingEvent::sidecar_temporary_created:
    return "sidecar_temporary_created";
  case RecordingEvent::marker_temporary_created:
    return "marker_temporary_created";
  case RecordingEvent::media_temporary_synced:
    return "media_temporary_synced";
  case RecordingEvent::sidecar_temporary_synced:
    return "sidecar_temporary_synced";
  case RecordingEvent::marker_temporary_synced:
    return "marker_temporary_synced";
  case RecordingEvent::prepared_directory_synced:
    return "prepared_directory_synced";
  case RecordingEvent::media_access_unit_written:
    return "media_access_unit_written";
  case RecordingEvent::media_group_synced:
    return "media_group_synced";
  case RecordingEvent::journal_group_written:
    return "journal_group_written";
  case RecordingEvent::journal_group_synced:
    return "journal_group_synced";
  case RecordingEvent::sidecar_group_written:
    return "sidecar_group_written";
  case RecordingEvent::sidecar_group_synced:
    return "sidecar_group_synced";
  case RecordingEvent::sidecar_written:
    return "sidecar_written";
  case RecordingEvent::sidecar_synced:
    return "sidecar_synced";
  case RecordingEvent::media_renamed:
    return "media_renamed";
  case RecordingEvent::sidecar_renamed:
    return "sidecar_renamed";
  case RecordingEvent::publication_directory_synced:
    return "publication_directory_synced";
  case RecordingEvent::marker_written:
    return "marker_written";
  case RecordingEvent::marker_synced:
    return "marker_synced";
  case RecordingEvent::marker_renamed:
    return "marker_renamed";
  case RecordingEvent::marker_directory_synced:
    return "marker_directory_synced";
  case RecordingEvent::journal_removed:
    return "journal_removed";
  case RecordingEvent::cleanup_directory_synced:
    return "cleanup_directory_synced";
  }
  return "unknown";
}

} // namespace glyphrelay
