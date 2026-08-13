#include "glyphrelay/doctor.hpp"

#include <iostream>
#include <string_view>

namespace {

void print_help() {
  std::cout << "GlyphRelay " << GLYPHRELAY_VERSION << "\n"
            << "Usage:\n"
            << "  glyphrelay doctor [--json]\n"
            << "  glyphrelay --help\n";
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

  print_help();
  return argc == 1 ? 0 : 2;
}
