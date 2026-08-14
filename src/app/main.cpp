#include "glyphrelay/doctor.hpp"
#include "glyphrelay/m0_protocol.hpp"
#include "glyphrelay/nvenc_benchmark.hpp"
#include "glyphrelay/nvenc_browser_fixture.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void print_help() {
  std::cout << "GlyphRelay " << GLYPHRELAY_VERSION << "\n"
            << "Usage:\n"
            << "  glyphrelay doctor [--json]\n"
            << "  glyphrelay benchmark --manifest FILE --output DIR\n"
            << "  glyphrelay browser-fixture --manifest FILE --output DIR\n"
            << "  glyphrelay --help\n";
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

  print_help();
  return argc == 1 ? 0 : 2;
}
