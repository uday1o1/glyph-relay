#include "glyphrelay/m0_protocol.hpp"

#include "glyphrelay/sha256.hpp"
#include "glyphrelay/synthetic_source.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace glyphrelay {
namespace {

constexpr std::string_view kManifestMagic = "glyphrelay-protocol-lock-v1";
constexpr std::string_view kFrameHashMagic = "glyphrelay-frame-hashes-v1";

const std::array<std::string_view, 18> kRequiredComponents = {
    "protocols/m0_fixed_map_v1/source.json",
    "protocols/m0_fixed_map_v1/protected.mask.json",
    "protocols/m0_fixed_map_v1/comparison.mask.json",
    "protocols/m0_fixed_map_v1/emphasis-map.rle",
    "protocols/m0_fixed_map_v1/metric.json",
    "protocols/m0_fixed_map_v1/run-config.json",
    "protocols/m0_fixed_map_v1/frame-hashes.tsv",
    "include/glyphrelay/synthetic_source.hpp",
    "src/core/synthetic_source.cpp",
    "include/glyphrelay/quality_metrics.hpp",
    "src/core/quality_metrics.cpp",
    "include/glyphrelay/benchmark_gate.hpp",
    "src/core/benchmark_gate.cpp",
    "include/glyphrelay/nvenc_benchmark.hpp",
    "src/gpu/nvenc_benchmark.cpp",
    "tools/validate_m0_benchmark.py",
    "schemas/m0-benchmark-summary-v1.schema.json",
    "schemas/m0-benchmark-gate-v1.schema.json",
};

bool hexadecimal_sha256(std::string_view value) {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

std::string read_file(const std::filesystem::path &path, std::size_t maximum_size) {
  const auto size = std::filesystem::file_size(path);
  if (size > maximum_size) {
    throw std::runtime_error("protocol file exceeds its size bound: " + path.string());
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read protocol file: " + path.string());
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::string_view> split_tabs(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const auto separator = line.find('\t', start);
    fields.push_back(line.substr(start, separator == std::string_view::npos ? line.size() - start
                                                                            : separator - start));
    if (separator == std::string_view::npos) {
      return fields;
    }
    start = separator + 1U;
  }
}

template <typename Integer> Integer parse_integer(std::string_view value, const char *label) {
  Integer result{};
  const auto conversion = std::from_chars(value.data(), value.data() + value.size(), result);
  if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size()) {
    throw std::runtime_error(std::string("invalid ") + label);
  }
  return result;
}

bool path_is_within(const std::filesystem::path &candidate, const std::filesystem::path &root) {
  auto candidate_part = candidate.begin();
  for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++candidate_part) {
    if (candidate_part == candidate.end() || *candidate_part != *root_part) {
      return false;
    }
  }
  return true;
}

std::filesystem::path checked_component_path(const std::filesystem::path &root,
                                             const std::filesystem::path &relative) {
  if (relative.empty() || relative.is_absolute() || relative != relative.lexically_normal()) {
    throw std::runtime_error("component path is not a canonical relative path");
  }
  for (const auto &part : relative) {
    if (part == ".." || part == ".") {
      throw std::runtime_error("component path contains traversal");
    }
  }
  const auto unresolved = root / relative;
  if (std::filesystem::symlink_status(unresolved).type() != std::filesystem::file_type::regular) {
    throw std::runtime_error("component is not a regular non-symlink file: " + relative.string());
  }
  const auto resolved = std::filesystem::canonical(unresolved);
  if (!path_is_within(resolved, root)) {
    throw std::runtime_error("component resolves outside the repository root");
  }
  return resolved;
}

std::vector<std::string> lines_of(const std::string &content) {
  if (content.empty() || content.back() != '\n' || content.find('\r') != std::string::npos) {
    throw std::runtime_error("protocol text must use final-newline LF encoding");
  }
  std::vector<std::string> lines;
  std::stringstream stream(content);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(std::move(line));
  }
  return lines;
}

void verify_emphasis_map(const std::filesystem::path &path) {
  const auto lines = lines_of(read_file(path, 64U * 1024U));
  if (lines.empty() || lines.front() != "glyphrelay-emphasis-map-rle-v1") {
    throw std::runtime_error("emphasis map has the wrong format identity");
  }
  std::vector<std::int8_t> expanded;
  for (const auto &line : lines) {
    const auto fields = split_tabs(line);
    if (fields.empty() || fields[0] != "run") {
      continue;
    }
    if (fields.size() != 4U) {
      throw std::runtime_error("emphasis map run has the wrong field count");
    }
    const auto start = parse_integer<std::size_t>(fields[1], "emphasis run start");
    const auto count = parse_integer<std::size_t>(fields[2], "emphasis run count");
    const auto level = parse_integer<int>(fields[3], "emphasis level");
    if (start != expanded.size() || count == 0U || level < 0 || level > 5 ||
        count > M0SourceGeometry::map_entries - expanded.size()) {
      throw std::runtime_error("emphasis map run violates its bounds or raster continuity");
    }
    expanded.insert(expanded.end(), count, static_cast<std::int8_t>(level));
  }
  if (expanded != m0_fixed_emphasis_map()) {
    throw std::runtime_error("expanded emphasis map does not match the frozen map algorithm");
  }
}

std::vector<std::string> read_expected_frame_hashes(const std::filesystem::path &path) {
  const auto lines = lines_of(read_file(path, 512U * 1024U));
  if (lines.empty() || lines.front() != kFrameHashMagic) {
    throw std::runtime_error("frame hash list has the wrong format identity");
  }
  if (lines.size() != M0SourceGeometry::frame_count + 1U) {
    throw std::runtime_error("frame hash list has the wrong frame count");
  }
  std::vector<std::string> hashes;
  hashes.reserve(M0SourceGeometry::frame_count);
  for (std::size_t index = 0; index < M0SourceGeometry::frame_count; ++index) {
    const auto fields = split_tabs(lines[index + 1U]);
    if (fields.size() != 3U || fields[0] != "frame" ||
        parse_integer<std::size_t>(fields[1], "frame index") != index ||
        !hexadecimal_sha256(fields[2])) {
      throw std::runtime_error("frame hash list is malformed or out of order");
    }
    hashes.emplace_back(fields[2]);
  }
  return hashes;
}

void verify_source_frames(const std::vector<std::string> &expected) {
  M0SyntheticSource source;
  std::vector<std::uint8_t> frame(M0SourceGeometry::frame_bytes);
  for (std::size_t index = 0; index < expected.size(); ++index) {
    source.generate(index, frame);
    if (sha256_hex(frame) != expected[index]) {
      throw std::runtime_error("synthetic source frame hash mismatch at index " +
                               std::to_string(index));
    }
  }
}

} // namespace

ProtocolVerification verify_m0_protocol(const std::filesystem::path &manifest_path) {
  ProtocolVerification result;
  try {
    if (std::filesystem::symlink_status(manifest_path).type() !=
        std::filesystem::file_type::regular) {
      throw std::runtime_error("manifest is not a regular non-symlink file");
    }
    const auto canonical_manifest = std::filesystem::canonical(manifest_path);
    const auto root =
        std::filesystem::canonical(canonical_manifest.parent_path().parent_path().parent_path());
    if (canonical_manifest.filename() != "manifest.lock" ||
        canonical_manifest.parent_path().filename() != "m0_fixed_map_v1" ||
        canonical_manifest.parent_path().parent_path().filename() != "protocols") {
      throw std::runtime_error("manifest is not in protocols/m0_fixed_map_v1");
    }
    const auto content = read_file(canonical_manifest, 64U * 1024U);
    const auto lines = lines_of(content);
    if (lines.size() < 4U || lines[0] != kManifestMagic ||
        lines[1] != "protocol\tm0_fixed_map_v1") {
      throw std::runtime_error("manifest identity is invalid");
    }
    const auto digest_fields = split_tabs(lines.back());
    if (digest_fields.size() != 2U || digest_fields[0] != "manifest_sha256" ||
        !hexadecimal_sha256(digest_fields[1])) {
      throw std::runtime_error("manifest digest record is invalid");
    }
    const auto digest_line_size = lines.back().size() + 1U;
    const auto digest_input =
        std::string_view(content).substr(0, content.size() - digest_line_size);
    if (sha256_hex(digest_input) != digest_fields[1]) {
      throw std::runtime_error("manifest digest does not match its locked content");
    }

    std::set<std::string> seen;
    M0ProtocolLock lock;
    lock.repository_root = root;
    lock.manifest_sha256 = std::string(digest_fields[1]);
    for (std::size_t index = 2U; index + 1U < lines.size(); ++index) {
      const auto fields = split_tabs(lines[index]);
      if (fields.size() != 3U || fields[0] != "component" || !hexadecimal_sha256(fields[2])) {
        throw std::runtime_error("manifest component record is invalid");
      }
      const std::filesystem::path relative(fields[1]);
      if (!seen.insert(relative.generic_string()).second) {
        throw std::runtime_error("manifest contains a duplicate component");
      }
      const auto path = checked_component_path(root, relative);
      if (sha256_file_hex(path) != fields[2]) {
        throw std::runtime_error("component hash mismatch: " + relative.generic_string());
      }
      lock.components.push_back({relative, std::string(fields[2])});
    }
    for (const auto required : kRequiredComponents) {
      if (!seen.contains(std::string(required))) {
        throw std::runtime_error("manifest omits required component: " + std::string(required));
      }
    }

    verify_emphasis_map(root / "protocols/m0_fixed_map_v1/emphasis-map.rle");
    const auto frame_hashes =
        read_expected_frame_hashes(root / "protocols/m0_fixed_map_v1/frame-hashes.tsv");
    verify_source_frames(frame_hashes);
    result.passed = true;
    result.reason = "m0_fixed_map_v1_verified";
    result.lock = std::move(lock);
  } catch (const std::exception &error) {
    result.reason = error.what();
  }
  return result;
}

} // namespace glyphrelay
