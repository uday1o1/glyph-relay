#include "glyphrelay/doctor.hpp"
#include "glyphrelay/m0_protocol.hpp"
#include "glyphrelay/nvenc_benchmark.hpp"
#include "glyphrelay/nvenc_browser_fixture.hpp"
#include "glyphrelay/record_command.hpp"
#include "glyphrelay/recording.hpp"
#include "glyphrelay/share_command.hpp"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

volatile std::sig_atomic_t command_stop_requested = 0;

extern "C" void request_command_stop(int) { command_stop_requested = 1; }

void print_help() {
  std::cout << "GlyphRelay " << GLYPHRELAY_VERSION << "\n"
            << "Usage:\n"
            << "  glyphrelay doctor [--json]\n"
            << "  glyphrelay benchmark --manifest FILE --output DIR\n"
            << "  glyphrelay browser-fixture --manifest FILE --output DIR\n"
            << "  glyphrelay share [--bitrate 500k|1m|2m|4m] [--record PATH.h264] [--json]\n"
            << "  glyphrelay record --output PATH.h264 [--window-label LABEL]"
               " [--bitrate 500k|1m|2m|4m]\n"
            << "  glyphrelay inspect --recording FILE [--json]\n"
            << "  glyphrelay --help\n";
}

std::string json_quote(std::string_view value) {
  std::string result = "\"";
  for (const unsigned char character : value) {
    switch (character) {
    case '\"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (character < 0x20U) {
        constexpr char digits[] = "0123456789abcdef";
        result += "\\u00";
        result.push_back(digits[(character >> 4U) & 0x0FU]);
        result.push_back(digits[character & 0x0FU]);
      } else {
        result.push_back(static_cast<char>(character));
      }
    }
  }
  result.push_back('\"');
  return result;
}

struct ArtifactArguments {
  std::filesystem::path manifest;
  std::filesystem::path output;
};

bool parse_artifact_arguments(int argc, char **argv, std::string_view command,
                              ArtifactArguments &arguments) {
  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if ((option == "--manifest" || option == "--output") && index + 1 < argc) {
      auto &destination = option == "--manifest" ? arguments.manifest : arguments.output;
      if (!destination.empty()) {
        std::cerr << option << " may be provided only once\n";
        return false;
      }
      destination = argv[++index];
    } else {
      std::cerr << command << " requires --manifest FILE --output DIR\n";
      return false;
    }
  }
  if (arguments.manifest.empty() || arguments.output.empty()) {
    std::cerr << command << " requires --manifest FILE --output DIR\n";
    return false;
  }
  if (std::filesystem::exists(arguments.output)) {
    std::cerr << command << " output already exists; refusing to overwrite it\n";
    return false;
  }
  return true;
}

int run_benchmark(int argc, char **argv) {
  ArtifactArguments arguments;
  if (!parse_artifact_arguments(argc, argv, "benchmark", arguments)) {
    return 2;
  }

  const auto verification = glyphrelay::verify_m0_protocol(arguments.manifest);
  if (!verification.passed) {
    std::cerr << "benchmark protocol verification failed: " << verification.reason << '\n';
    return 7;
  }
  std::cout << "Verified m0_fixed_map_v1 manifest " << verification.lock.manifest_sha256 << '\n';

  const auto result = glyphrelay::run_m0_nvenc_benchmark({verification.lock, arguments.output});
  if (result.status == glyphrelay::M0BenchmarkStatus::passed) {
    std::cout << "Milestone 0 NVENC benchmark completed: " << arguments.output << '\n';
    return 0;
  }
  std::cerr << "benchmark failed: " << result.reason << '\n';
  return result.status == glyphrelay::M0BenchmarkStatus::unsupported ? 3 : 8;
}

int run_browser_fixture(int argc, char **argv) {
  ArtifactArguments arguments;
  if (!parse_artifact_arguments(argc, argv, "browser-fixture", arguments)) {
    return 2;
  }
  const auto verification = glyphrelay::verify_m0_protocol(arguments.manifest);
  if (!verification.passed) {
    std::cerr << "browser fixture protocol verification failed: " << verification.reason << '\n';
    return 7;
  }
  std::cout << "Verified m0_fixed_map_v1 manifest " << verification.lock.manifest_sha256 << '\n';

  const auto result =
      glyphrelay::run_m0_nvenc_browser_fixture({verification.lock, arguments.output});
  if (result.status == glyphrelay::M0BenchmarkStatus::passed) {
    std::cout << "Milestone 0 NVENC browser fixture completed: " << arguments.output << '\n';
    return 0;
  }
  std::cerr << "browser fixture failed: " << result.reason << '\n';
  return result.status == glyphrelay::M0BenchmarkStatus::unsupported ? 3 : 8;
}

int run_record(int argc, char **argv) {
  glyphrelay::RecordCommandOptions options;
  bool output_seen = false;
  bool label_seen = false;
  bool bitrate_seen = false;
  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if ((option == "--output" || option == "--window-label" || option == "--bitrate") &&
        index + 1 < argc) {
      if ((option == "--output" && output_seen) || (option == "--window-label" && label_seen) ||
          (option == "--bitrate" && bitrate_seen)) {
        std::cerr << option << " may be provided only once\n";
        return 2;
      }
      const std::string value(argv[++index]);
      if (option == "--output") {
        options.output_path = value;
        output_seen = true;
      } else if (option == "--window-label") {
        options.window_label = value;
        label_seen = true;
      } else {
        options.bitrate_profile = value;
        bitrate_seen = true;
      }
    } else {
      std::cerr << "record requires --output PATH.h264 and accepts only --window-label and "
                   "--bitrate\n";
      return 2;
    }
  }
  if (!output_seen || !glyphrelay::record_bitrate_bps(options.bitrate_profile) ||
      !glyphrelay::valid_window_label(options.window_label)) {
    std::cerr << "record arguments are invalid\n";
    return 2;
  }

  command_stop_requested = 0;
  const auto previous_interrupt = std::signal(SIGINT, request_command_stop);
  const auto previous_terminate = std::signal(SIGTERM, request_command_stop);
  const auto result = glyphrelay::run_interactive_record(
      options, []() { return command_stop_requested != 0; },
      [](std::string_view label) { std::cout << "Selected window label: " << label << '\n'; });
  static_cast<void>(std::signal(SIGINT, previous_interrupt));
  static_cast<void>(std::signal(SIGTERM, previous_terminate));
  if (result.exit_code == 0) {
    std::cout << "Recording completed: " << options.output_path << " ("
              << result.encoded_access_units << " access units)\n";
  } else {
    std::cerr << "record failed: " << result.reason << '\n';
  }
  return result.exit_code;
}

int run_share(int argc, char **argv) {
  glyphrelay::ShareCommandOptions options;
  bool bitrate_seen = false;
  bool record_seen = false;
  bool json_seen = false;
  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if ((option == "--bitrate" || option == "--record") && index + 1 < argc) {
      if ((option == "--bitrate" && bitrate_seen) || (option == "--record" && record_seen)) {
        std::cerr << option << " may be provided only once\n";
        return 2;
      }
      const std::string value(argv[++index]);
      if (option == "--bitrate") {
        options.bitrate_profile = value;
        bitrate_seen = true;
      } else {
        options.recording_path = value;
        record_seen = true;
      }
    } else if (option == "--json" && !json_seen) {
      options.json = true;
      json_seen = true;
    } else {
      std::cerr << "share accepts only --bitrate, --record, and --json\n";
      return 2;
    }
  }
  if (const char *origin = std::getenv("GLYPHRELAY_SIGNALING_ORIGIN")) {
    options.signaling_origin = origin;
  }
  if (const char *ca_path = std::getenv("GLYPHRELAY_SIGNALING_CA_PATH")) {
    options.signaling_ca_path = ca_path;
  }
  std::string validation_reason;
  if (!glyphrelay::valid_share_options(options, validation_reason)) {
    if (options.json) {
      std::cout << "{\"exitCode\":2,\"reason\":" << json_quote(validation_reason)
                << ",\"connectionState\":\"idle\",\"joinUrl\":\"\"}\n";
    } else {
      std::cerr << "share failed: " << validation_reason << '\n';
    }
    return 2;
  }

  command_stop_requested = 0;
  const auto previous_interrupt = std::signal(SIGINT, request_command_stop);
  const auto previous_terminate = std::signal(SIGTERM, request_command_stop);
  const auto result = glyphrelay::run_interactive_share(
      options, []() { return command_stop_requested != 0; },
      options.json
          ? glyphrelay::ShareStatusCallback{}
          : glyphrelay::ShareStatusCallback{[](std::string_view state, std::string_view detail) {
              if (state == "join_open") {
                std::cout << "Share link: " << detail << '\n';
              } else {
                std::cout << "Share state: " << state << " (" << detail << ")\n";
              }
            }});
  static_cast<void>(std::signal(SIGINT, previous_interrupt));
  static_cast<void>(std::signal(SIGTERM, previous_terminate));

  if (options.json) {
    std::cout << "{\"exitCode\":" << result.exit_code << ",\"reason\":" << json_quote(result.reason)
              << ",\"connectionState\":" << json_quote(result.connection_state)
              << ",\"joinUrl\":" << json_quote(result.join_url)
              << ",\"recordingError\":" << json_quote(result.recording_error)
              << ",\"capturedFrames\":" << result.captured_frames
              << ",\"encodedAccessUnits\":" << result.encoded_access_units
              << ",\"transportedAccessUnits\":" << result.transported_access_units << "}\n";
  } else if (result.exit_code != 0) {
    std::cerr << "share failed: " << result.reason << '\n';
  }
  return result.exit_code;
}

int run_inspect(int argc, char **argv) {
  std::filesystem::path recording;
  bool json = false;
  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--recording" && index + 1 < argc && recording.empty()) {
      recording = argv[++index];
    } else if (option == "--json" && !json) {
      json = true;
    } else {
      std::cerr << "inspect requires --recording FILE and accepts only --json\n";
      return 2;
    }
  }
  if (recording.empty()) {
    std::cerr << "inspect requires --recording FILE\n";
    return 2;
  }
  if (!glyphrelay::durable_recording_available()) {
    std::cerr << "inspect failed: durable_recording_requires_linux\n";
    return 3;
  }
  const auto inspection = glyphrelay::inspect_recording(recording);
  if (json) {
    std::cout << glyphrelay::recording_inspection_json(inspection);
  } else {
    std::cout << "Recording state: "
              << glyphrelay::recording_inspection_state_name(inspection.state) << '\n'
              << "Reason: " << inspection.reason << '\n'
              << "Committed access units: " << inspection.committed_access_units << '\n'
              << "Committed media bytes: " << inspection.committed_media_bytes << '\n';
  }
  return inspection.passed ? 0 : 8;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    print_help();
    return 0;
  }

  if (argc >= 2 && std::string_view(argv[1]) == "doctor") {
    bool json = false;
    if (argc == 3 && std::string_view(argv[2]) == "--json") {
      json = true;
    } else if (argc != 2) {
      std::cerr << "doctor accepts only --json\n";
      return 2;
    }

    const auto report = glyphrelay::build_doctor_report(glyphrelay::collect_environment_snapshot());
    std::cout << (json ? glyphrelay::doctor_report_json(report)
                       : glyphrelay::doctor_report_text(report));
    return 0;
  }

  if (argc >= 2 && std::string_view(argv[1]) == "benchmark") {
    return run_benchmark(argc, argv);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "browser-fixture") {
    return run_browser_fixture(argc, argv);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "record") {
    return run_record(argc, argv);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "share") {
    return run_share(argc, argv);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "inspect") {
    return run_inspect(argc, argv);
  }

  print_help();
  return argc == 1 ? 0 : 2;
}
