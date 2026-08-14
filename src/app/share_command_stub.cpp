#include "glyphrelay/share_command.hpp"

namespace glyphrelay {

ShareRunResult run_interactive_share(const ShareCommandOptions &options, RecordStopPredicate,
                                     ShareStatusCallback) {
  std::string reason;
  if (!valid_share_options(options, reason)) {
    ShareRunResult result;
    result.exit_code = 2;
    result.reason = std::move(reason);
    return result;
  }
  ShareRunResult result;
  result.exit_code = 3;
  result.reason = "sharing_backend_unavailable";
  return result;
}

} // namespace glyphrelay
