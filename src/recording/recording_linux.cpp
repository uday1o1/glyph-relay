#include "glyphrelay/recording.hpp"

#include "glyphrelay/annex_b.hpp"
#include "glyphrelay/sha256.hpp"

#include <linux/fs.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace glyphrelay {
namespace {

constexpr std::size_t kMaximumAccessUnitBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumQueuedAccessUnits = 4096U;
constexpr std::size_t kMaximumPendingJournalRecords = 4096U;
constexpr std::uint64_t kMaximumCommitDelayNs = 1'000'000'000ULL;
constexpr std::array<std::uint8_t, 8U> kJournalMagic = {'G', 'L', 'Y', 'R', 'J', 'N', 'L', '1'};
constexpr std::array<std::uint8_t, 4U> kAccessUnitMagic = {'A', 'U', 'R', '1'};
constexpr std::array<std::uint8_t, 4U> kCommitMagic = {'C', 'M', 'T', '1'};

class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int value) : value_(value) {}
  ~UniqueFd() { reset(); }
  UniqueFd(UniqueFd &&other) noexcept : value_(std::exchange(other.value_, -1)) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  explicit operator bool() const { return value_ >= 0; }
  int get() const { return value_; }
  int release() { return std::exchange(value_, -1); }
  void reset(int replacement = -1) {
    if (value_ >= 0) {
      while (::close(value_) < 0 && errno == EINTR) {
      }
    }
    value_ = replacement;
  }

private:
  int value_ = -1;
};

struct RecordingNames {
  std::string final_media;
  std::string final_sidecar;
  std::string final_marker;
  std::string journal;
  std::string temporary_media;
  std::string temporary_sidecar;
  std::string temporary_marker;
};

struct OpenedOutput {
  UniqueFd directory;
  std::filesystem::path parent;
  std::string basename;
};

struct JournalHeader {
  std::uint32_t version = 0;
  std::string session_id;
  std::string recording_id;
  std::string profile_sha256;
  RecordingNames names;
  std::size_t byte_size = 0;
};

struct JournalAccessUnit {
  std::uint64_t index = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::string session_id;
  std::string recording_id;
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
  Sha256::Digest payload_sha256{};
};

struct ParsedJournal {
  JournalHeader header;
  std::uint64_t committed_count = 0;
  std::uint64_t committed_bytes = 0;
};

bool valid_hex64(std::string_view value) {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

bool valid_identifier(std::string_view value) {
  return !value.empty() && value.size() <= 64U &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '_' || character == '-';
         });
}

bool safe_filename(std::string_view name) {
  return !name.empty() && name != "." && name != ".." && name.size() <= 240U &&
         name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos &&
         std::all_of(name.begin(), name.end(), [](unsigned char character) {
           return character >= 0x20U && character != 0x7FU;
         });
}

bool path_contains_parent_reference(const std::filesystem::path &path) {
  return std::any_of(path.begin(), path.end(),
                     [](const auto &component) { return component == ".."; });
}

bool checked_add(std::size_t left, std::size_t right, std::size_t &result) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void patch_u32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned int index = 0U; index < 4U; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

void append_string(std::vector<std::uint8_t> &bytes, std::string_view value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("recording journal string is too large");
  }
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

class ByteReader {
public:
  explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  bool read_u8(std::uint8_t &value) {
    if (offset_ >= bytes_.size()) {
      return false;
    }
    value = bytes_[offset_++];
    return true;
  }

  bool read_u32(std::uint32_t &value) {
    if (bytes_.size() - offset_ < 4U) {
      return false;
    }
    value = 0U;
    for (unsigned int index = 0U; index < 4U; ++index) {
      value |= static_cast<std::uint32_t>(bytes_[offset_++]) << (index * 8U);
    }
    return true;
  }

  bool read_u64(std::uint64_t &value) {
    if (bytes_.size() - offset_ < 8U) {
      return false;
    }
    value = 0U;
    for (unsigned int index = 0U; index < 8U; ++index) {
      value |= static_cast<std::uint64_t>(bytes_[offset_++]) << (index * 8U);
    }
    return true;
  }

  bool read_string(std::string &value, std::size_t maximum = 4096U) {
    std::uint32_t size = 0U;
    if (!read_u32(size) || size > maximum || bytes_.size() - offset_ < size) {
      return false;
    }
    value.assign(reinterpret_cast<const char *>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }

  bool read_digest(Sha256::Digest &digest) {
    if (bytes_.size() - offset_ < digest.size()) {
      return false;
    }
    std::copy_n(bytes_.data() + offset_, digest.size(), digest.begin());
    offset_ += digest.size();
    return true;
  }

  std::size_t offset() const { return offset_; }
  std::size_t remaining() const { return bytes_.size() - offset_; }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t offset_ = 0U;
};

bool write_all(int descriptor, std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto result = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(result);
  }
  return true;
}

bool sync_data(int descriptor) {
  while (::fdatasync(descriptor) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

bool sync_file(int descriptor) {
  while (::fsync(descriptor) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

bool regular_owned_file(int descriptor) {
  struct stat information{};
  return ::fstat(descriptor, &information) == 0 && S_ISREG(information.st_mode) &&
         information.st_uid == ::geteuid() && (information.st_mode & 0777U) == 0600U;
}

bool directory_is_safe(int descriptor) {
  struct stat information{};
  return ::fstat(descriptor, &information) == 0 && S_ISDIR(information.st_mode) &&
         information.st_uid == ::geteuid() && (information.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

OpenedOutput open_output_parent(const std::filesystem::path &output, std::string &reason) {
  OpenedOutput result;
  if (output.empty() || output.filename().empty() || path_contains_parent_reference(output)) {
    reason = "OUTPUT_PATH_UNSAFE";
    return result;
  }
  result.basename = output.filename().string();
  if (!safe_filename(result.basename) || output.extension() != ".h264") {
    reason = "OUTPUT_PATH_UNSAFE";
    return result;
  }
  result.parent = output.parent_path();
  if (result.parent.empty()) {
    result.parent = ".";
  }
  UniqueFd current(
      ::open(output.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!current) {
    reason = "OUTPUT_DIRECTORY_OPEN_FAILED";
    return result;
  }
  for (const auto &component : result.parent.relative_path()) {
    if (component == "." || component.empty()) {
      continue;
    }
    const auto name = component.string();
    if (!safe_filename(name)) {
      reason = "OUTPUT_PATH_UNSAFE";
      return result;
    }
    UniqueFd next(
        ::openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!next) {
      reason = errno == EACCES ? "OUTPUT_PERMISSION_DENIED" : "OUTPUT_DIRECTORY_OPEN_FAILED";
      return result;
    }
    current = std::move(next);
  }
  if (!directory_is_safe(current.get())) {
    reason = "OUTPUT_DIRECTORY_UNSAFE";
    return result;
  }
  const long maximum_name = ::fpathconf(current.get(), _PC_NAME_MAX);
  if (maximum_name <= 0 || static_cast<std::size_t>(maximum_name) < 64U) {
    reason = "OUTPUT_DIRECTORY_NAME_LIMIT_INVALID";
    return result;
  }
  result.directory = std::move(current);
  reason = "OUTPUT_DIRECTORY_READY";
  return result;
}

bool path_exists(int directory, std::string_view name) {
  struct stat information{};
  if (::fstatat(directory, std::string(name).c_str(), &information, AT_SYMLINK_NOFOLLOW) == 0) {
    return true;
  }
  return errno != ENOENT;
}

UniqueFd create_regular_file(int directory, std::string_view name) {
  const int descriptor = ::openat(directory, std::string(name).c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  UniqueFd result(descriptor);
  if (!result || !regular_owned_file(result.get())) {
    return {};
  }
  return result;
}

std::optional<std::string> random_hex(std::size_t byte_count) {
  std::vector<std::uint8_t> bytes(byte_count);
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0U);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return std::nullopt;
    }
    offset += static_cast<std::size_t>(count);
  }
  constexpr char digits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2U, '0');
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    result[index * 2U] = digits[bytes[index] >> 4U];
    result[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
  }
  return result;
}

bool unlink_if_present(int directory, std::string_view name) {
  while (::unlinkat(directory, std::string(name).c_str(), 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    return errno == ENOENT;
  }
  return true;
}

bool rename_noreplace(int directory, std::string_view source, std::string_view destination) {
  while (true) {
    const long result = ::syscall(SYS_renameat2, directory, std::string(source).c_str(), directory,
                                  std::string(destination).c_str(), RENAME_NOREPLACE);
    if (result == 0) {
      return true;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

bool probe_rename_noreplace(int directory) {
  const auto identifier = random_hex(8U);
  if (!identifier) {
    return false;
  }
  const std::string source = ".glyphrelay-rename-probe-" + *identifier + ".source";
  const std::string destination = ".glyphrelay-rename-probe-" + *identifier + ".destination";
  auto source_fd = create_regular_file(directory, source);
  auto destination_fd = create_regular_file(directory, destination);
  if (!source_fd || !destination_fd) {
    unlink_if_present(directory, source);
    unlink_if_present(directory, destination);
    return false;
  }
  source_fd.reset();
  destination_fd.reset();
  const long result = ::syscall(SYS_renameat2, directory, source.c_str(), directory,
                                destination.c_str(), RENAME_NOREPLACE);
  const int saved_error = errno;
  const bool cleaned = unlink_if_present(directory, source) &&
                       unlink_if_present(directory, destination) && sync_file(directory);
  return result < 0 && saved_error == EEXIST && cleaned;
}

std::vector<std::uint8_t> encode_header(const JournalHeader &header) {
  std::vector<std::uint8_t> bytes(kJournalMagic.begin(), kJournalMagic.end());
  append_u32(bytes, 1U);
  const auto length_offset = bytes.size();
  append_u32(bytes, 0U);
  append_u32(bytes, 1U);
  append_string(bytes, header.session_id);
  append_string(bytes, header.recording_id);
  append_string(bytes, header.profile_sha256);
  append_string(bytes, header.names.final_media);
  append_string(bytes, header.names.final_sidecar);
  append_string(bytes, header.names.final_marker);
  append_string(bytes, header.names.journal);
  append_string(bytes, header.names.temporary_media);
  append_string(bytes, header.names.temporary_sidecar);
  append_string(bytes, header.names.temporary_marker);
  const auto total_size = bytes.size() + Sha256::Digest{}.size();
  if (total_size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("recording journal header is too large");
  }
  patch_u32(bytes, length_offset, static_cast<std::uint32_t>(total_size));
  const auto digest = Sha256::digest(bytes);
  bytes.insert(bytes.end(), digest.begin(), digest.end());
  return bytes;
}

std::optional<JournalHeader> parse_header(std::span<const std::uint8_t> journal) {
  if (journal.size() < kJournalMagic.size() + 12U + Sha256::Digest{}.size() ||
      !std::equal(kJournalMagic.begin(), kJournalMagic.end(), journal.begin())) {
    return std::nullopt;
  }
  ByteReader prefix(journal.subspan(kJournalMagic.size()));
  std::uint32_t version = 0U;
  std::uint32_t header_length = 0U;
  std::uint32_t state = 0U;
  if (!prefix.read_u32(version) || !prefix.read_u32(header_length) || !prefix.read_u32(state) ||
      version != 1U || state != 1U || header_length > journal.size() || header_length > 65536U ||
      header_length < kJournalMagic.size() + 12U + Sha256::Digest{}.size()) {
    return std::nullopt;
  }
  const auto hashed_size = static_cast<std::size_t>(header_length) - Sha256::Digest{}.size();
  const auto expected = Sha256::digest(journal.first(hashed_size));
  if (!std::equal(expected.begin(), expected.end(),
                  journal.begin() + static_cast<std::ptrdiff_t>(hashed_size))) {
    return std::nullopt;
  }
  ByteReader reader(
      journal.subspan(kJournalMagic.size() + 12U, hashed_size - (kJournalMagic.size() + 12U)));
  JournalHeader header;
  header.version = version;
  if (!reader.read_string(header.session_id) || !reader.read_string(header.recording_id) ||
      !reader.read_string(header.profile_sha256) || !reader.read_string(header.names.final_media) ||
      !reader.read_string(header.names.final_sidecar) ||
      !reader.read_string(header.names.final_marker) || !reader.read_string(header.names.journal) ||
      !reader.read_string(header.names.temporary_media) ||
      !reader.read_string(header.names.temporary_sidecar) ||
      !reader.read_string(header.names.temporary_marker) || reader.remaining() != 0U ||
      !valid_identifier(header.session_id) || !valid_identifier(header.recording_id) ||
      !valid_hex64(header.profile_sha256) || !safe_filename(header.names.final_media) ||
      !safe_filename(header.names.final_sidecar) || !safe_filename(header.names.final_marker) ||
      !safe_filename(header.names.journal) || !safe_filename(header.names.temporary_media) ||
      !safe_filename(header.names.temporary_sidecar) ||
      !safe_filename(header.names.temporary_marker)) {
    return std::nullopt;
  }
  header.byte_size = header_length;
  return header;
}

std::vector<std::uint8_t> encode_access_unit_record(const JournalAccessUnit &record) {
  std::vector<std::uint8_t> bytes(kAccessUnitMagic.begin(), kAccessUnitMagic.end());
  const auto length_offset = bytes.size();
  append_u32(bytes, 0U);
  append_u64(bytes, record.index);
  append_u64(bytes, record.offset);
  append_u64(bytes, record.size);
  append_u64(bytes, record.media_epoch);
  append_u64(bytes, record.dependency_epoch);
  append_u64(bytes, record.geometry_epoch);
  append_u64(bytes, record.encoder_configuration_epoch);
  append_u64(bytes, record.source_frame_id);
  append_u64(bytes, record.extended_rtp_timestamp);
  append_u64(bytes, record.presentation_timestamp_ns);
  bytes.push_back(static_cast<std::uint8_t>(record.picture_type));
  bytes.push_back(record.keyframe ? 1U : 0U);
  bytes.push_back(record.parameter_sets_present ? 1U : 0U);
  bytes.push_back(0U);
  append_string(bytes, record.session_id);
  append_string(bytes, record.recording_id);
  append_string(bytes, record.configuration_sha256);
  bytes.insert(bytes.end(), record.payload_sha256.begin(), record.payload_sha256.end());
  const auto total_size = bytes.size() + Sha256::Digest{}.size();
  if (total_size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("recording journal record is too large");
  }
  patch_u32(bytes, length_offset, static_cast<std::uint32_t>(total_size));
  const auto digest = Sha256::digest(bytes);
  bytes.insert(bytes.end(), digest.begin(), digest.end());
  return bytes;
}

std::vector<std::uint8_t> encode_commit_record(std::uint64_t highest_index,
                                               std::uint64_t durable_bytes) {
  std::vector<std::uint8_t> bytes(kCommitMagic.begin(), kCommitMagic.end());
  const auto length_offset = bytes.size();
  append_u32(bytes, 0U);
  append_u64(bytes, highest_index);
  append_u64(bytes, durable_bytes);
  const auto total_size = bytes.size() + Sha256::Digest{}.size();
  patch_u32(bytes, length_offset, static_cast<std::uint32_t>(total_size));
  const auto digest = Sha256::digest(bytes);
  bytes.insert(bytes.end(), digest.begin(), digest.end());
  return bytes;
}

bool record_digest_valid(std::span<const std::uint8_t> record) {
  if (record.size() < Sha256::Digest{}.size()) {
    return false;
  }
  const auto payload = record.first(record.size() - Sha256::Digest{}.size());
  const auto digest = Sha256::digest(payload);
  return std::equal(digest.begin(), digest.end(),
                    record.end() - static_cast<std::ptrdiff_t>(digest.size()));
}

std::optional<JournalAccessUnit> parse_access_unit_record(std::span<const std::uint8_t> record) {
  if (record.size() < 8U + Sha256::Digest{}.size() ||
      !std::equal(kAccessUnitMagic.begin(), kAccessUnitMagic.end(), record.begin()) ||
      !record_digest_valid(record)) {
    return std::nullopt;
  }
  ByteReader reader(record.subspan(4U, record.size() - 4U - Sha256::Digest{}.size()));
  std::uint32_t length = 0U;
  JournalAccessUnit output;
  std::uint8_t picture = 0U;
  std::uint8_t keyframe = 0U;
  std::uint8_t parameter_sets = 0U;
  std::uint8_t reserved = 0U;
  if (!reader.read_u32(length) || length != record.size() || !reader.read_u64(output.index) ||
      !reader.read_u64(output.offset) || !reader.read_u64(output.size) ||
      !reader.read_u64(output.media_epoch) || !reader.read_u64(output.dependency_epoch) ||
      !reader.read_u64(output.geometry_epoch) ||
      !reader.read_u64(output.encoder_configuration_epoch) ||
      !reader.read_u64(output.source_frame_id) || !reader.read_u64(output.extended_rtp_timestamp) ||
      !reader.read_u64(output.presentation_timestamp_ns) || !reader.read_u8(picture) ||
      !reader.read_u8(keyframe) || !reader.read_u8(parameter_sets) || !reader.read_u8(reserved) ||
      !reader.read_string(output.session_id) || !reader.read_string(output.recording_id) ||
      !reader.read_string(output.configuration_sha256) ||
      !reader.read_digest(output.payload_sha256) || reader.remaining() != 0U || reserved != 0U ||
      (picture != static_cast<std::uint8_t>(RecordingPictureType::idr) &&
       picture != static_cast<std::uint8_t>(RecordingPictureType::predicted)) ||
      keyframe > 1U || parameter_sets > 1U || !valid_identifier(output.session_id) ||
      !valid_identifier(output.recording_id) || !valid_hex64(output.configuration_sha256)) {
    return std::nullopt;
  }
  output.picture_type = static_cast<RecordingPictureType>(picture);
  output.keyframe = keyframe != 0U;
  output.parameter_sets_present = parameter_sets != 0U;
  return output;
}

bool parse_commit_record(std::span<const std::uint8_t> record, std::uint64_t &highest,
                         std::uint64_t &durable_bytes) {
  if (record.size() < 8U + Sha256::Digest{}.size() ||
      !std::equal(kCommitMagic.begin(), kCommitMagic.end(), record.begin()) ||
      !record_digest_valid(record)) {
    return false;
  }
  ByteReader reader(record.subspan(4U, record.size() - 4U - Sha256::Digest{}.size()));
  std::uint32_t length = 0U;
  return reader.read_u32(length) && length == record.size() && reader.read_u64(highest) &&
         reader.read_u64(durable_bytes) && reader.remaining() == 0U;
}

std::optional<std::vector<std::uint8_t>> read_all(int descriptor, std::size_t maximum) {
  struct stat information{};
  if (::fstat(descriptor, &information) != 0 || information.st_size < 0 ||
      static_cast<std::uint64_t>(information.st_size) > maximum) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> result(static_cast<std::size_t>(information.st_size));
  std::size_t offset = 0U;
  while (offset < result.size()) {
    const auto count = ::pread(descriptor, result.data() + offset, result.size() - offset,
                               static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return std::nullopt;
    }
    offset += static_cast<std::size_t>(count);
  }
  return result;
}

bool read_exact_at(int descriptor, std::span<std::uint8_t> bytes, std::uint64_t offset) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      bytes.size() > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - offset) {
    return false;
  }
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const auto count = ::pread(descriptor, bytes.data() + consumed, bytes.size() - consumed,
                               static_cast<off_t>(offset + static_cast<std::uint64_t>(consumed)));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    consumed += static_cast<std::size_t>(count);
  }
  return true;
}

std::optional<UniqueFd> open_regular_readonly(int directory, std::string_view name) {
  UniqueFd descriptor(
      ::openat(directory, std::string(name).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (!descriptor || !regular_owned_file(descriptor.get())) {
    return std::nullopt;
  }
  return std::optional<UniqueFd>(std::move(descriptor));
}

std::optional<Sha256::Digest> hash_fd_range(int descriptor, std::uint64_t offset,
                                            std::uint64_t size);

std::optional<JournalHeader> read_journal_header(int descriptor, std::uint64_t journal_size) {
  constexpr std::size_t minimum_header_size = kJournalMagic.size() + 12U + Sha256::Digest{}.size();
  if (journal_size < minimum_header_size) {
    return std::nullopt;
  }
  std::array<std::uint8_t, kJournalMagic.size() + 12U> prefix{};
  if (!read_exact_at(descriptor, prefix, 0U) ||
      !std::equal(kJournalMagic.begin(), kJournalMagic.end(), prefix.begin())) {
    return std::nullopt;
  }
  const std::uint32_t header_length = static_cast<std::uint32_t>(prefix[12U]) |
                                      (static_cast<std::uint32_t>(prefix[13U]) << 8U) |
                                      (static_cast<std::uint32_t>(prefix[14U]) << 16U) |
                                      (static_cast<std::uint32_t>(prefix[15U]) << 24U);
  if (header_length < minimum_header_size || header_length > 65536U ||
      header_length > journal_size) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes(header_length);
  if (!read_exact_at(descriptor, bytes, 0U)) {
    return std::nullopt;
  }
  return parse_header(bytes);
}

std::optional<ParsedJournal> parse_journal(int journal_descriptor, std::uint64_t journal_size,
                                           int media_descriptor, std::uint64_t media_size,
                                           const JournalHeader &header) {
  ParsedJournal output;
  output.header = header;
  std::uint64_t offset = header.byte_size;
  std::uint64_t next_index = 0U;
  std::uint64_t expected_offset = 0U;
  std::vector<JournalAccessUnit> pending;
  while (journal_size - offset >= 8U) {
    std::array<std::uint8_t, 8U> prefix{};
    if (!read_exact_at(journal_descriptor, prefix, offset)) {
      break;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(prefix[4U]) |
                                 (static_cast<std::uint32_t>(prefix[5U]) << 8U) |
                                 (static_cast<std::uint32_t>(prefix[6U]) << 16U) |
                                 (static_cast<std::uint32_t>(prefix[7U]) << 24U);
    if (length < 8U + Sha256::Digest{}.size() || length > 65536U ||
        length > journal_size - offset) {
      break;
    }
    std::vector<std::uint8_t> record(length);
    if (!read_exact_at(journal_descriptor, record, offset)) {
      break;
    }
    if (std::equal(kAccessUnitMagic.begin(), kAccessUnitMagic.end(), record.begin())) {
      const auto parsed = parse_access_unit_record(record);
      if (!parsed || parsed->index != next_index ||
          parsed->session_id != output.header.session_id ||
          parsed->recording_id != output.header.recording_id ||
          pending.size() == kMaximumPendingJournalRecords) {
        break;
      }
      pending.push_back(*parsed);
      ++next_index;
    } else if (std::equal(kCommitMagic.begin(), kCommitMagic.end(), record.begin())) {
      std::uint64_t highest = 0U;
      std::uint64_t durable = 0U;
      if (!parse_commit_record(record, highest, durable) || pending.empty() ||
          highest != pending.back().index || highest + 1U != next_index ||
          pending.back().offset > std::numeric_limits<std::uint64_t>::max() - pending.back().size ||
          pending.back().offset + pending.back().size != durable || durable > media_size) {
        break;
      }
      auto group_offset = expected_offset;
      for (const auto &access_unit : pending) {
        if (access_unit.offset != group_offset || access_unit.size == 0U ||
            access_unit.offset > durable || access_unit.size > durable - access_unit.offset) {
          return std::nullopt;
        }
        const auto digest = hash_fd_range(media_descriptor, access_unit.offset, access_unit.size);
        if (!digest || *digest != access_unit.payload_sha256) {
          return std::nullopt;
        }
        group_offset += access_unit.size;
      }
      if (group_offset != durable) {
        return std::nullopt;
      }
      output.committed_count = highest + 1U;
      output.committed_bytes = durable;
      expected_offset = durable;
      pending.clear();
    } else {
      break;
    }
    offset += length;
  }
  return output;
}

std::optional<Sha256::Digest> hash_fd_range(int descriptor, std::uint64_t offset,
                                            std::uint64_t size) {
  Sha256 hash;
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  std::uint64_t consumed = 0U;
  while (consumed < size) {
    const auto wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - consumed));
    const auto count =
        ::pread(descriptor, buffer.data(), wanted, static_cast<off_t>(offset + consumed));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return std::nullopt;
    }
    hash.update(std::span(buffer.data(), static_cast<std::size_t>(count)));
    consumed += static_cast<std::uint64_t>(count);
  }
  return hash.finalize();
}

std::optional<std::string> hash_fd_hex(int descriptor) {
  struct stat information{};
  if (::fstat(descriptor, &information) != 0 || information.st_size < 0) {
    return std::nullopt;
  }
  const auto digest =
      hash_fd_range(descriptor, 0U, static_cast<std::uint64_t>(information.st_size));
  return digest ? std::optional<std::string>(sha256_hex(*digest)) : std::nullopt;
}

std::optional<std::uint64_t> file_size(int descriptor) {
  struct stat information{};
  if (::fstat(descriptor, &information) != 0 || information.st_size < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(information.st_size);
}

std::string sidecar_prefix(const JournalHeader &header) {
  std::ostringstream output;
  output << "{\n"
         << "  \"schemaVersion\": 1,\n"
         << "  \"sessionId\": \"" << header.session_id << "\",\n"
         << "  \"recordingId\": \"" << header.recording_id << "\",\n"
         << "  \"recordingProfileSha256\": \"" << header.profile_sha256 << "\",\n"
         << "  \"accessUnits\": [\n";
  return output.str();
}

std::string sidecar_records(std::span<const JournalAccessUnit> records, bool first_record) {
  std::ostringstream output;
  bool first = first_record;
  for (const auto &record : records) {
    if (!first) {
      output << ",\n";
    }
    first = false;
    output << "    {\"sessionId\":\"" << record.session_id << "\",\"recordingId\":\""
           << record.recording_id << "\",\"accessUnitIndex\":" << record.index
           << ",\"byteOffset\":" << record.offset << ",\"byteSize\":" << record.size
           << ",\"mediaEpoch\":" << record.media_epoch
           << ",\"dependencyEpoch\":" << record.dependency_epoch
           << ",\"geometryEpoch\":" << record.geometry_epoch
           << ",\"encoderConfigurationEpoch\":" << record.encoder_configuration_epoch
           << ",\"configurationSha256\":\"" << record.configuration_sha256
           << "\",\"sourceFrameId\":" << record.source_frame_id
           << ",\"extendedRtpTimestamp\":" << record.extended_rtp_timestamp << ",\"pictureType\":\""
           << (record.picture_type == RecordingPictureType::idr ? "IDR" : "P")
           << "\",\"keyframe\":" << (record.keyframe ? "true" : "false")
           << ",\"parameterSetsPresent\":" << (record.parameter_sets_present ? "true" : "false")
           << ",\"presentationTimestampNs\":" << record.presentation_timestamp_ns
           << ",\"payloadSha256\":\"" << sha256_hex(record.payload_sha256) << "\"}";
  }
  return output.str();
}

std::string sidecar_suffix(std::uint64_t access_units, std::uint64_t media_bytes,
                           std::string_view media_sha256) {
  std::ostringstream output;
  output << "\n  ],\n"
         << "  \"accessUnitCount\": " << access_units << ",\n"
         << "  \"mediaBytes\": " << media_bytes << ",\n"
         << "  \"mediaSha256\": \"" << media_sha256 << "\"\n"
         << "}\n";
  return output.str();
}

std::string completion_marker(const JournalHeader &header, std::uint64_t access_units,
                              std::uint64_t media_size, std::string_view media_sha256,
                              std::uint64_t sidecar_size, std::string_view sidecar_sha256) {
  std::ostringstream output;
  output << "glyphrelay-completion-v1\n"
         << "session_id=" << header.session_id << '\n'
         << "recording_id=" << header.recording_id << '\n'
         << "access_units=" << access_units << '\n'
         << "media_size=" << media_size << '\n'
         << "media_sha256=" << media_sha256 << '\n'
         << "sidecar_size=" << sidecar_size << '\n'
         << "sidecar_sha256=" << sidecar_sha256 << '\n';
  return output.str();
}

struct ParsedMarker {
  std::string session_id;
  std::string recording_id;
  std::uint64_t access_units = 0U;
  std::uint64_t media_size = 0U;
  std::string media_sha256;
  std::uint64_t sidecar_size = 0U;
  std::string sidecar_sha256;
};

std::optional<std::uint64_t> parse_decimal(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t result = 0U;
  for (const char character : value) {
    if (character < '0' || character > '9' ||
        result > (std::numeric_limits<std::uint64_t>::max() -
                  static_cast<std::uint64_t>(character - '0')) /
                     10U) {
      return std::nullopt;
    }
    result = result * 10U + static_cast<std::uint64_t>(character - '0');
  }
  return result;
}

std::optional<ParsedMarker> parse_marker(std::string_view marker) {
  std::vector<std::string_view> lines;
  std::size_t offset = 0U;
  while (offset < marker.size()) {
    const auto end = marker.find('\n', offset);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    lines.push_back(marker.substr(offset, end - offset));
    offset = end + 1U;
  }
  if (lines.size() != 8U || lines[0] != "glyphrelay-completion-v1") {
    return std::nullopt;
  }
  const auto field = [&lines](std::size_t index,
                              std::string_view name) -> std::optional<std::string_view> {
    const std::string prefix = std::string(name) + "=";
    if (!lines[index].starts_with(prefix)) {
      return std::nullopt;
    }
    return lines[index].substr(prefix.size());
  };
  const auto session = field(1U, "session_id");
  const auto recording = field(2U, "recording_id");
  const auto access_units = field(3U, "access_units");
  const auto media_size = field(4U, "media_size");
  const auto media_hash = field(5U, "media_sha256");
  const auto sidecar_size = field(6U, "sidecar_size");
  const auto sidecar_hash = field(7U, "sidecar_sha256");
  if (!session || !recording || !access_units || !media_size || !media_hash || !sidecar_size ||
      !sidecar_hash) {
    return std::nullopt;
  }
  const auto parsed_access_units = parse_decimal(*access_units);
  const auto parsed_media_size = parse_decimal(*media_size);
  const auto parsed_sidecar_size = parse_decimal(*sidecar_size);
  if (!valid_identifier(*session) || !valid_identifier(*recording) || !parsed_access_units ||
      !parsed_media_size || !parsed_sidecar_size || !valid_hex64(*media_hash) ||
      !valid_hex64(*sidecar_hash)) {
    return std::nullopt;
  }
  return ParsedMarker{std::string(*session),     std::string(*recording),  *parsed_access_units,
                      *parsed_media_size,        std::string(*media_hash), *parsed_sidecar_size,
                      std::string(*sidecar_hash)};
}

std::uint64_t steady_nanoseconds() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

} // namespace

struct DurableRecorder::Implementation {
  struct QueuedAccessUnit {
    RecordedAccessUnit access_unit;
    std::uint64_t index = 0U;
  };

  explicit Implementation(RecorderConfig input) : config(std::move(input)) { initialize(); }

  ~Implementation() {
    {
      std::scoped_lock lock(mutex);
      stop_requested = true;
    }
    condition.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }

  bool emit(RecordingEvent event) {
    if (!config.event_callback) {
      return true;
    }
    try {
      config.event_callback(event);
      return true;
    } catch (...) {
      return false;
    }
  }

  void initialize() {
    diagnostics.maximum_queue_bytes = config.maximum_queue_bytes;
    diagnostics.maximum_queue_age_ns = config.maximum_queue_age_ns;
    if (!valid_identifier(config.session_id) || !valid_hex64(config.recording_profile_sha256) ||
        config.maximum_queue_bytes == 0U || config.maximum_queue_bytes > 64U * 1024U * 1024U ||
        config.maximum_queue_age_ns == 0U || config.maximum_queue_age_ns > 2'000'000'000ULL ||
        config.group_commit_interval_ns == 0U || config.group_commit_interval_ns > 250'000'000ULL) {
      fail_initialization("RECORDER_CONFIGURATION_INVALID");
      return;
    }
    std::string path_reason;
    auto opened = open_output_parent(config.output_path, path_reason);
    if (!opened.directory) {
      fail_initialization(path_reason);
      return;
    }
    directory = std::move(opened.directory);
    parent_path = std::move(opened.parent);
    if (!probe_rename_noreplace(directory.get())) {
      fail_initialization("RENAME_NOREPLACE_UNAVAILABLE");
      return;
    }
    const auto identifier = random_hex(16U);
    if (!identifier) {
      fail_initialization("RECORDING_IDENTIFIER_GENERATION_FAILED");
      return;
    }
    header.version = 1U;
    header.session_id = config.session_id;
    header.recording_id = *identifier;
    header.profile_sha256 = config.recording_profile_sha256;
    header.names.final_media = opened.basename;
    header.names.final_sidecar = opened.basename + ".json";
    header.names.final_marker = opened.basename + ".complete";
    header.names.journal = opened.basename + ".journal";
    const std::string temporary_prefix = "." + opened.basename + "." + *identifier;
    header.names.temporary_media = temporary_prefix + ".media.tmp";
    header.names.temporary_sidecar = temporary_prefix + ".sidecar.tmp";
    header.names.temporary_marker = temporary_prefix + ".marker.tmp";
    const std::array<std::string_view, 7U> names = {
        header.names.final_media,      header.names.final_sidecar,   header.names.final_marker,
        header.names.journal,          header.names.temporary_media, header.names.temporary_sidecar,
        header.names.temporary_marker,
    };
    if (std::any_of(names.begin(), names.end(),
                    [](std::string_view name) { return !safe_filename(name); })) {
      fail_initialization("OUTPUT_PATH_UNSAFE");
      return;
    }
    if (path_exists(directory.get(), header.names.journal)) {
      fail_initialization("OUTPUT_INCOMPLETE_EXISTS");
      return;
    }
    if (path_exists(directory.get(), header.names.final_media) ||
        path_exists(directory.get(), header.names.final_sidecar) ||
        path_exists(directory.get(), header.names.final_marker)) {
      fail_initialization("OUTPUT_EXISTS");
      return;
    }
    if (path_exists(directory.get(), header.names.temporary_media) ||
        path_exists(directory.get(), header.names.temporary_sidecar) ||
        path_exists(directory.get(), header.names.temporary_marker)) {
      fail_initialization("OUTPUT_INCOMPLETE_EXISTS");
      return;
    }

    journal = create_regular_file(directory.get(), header.names.journal);
    if (!journal) {
      fail_initialization(errno == EEXIST ? "OUTPUT_INCOMPLETE_EXISTS"
                                          : "RECORDING_JOURNAL_CREATE_FAILED");
      return;
    }
    if (!emit(RecordingEvent::journal_created)) {
      fail_initialization("RECORDING_EVENT_CALLBACK_FAILED");
      return;
    }
    media = create_regular_file(directory.get(), header.names.temporary_media);
    if (!media || !emit(RecordingEvent::media_temporary_created)) {
      fail_initialization("RECORDING_MEDIA_CREATE_FAILED");
      return;
    }
    sidecar = create_regular_file(directory.get(), header.names.temporary_sidecar);
    if (!sidecar || !emit(RecordingEvent::sidecar_temporary_created)) {
      fail_initialization("RECORDING_SIDECAR_CREATE_FAILED");
      return;
    }
    marker = create_regular_file(directory.get(), header.names.temporary_marker);
    if (!marker || !emit(RecordingEvent::marker_temporary_created)) {
      fail_initialization("RECORDING_MARKER_CREATE_FAILED");
      return;
    }
    std::vector<std::uint8_t> header_bytes;
    try {
      header_bytes = encode_header(header);
    } catch (...) {
      fail_initialization("RECORDING_JOURNAL_HEADER_INVALID");
      return;
    }
    header.byte_size = header_bytes.size();
    if (!write_all(journal.get(), header_bytes) || !emit(RecordingEvent::journal_header_written) ||
        !sync_data(journal.get()) || !emit(RecordingEvent::journal_header_synced)) {
      fail_initialization("RECORDING_JOURNAL_PREPARE_FAILED");
      return;
    }
    if (!sync_file(media.get()) || !emit(RecordingEvent::media_temporary_synced) ||
        !sync_file(sidecar.get()) || !emit(RecordingEvent::sidecar_temporary_synced) ||
        !sync_file(marker.get()) || !emit(RecordingEvent::marker_temporary_synced) ||
        !sync_file(directory.get()) || !emit(RecordingEvent::prepared_directory_synced)) {
      fail_initialization("RECORDING_PREPARED_BARRIER_FAILED");
      return;
    }
    ready_state = true;
    diagnostics.ready = true;
    diagnostics.reason = "RECORDER_READY";
    diagnostics.recording_id = header.recording_id;
    worker = std::thread([this]() { run(); });
  }

  void fail_initialization(std::string reason) {
    ready_state = false;
    failed_state = true;
    failure_reason = std::move(reason);
    diagnostics.failed = true;
    diagnostics.reason = failure_reason;
  }

  void fail_runtime(std::string reason) {
    std::scoped_lock lock(mutex);
    if (!failed_state) {
      failed_state = true;
      failure_reason = std::move(reason);
      diagnostics.failed = true;
      diagnostics.reason = failure_reason;
    }
    queue.clear();
    queued_bytes = 0U;
    diagnostics.queue_access_units = 0U;
    diagnostics.queue_bytes = 0U;
  }

  bool write_access_unit(const QueuedAccessUnit &queued) {
    const auto &access_unit = queued.access_unit;
    const auto offset = media_offset;
    if (!write_all(media.get(), *access_unit.bytes) ||
        !emit(RecordingEvent::media_access_unit_written)) {
      fail_runtime("RECORDER_MEDIA_WRITE_FAILED");
      return false;
    }
    media_hash.update(*access_unit.bytes);
    media_offset += access_unit.bytes->size();
    JournalAccessUnit record;
    record.index = queued.index;
    record.offset = offset;
    record.size = access_unit.bytes->size();
    record.session_id = header.session_id;
    record.recording_id = header.recording_id;
    record.media_epoch = access_unit.media_epoch;
    record.dependency_epoch = access_unit.dependency_epoch;
    record.geometry_epoch = access_unit.geometry_epoch;
    record.encoder_configuration_epoch = access_unit.encoder_configuration_epoch;
    record.configuration_sha256 = access_unit.configuration_sha256;
    record.source_frame_id = access_unit.source_frame_id;
    record.extended_rtp_timestamp = access_unit.extended_rtp_timestamp;
    record.picture_type = access_unit.picture_type;
    record.keyframe = access_unit.keyframe;
    record.parameter_sets_present = access_unit.parameter_sets_present;
    record.presentation_timestamp_ns = access_unit.presentation_timestamp_ns;
    record.payload_sha256 = Sha256::digest(*access_unit.bytes);
    pending_records.push_back(std::move(record));
    if (pending_since_ns == 0U) {
      pending_since_ns = steady_nanoseconds();
    }
    return true;
  }

  bool commit_group() {
    if (pending_records.empty()) {
      return true;
    }
    const auto started = steady_nanoseconds();
    if (!sync_data(media.get()) || !emit(RecordingEvent::media_group_synced)) {
      fail_runtime("RECORDER_MEDIA_SYNC_FAILED");
      return false;
    }
    for (const auto &record : pending_records) {
      std::vector<std::uint8_t> encoded;
      try {
        encoded = encode_access_unit_record(record);
      } catch (...) {
        fail_runtime("RECORDER_JOURNAL_RECORD_INVALID");
        return false;
      }
      if (!write_all(journal.get(), encoded)) {
        fail_runtime("RECORDER_JOURNAL_WRITE_FAILED");
        return false;
      }
    }
    const auto &last = pending_records.back();
    const auto commit = encode_commit_record(last.index, media_offset);
    if (!write_all(journal.get(), commit) || !emit(RecordingEvent::journal_group_written) ||
        !sync_data(journal.get()) || !emit(RecordingEvent::journal_group_synced)) {
      fail_runtime("RECORDER_JOURNAL_SYNC_FAILED");
      return false;
    }
    {
      std::scoped_lock lock(mutex);
      diagnostics.committed_access_units = last.index + 1U;
      diagnostics.committed_media_bytes = media_offset;
    }
    std::string sidecar_content;
    if (!sidecar_started) {
      sidecar_content = sidecar_prefix(header);
    }
    sidecar_content += sidecar_records(pending_records, !sidecar_has_records);
    const auto sidecar_bytes = std::span(
        reinterpret_cast<const std::uint8_t *>(sidecar_content.data()), sidecar_content.size());
    if (!write_all(sidecar.get(), sidecar_bytes) || !emit(RecordingEvent::sidecar_group_written) ||
        !sync_data(sidecar.get()) || !emit(RecordingEvent::sidecar_group_synced)) {
      fail_runtime("RECORDER_SIDECAR_GROUP_FAILED");
      return false;
    }
    sidecar_hash.update(sidecar_bytes);
    sidecar_size += sidecar_content.size();
    sidecar_started = true;
    sidecar_has_records = true;
    if (steady_nanoseconds() - started > kMaximumCommitDelayNs ||
        (pending_since_ns != 0U &&
         steady_nanoseconds() - pending_since_ns > kMaximumCommitDelayNs)) {
      fail_runtime("RECORDER_COMMIT_DEADLINE_EXCEEDED");
      return false;
    }
    last_committed_presentation_timestamp_ns = last.presentation_timestamp_ns;
    has_committed_group = true;
    pending_records.clear();
    pending_since_ns = 0U;
    last_commit_ns = steady_nanoseconds();
    return true;
  }

  bool publish() {
    if (!commit_group()) {
      return false;
    }
    media_hex = sha256_hex(media_hash.finalize());
    const auto sidecar_content = sidecar_suffix(diagnostics.committed_access_units,
                                                diagnostics.committed_media_bytes, media_hex);
    const auto sidecar_bytes = std::span(
        reinterpret_cast<const std::uint8_t *>(sidecar_content.data()), sidecar_content.size());
    if (!write_all(sidecar.get(), sidecar_bytes) || !emit(RecordingEvent::sidecar_written) ||
        !sync_data(sidecar.get()) || !emit(RecordingEvent::sidecar_synced)) {
      fail_runtime("RECORDER_SIDECAR_WRITE_FAILED");
      return false;
    }
    sidecar_hash.update(sidecar_bytes);
    sidecar_size += sidecar_content.size();
    const auto sidecar_hex = sha256_hex(sidecar_hash.finalize());
    const auto marker_content =
        completion_marker(header, diagnostics.committed_access_units,
                          diagnostics.committed_media_bytes, media_hex, sidecar_size, sidecar_hex);
    media.reset();
    sidecar.reset();
    if (!rename_noreplace(directory.get(), header.names.temporary_media,
                          header.names.final_media) ||
        !emit(RecordingEvent::media_renamed) ||
        !rename_noreplace(directory.get(), header.names.temporary_sidecar,
                          header.names.final_sidecar) ||
        !emit(RecordingEvent::sidecar_renamed) || !sync_file(directory.get()) ||
        !emit(RecordingEvent::publication_directory_synced)) {
      fail_runtime("RECORDER_PUBLICATION_FAILED");
      return false;
    }
    if (::ftruncate(marker.get(), 0) != 0 ||
        !write_all(marker.get(),
                   std::span(reinterpret_cast<const std::uint8_t *>(marker_content.data()),
                             marker_content.size())) ||
        !emit(RecordingEvent::marker_written) || !sync_data(marker.get()) ||
        !emit(RecordingEvent::marker_synced)) {
      fail_runtime("RECORDER_MARKER_WRITE_FAILED");
      return false;
    }
    marker.reset();
    if (!rename_noreplace(directory.get(), header.names.temporary_marker,
                          header.names.final_marker) ||
        !emit(RecordingEvent::marker_renamed) || !sync_file(directory.get()) ||
        !emit(RecordingEvent::marker_directory_synced)) {
      fail_runtime("RECORDER_MARKER_PUBLICATION_FAILED");
      return false;
    }
    journal.reset();
    if (!unlink_if_present(directory.get(), header.names.journal) ||
        !emit(RecordingEvent::journal_removed) || !sync_file(directory.get()) ||
        !emit(RecordingEvent::cleanup_directory_synced)) {
      fail_runtime("RECORDER_JOURNAL_CLEANUP_FAILED");
      return false;
    }
    std::scoped_lock lock(mutex);
    completed_state = true;
    diagnostics.completed = true;
    diagnostics.reason = "RECORDER_COMPLETED";
    return true;
  }

  void run() {
    last_commit_ns = steady_nanoseconds();
    while (true) {
      std::optional<QueuedAccessUnit> next;
      bool should_stop = false;
      bool should_publish = false;
      bool should_commit_failure = false;
      {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, std::chrono::milliseconds(20),
                           [this]() { return stop_requested || failed_state || !queue.empty(); });
        if (!queue.empty() && !failed_state) {
          next = std::move(queue.front());
          queued_bytes -= next->access_unit.bytes->size();
          queue.pop_front();
          diagnostics.queue_access_units = queue.size();
          diagnostics.queue_bytes = queued_bytes;
        } else if (failed_state || stop_requested) {
          should_stop = true;
          should_publish = publish_requested && !failed_state;
          should_commit_failure = failed_state && commit_pending_on_failure;
        }
      }
      if (next) {
        const bool configuration_changed = last_written_encoder_configuration_epoch != 0U &&
                                           next->access_unit.encoder_configuration_epoch !=
                                               last_written_encoder_configuration_epoch;
        last_written_encoder_configuration_epoch = next->access_unit.encoder_configuration_epoch;
        if (!write_access_unit(*next)) {
          continue;
        }
        const bool sender_time_due =
            has_committed_group && next->access_unit.presentation_timestamp_ns -
                                           last_committed_presentation_timestamp_ns >=
                                       config.group_commit_interval_ns;
        const bool immediate = next->access_unit.keyframe || configuration_changed;
        if (immediate || sender_time_due ||
            steady_nanoseconds() - last_commit_ns >= config.group_commit_interval_ns) {
          static_cast<void>(commit_group());
        }
        continue;
      }
      if (!pending_records.empty() &&
          steady_nanoseconds() - last_commit_ns >= config.group_commit_interval_ns) {
        if (!commit_group()) {
          continue;
        }
      }
      if (should_stop) {
        bool can_finish = false;
        {
          std::scoped_lock lock(mutex);
          can_finish = !failed_state;
        }
        if (can_finish || should_commit_failure) {
          static_cast<void>(commit_group());
        }
        {
          std::scoped_lock lock(mutex);
          can_finish = !failed_state;
        }
        if (should_publish && can_finish) {
          static_cast<void>(publish());
        }
        break;
      }
    }
    condition.notify_all();
  }

  RecorderConfig config;
  UniqueFd directory;
  UniqueFd journal;
  UniqueFd media;
  UniqueFd sidecar;
  UniqueFd marker;
  std::filesystem::path parent_path;
  JournalHeader header;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::thread worker;
  std::deque<QueuedAccessUnit> queue;
  std::vector<JournalAccessUnit> pending_records;
  std::size_t queued_bytes = 0U;
  std::uint64_t next_access_unit_index = 0U;
  std::uint64_t media_offset = 0U;
  std::uint64_t last_presentation_timestamp_ns = 0U;
  std::uint64_t last_committed_presentation_timestamp_ns = 0U;
  std::uint64_t last_written_encoder_configuration_epoch = 0U;
  std::uint64_t pending_since_ns = 0U;
  std::uint64_t last_commit_ns = 0U;
  Sha256 media_hash;
  Sha256 sidecar_hash;
  std::uint64_t sidecar_size = 0U;
  std::string media_hex;
  RecorderDiagnostics diagnostics;
  std::string failure_reason;
  bool ready_state = false;
  bool failed_state = false;
  bool completed_state = false;
  bool stop_requested = false;
  bool publish_requested = false;
  bool recovery_admitted = false;
  bool sidecar_started = false;
  bool sidecar_has_records = false;
  bool has_committed_group = false;
  bool commit_pending_on_failure = false;
};

DurableRecorder::DurableRecorder(RecorderConfig config)
    : implementation_(std::make_unique<Implementation>(std::move(config))) {}

DurableRecorder::~DurableRecorder() = default;
DurableRecorder::DurableRecorder(DurableRecorder &&) noexcept = default;
DurableRecorder &DurableRecorder::operator=(DurableRecorder &&) noexcept = default;

bool DurableRecorder::ready() const {
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->ready_state && !implementation_->failed_state &&
         !implementation_->completed_state;
}

std::string DurableRecorder::initialization_reason() const {
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->diagnostics.reason;
}

RecorderEnqueueResult DurableRecorder::enqueue(RecordedAccessUnit access_unit) {
  const auto fail_access_unit = [this](std::string reason) {
    auto &implementation = *implementation_;
    {
      std::scoped_lock lock(implementation.mutex);
      if (implementation.ready_state && !implementation.failed_state &&
          !implementation.completed_state && !implementation.stop_requested) {
        implementation.failed_state = true;
        implementation.commit_pending_on_failure = true;
        implementation.failure_reason = reason;
        implementation.diagnostics.failed = true;
        implementation.diagnostics.reason = reason;
      } else if (implementation.failed_state) {
        reason = implementation.failure_reason;
      }
    }
    implementation.condition.notify_all();
    return RecorderEnqueueResult{false, true, std::move(reason)};
  };
  if (!access_unit.bytes || access_unit.bytes->empty() ||
      access_unit.bytes->size() > kMaximumAccessUnitBytes || access_unit.media_epoch == 0U ||
      access_unit.dependency_epoch == 0U || access_unit.geometry_epoch == 0U ||
      access_unit.encoder_configuration_epoch == 0U || access_unit.source_frame_id == 0U ||
      !valid_hex64(access_unit.configuration_sha256)) {
    return fail_access_unit("RECORDER_ACCESS_UNIT_INVALID");
  }
  const auto parsed = parse_annex_b_access_unit(*access_unit.bytes);
  if (!parsed.passed) {
    return fail_access_unit("RECORDER_ACCESS_UNIT_" + parsed.reason);
  }
  const bool keyframe = parsed.access_unit.contains(5U);
  const bool parameter_sets = parsed.access_unit.contains(7U) && parsed.access_unit.contains(8U);
  if (access_unit.keyframe != keyframe || access_unit.parameter_sets_present != parameter_sets ||
      (keyframe && access_unit.picture_type != RecordingPictureType::idr) ||
      (!keyframe && access_unit.picture_type != RecordingPictureType::predicted) ||
      (keyframe && !parsed.access_unit.starts_with_parameter_sets_and_idr())) {
    return fail_access_unit("RECORDER_ACCESS_UNIT_METADATA_MISMATCH");
  }

  std::scoped_lock lock(implementation_->mutex);
  if (!implementation_->ready_state || implementation_->failed_state ||
      implementation_->stop_requested || implementation_->completed_state) {
    return {false, implementation_->failed_state,
            implementation_->failed_state ? implementation_->failure_reason
                                          : "RECORDER_ADMISSION_CLOSED"};
  }
  if (!implementation_->recovery_admitted) {
    if (!keyframe || !parameter_sets) {
      return {false, false, "RECORDER_AWAITING_RECOVERY_IDR"};
    }
    implementation_->recovery_admitted = true;
  }
  if (implementation_->next_access_unit_index != 0U &&
      access_unit.presentation_timestamp_ns <= implementation_->last_presentation_timestamp_ns) {
    implementation_->failed_state = true;
    implementation_->commit_pending_on_failure = true;
    implementation_->failure_reason = "RECORDER_PRESENTATION_TIMESTAMP_INVALID";
    implementation_->diagnostics.failed = true;
    implementation_->diagnostics.reason = implementation_->failure_reason;
    implementation_->condition.notify_all();
    return {false, true, implementation_->failure_reason};
  }
  std::size_t prospective_bytes = 0U;
  const bool byte_overflow =
      !checked_add(implementation_->queued_bytes, access_unit.bytes->size(), prospective_bytes);
  const bool age_overflow =
      !implementation_->queue.empty() &&
      access_unit.presentation_timestamp_ns -
              implementation_->queue.front().access_unit.presentation_timestamp_ns >
          implementation_->config.maximum_queue_age_ns;
  if (byte_overflow || prospective_bytes > implementation_->config.maximum_queue_bytes ||
      age_overflow || implementation_->queue.size() == kMaximumQueuedAccessUnits) {
    implementation_->failed_state = true;
    implementation_->commit_pending_on_failure = true;
    implementation_->failure_reason = "RECORDER_QUEUE_OVERLOADED";
    implementation_->diagnostics.failed = true;
    implementation_->diagnostics.reason = implementation_->failure_reason;
    ++implementation_->diagnostics.overload_failures;
    implementation_->queue.clear();
    implementation_->queued_bytes = 0U;
    implementation_->diagnostics.queue_access_units = 0U;
    implementation_->diagnostics.queue_bytes = 0U;
    implementation_->condition.notify_all();
    return {false, true, implementation_->failure_reason};
  }
  const auto index = implementation_->next_access_unit_index++;
  implementation_->last_presentation_timestamp_ns = access_unit.presentation_timestamp_ns;
  implementation_->queued_bytes = prospective_bytes;
  implementation_->queue.push_back({std::move(access_unit), index});
  ++implementation_->diagnostics.accepted_access_units;
  implementation_->diagnostics.queue_access_units = implementation_->queue.size();
  implementation_->diagnostics.queue_bytes = implementation_->queued_bytes;
  implementation_->condition.notify_one();
  return {true, false, "RECORDER_ACCESS_UNIT_ACCEPTED"};
}

RecorderFinalizeResult DurableRecorder::finalize() {
  auto &implementation = *implementation_;
  {
    std::scoped_lock lock(implementation.mutex);
    if (!implementation.ready_state) {
      return {false, implementation.diagnostics.reason};
    }
    if (implementation.completed_state) {
      return {true, "RECORDER_COMPLETED"};
    }
    if (implementation.failed_state) {
      implementation.stop_requested = true;
    } else if (implementation.next_access_unit_index == 0U) {
      implementation.failed_state = true;
      implementation.failure_reason = "RECORDER_EMPTY";
      implementation.diagnostics.failed = true;
      implementation.diagnostics.reason = implementation.failure_reason;
    } else {
      implementation.publish_requested = true;
    }
    implementation.stop_requested = true;
  }
  implementation.condition.notify_all();
  if (implementation.worker.joinable()) {
    implementation.worker.join();
  }
  implementation.media.reset();
  implementation.sidecar.reset();
  implementation.marker.reset();
  implementation.journal.reset();
  implementation.directory.reset();
  std::scoped_lock lock(implementation.mutex);
  return {implementation.completed_state,
          implementation.completed_state ? "RECORDER_COMPLETED" : implementation.failure_reason};
}

RecorderDiagnostics DurableRecorder::diagnostics() const {
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->diagnostics;
}

RecordingInspection inspect_recording(const std::filesystem::path &recording_path) {
  RecordingInspection output;
  std::string reason;
  auto opened = open_output_parent(recording_path, reason);
  if (!opened.directory) {
    output.state = RecordingInspectionState::corrupt;
    output.reason = reason;
    return output;
  }
  const std::string media_name = opened.basename;
  const std::string sidecar_name = opened.basename + ".json";
  const std::string marker_name = opened.basename + ".complete";
  const std::string journal_name = opened.basename + ".journal";
  output.media_path = opened.parent / media_name;
  output.sidecar_path = opened.parent / sidecar_name;
  output.marker_path = opened.parent / marker_name;

  if (path_exists(opened.directory.get(), marker_name)) {
    const auto marker_fd = open_regular_readonly(opened.directory.get(), marker_name);
    const auto media_fd = open_regular_readonly(opened.directory.get(), media_name);
    const auto sidecar_fd = open_regular_readonly(opened.directory.get(), sidecar_name);
    if (!marker_fd || !media_fd || !sidecar_fd) {
      output.state = RecordingInspectionState::corrupt;
      output.reason = "RECORDING_COMPLETE_COMPANION_INVALID";
      return output;
    }
    const auto marker_bytes = read_all(marker_fd->get(), 65536U);
    const auto media_size = file_size(media_fd->get());
    const auto sidecar_size = file_size(sidecar_fd->get());
    const auto media_hash = hash_fd_hex(media_fd->get());
    const auto sidecar_hash = hash_fd_hex(sidecar_fd->get());
    if (!marker_bytes || !media_size || !sidecar_size || !media_hash || !sidecar_hash) {
      output.state = RecordingInspectionState::corrupt;
      output.reason = "RECORDING_COMPLETE_HASH_FAILED";
      return output;
    }
    const auto marker = parse_marker(std::string_view(
        reinterpret_cast<const char *>(marker_bytes->data()), marker_bytes->size()));
    if (!marker || marker->media_size != *media_size || marker->sidecar_size != *sidecar_size ||
        marker->media_sha256 != *media_hash || marker->sidecar_sha256 != *sidecar_hash) {
      output.state = RecordingInspectionState::corrupt;
      output.reason = "RECORDING_COMPLETION_MARKER_INVALID";
      return output;
    }
    output.passed = true;
    output.state = RecordingInspectionState::complete;
    output.reason = "RECORDING_COMPLETE";
    output.session_id = marker->session_id;
    output.recording_id = marker->recording_id;
    output.committed_access_units = marker->access_units;
    output.committed_media_bytes = *media_size;
    return output;
  }

  if (!path_exists(opened.directory.get(), journal_name)) {
    if (path_exists(opened.directory.get(), media_name) ||
        path_exists(opened.directory.get(), sidecar_name)) {
      output.state = RecordingInspectionState::incomplete;
      output.reason = "RECORDING_FINAL_WITHOUT_MARKER";
    } else {
      output.state = RecordingInspectionState::absent;
      output.reason = "RECORDING_ABSENT";
    }
    return output;
  }
  const auto journal_fd = open_regular_readonly(opened.directory.get(), journal_name);
  if (!journal_fd) {
    output.state = RecordingInspectionState::corrupt;
    output.reason = "RECORDING_JOURNAL_UNSAFE";
    return output;
  }
  const auto journal_size = file_size(journal_fd->get());
  if (!journal_size) {
    output.state = RecordingInspectionState::corrupt;
    output.reason = "RECORDING_JOURNAL_READ_FAILED";
    return output;
  }
  if (*journal_size < kJournalMagic.size() + 12U + Sha256::Digest{}.size()) {
    output.passed = true;
    output.state = RecordingInspectionState::prepared_incomplete;
    output.reason = "RECORDING_JOURNAL_HEADER_INCOMPLETE";
    return output;
  }
  const auto header = read_journal_header(journal_fd->get(), *journal_size);
  if (!header || header->names.journal != journal_name || header->names.final_media != media_name ||
      header->names.final_sidecar != sidecar_name || header->names.final_marker != marker_name) {
    output.state = RecordingInspectionState::corrupt;
    output.reason = "RECORDING_JOURNAL_HEADER_INVALID";
    return output;
  }
  output.session_id = header->session_id;
  output.recording_id = header->recording_id;
  const bool temporary_media_exists =
      path_exists(opened.directory.get(), header->names.temporary_media);
  const bool final_media_exists = path_exists(opened.directory.get(), media_name);
  if (temporary_media_exists && final_media_exists) {
    output.state = RecordingInspectionState::corrupt;
    output.reason = "RECORDING_MEDIA_STATE_AMBIGUOUS";
    return output;
  }
  const bool temporary_sidecar_exists =
      path_exists(opened.directory.get(), header->names.temporary_sidecar);
  const bool final_sidecar_exists = path_exists(opened.directory.get(), sidecar_name);
  if (temporary_sidecar_exists && final_sidecar_exists) {
    output.state = RecordingInspectionState::corrupt;
    output.reason = "RECORDING_SIDECAR_STATE_AMBIGUOUS";
    return output;
  }
  if (!temporary_media_exists && !final_media_exists) {
    if (*journal_size != header->byte_size) {
      output.state = RecordingInspectionState::corrupt;
      output.reason = "RECORDING_COMMITTED_MEDIA_MISSING";
      return output;
    }
    output.passed = true;
    output.state = RecordingInspectionState::prepared_incomplete;
    output.reason = "RECORDING_PREPARED_COMPANION_INCOMPLETE";
    return output;
  }
  const auto media_fd = open_regular_readonly(opened.directory.get(),
                                              temporary_media_exists ? header->names.temporary_media
                                                                     : header->names.final_media);
  if (!media_fd) {
    output.state = RecordingInspectionState::corrupt;
    output.reason = "RECORDING_COMMITTED_MEDIA_MISSING";
    return output;
  }
  const auto size = file_size(media_fd->get());
  if (!size) {
    output.state = RecordingInspectionState::corrupt;
    output.reason = "RECORDING_COMMITTED_RANGE_INVALID";
    return output;
  }
  const auto journal =
      parse_journal(journal_fd->get(), *journal_size, media_fd->get(), *size, *header);
  if (!journal) {
    output.state = RecordingInspectionState::corrupt;
    output.reason = "RECORDING_JOURNAL_OR_MEDIA_INVALID";
    return output;
  }
  if (journal->committed_count == 0U) {
    output.passed = true;
    output.state = RecordingInspectionState::prepared_incomplete;
    output.reason = "RECORDING_PREPARED_INCOMPLETE";
    return output;
  }
  output.passed = true;
  output.state = RecordingInspectionState::incomplete;
  output.reason = "RECORDING_INCOMPLETE_AT_DURABLE_BOUNDARY";
  output.committed_access_units = journal->committed_count;
  output.committed_media_bytes = journal->committed_bytes;
  return output;
}

bool durable_recording_available() { return true; }

} // namespace glyphrelay
