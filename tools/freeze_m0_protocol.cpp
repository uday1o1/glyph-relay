#include "glyphrelay/sha256.hpp"
#include "glyphrelay/synthetic_source.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kUsage = "usage: freeze_m0_protocol --write-frame-hashes PATH\n"
                                    "       freeze_m0_protocol --hash-files FILE...\n";

bool safe_output(const std::filesystem::path &path) {
  return !path.empty() && path.filename() == "frame-hashes.tsv" &&
         path.parent_path().filename() == "m0_fixed_map_v1";
}

int write_frame_hashes(const std::filesystem::path &path) {
  if (!safe_output(path)) {
    std::cerr << "refusing an output outside m0_fixed_map_v1/frame-hashes.tsv\n";
    return 2;
  }
  if (std::filesystem::exists(path)) {
    std::cerr << "refusing to overwrite an existing frame-hash lock\n";
    return 2;
  }
  std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!output) {
    std::cerr << "cannot create the frame-hash lock\n";
    return 2;
  }
  output << "glyphrelay-frame-hashes-v1\n";
  glyphrelay::M0SyntheticSource source;
  std::vector<std::uint8_t> frame(glyphrelay::M0SourceGeometry::frame_bytes);
  for (std::size_t index = 0; index < glyphrelay::M0SourceGeometry::frame_count; ++index) {
    source.generate(index, frame);
    output << "frame\t" << index << '\t' << glyphrelay::sha256_hex(frame) << '\n';
  }
  output.flush();
  if (!output) {
    std::cerr << "failed while writing the frame-hash lock\n";
    return 2;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--write-frame-hashes") {
    return write_frame_hashes(argv[2]);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "--hash-files") {
    for (int index = 2; index < argc; ++index) {
      std::cout << glyphrelay::sha256_file_hex(argv[index]) << '\t'
                << std::filesystem::path(argv[index]).generic_string() << '\n';
    }
    return 0;
  }
  std::cerr << kUsage;
  return 2;
}
