#include "glyphrelay/record_command.hpp"

namespace glyphrelay {

RecordRunResult run_interactive_record(const RecordCommandOptions &, RecordStopPredicate,
                                       WindowSelectedCallback) {
  return {.exit_code = 3,
          .reason = "recording_requires_linux_portal",
          .captured_frames = 0U,
          .encoded_access_units = 0U,
          .frame_rate_drops = 0U,
          .capture = {},
          .recorder = {}};
}

} // namespace glyphrelay
