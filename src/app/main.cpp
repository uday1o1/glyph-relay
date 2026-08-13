#include "glyphrelay/doctor.hpp"
#include "glyphrelay/m0_protocol.hpp"

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
            << "  glyphrelay --help\n";
}

int run_benchmark(int argc, char **argv) {
  std::filesystem::path manifest;
  std::filesystem::path output;
  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if ((option == "--manifest" || option == "--output") && index + 1 < argc) {
      auto &destination = option == "--manifest" ? manifest : output;
      if (!destination.empty()) {
        std::cerr << option << " may be provided only once\n";
        return 2;
      }
      destination = argv[++index];
    } else {
      std::cerr << "benchmark requires --manifest FILE --output DIR\n";
      return 2;
    }
  }
  if (manifest.empty() || output.empty()) {
    std::cerr << "benchmark requires --manifest FILE --output DIR\n";
    return 2;
  }
  if (std::filesystem::exists(output)) {
    std::cerr << "benchmark output already exists; refusing to overwrite it\n";
    return 2;
  }

  const auto verification = glyphrelay::verify_m0_protocol(manifest);
  if (!verification.passed) {
    std::cerr << "benchmark protocol verification failed: " << verification.reason << '\n';
    return 7;
  }
  std::cout << "Verified m0_fixed_map_v1 manifest " << verification.lock.manifest_sha256 << '\n';

  const auto report = glyphrelay::build_doctor_report(glyphrelay::collect_environment_snapshot());
  if (report.mode != "enhanced_nvenc") {
    std::cerr << "benchmark requires enhanced_nvenc; doctor selected " << report.mode << '\n';
    for (const auto &reason : report.reasons) {
      std::cerr << "reason: " << reason << '\n';
    }
    return 3;
  }
  std::cerr << "benchmark requires a build with the NVENC benchmark backend\n";
  return 3;
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

  print_help();
  return argc == 1 ? 0 : 2;
}
