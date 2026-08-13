#pragma once

#include <string_view>

namespace glyphrelay::dependency_versions {

inline constexpr std::string_view nvenc_header_version = "13.1.15.0";
inline constexpr std::string_view nvenc_header_commit = "0a6fba9a2820628b8103464f4c8753ee05838baa";
inline constexpr std::string_view nvenc_header_sha256 =
    "8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228";
inline constexpr std::string_view nvenc_header_license = "MIT";
inline constexpr std::string_view nvenc_minimum_linux_driver = "610.0";
inline constexpr int nvenc_api_major = 13;
inline constexpr int nvenc_api_minor = 1;

inline constexpr std::string_view libdatachannel_version = "0.24.1";
inline constexpr std::string_view libdatachannel_commit =
    "a02b751917ac8afc8c58dc6f4461d25ff9465d48";

inline constexpr std::string_view openh264_version = "2.4.1+dfsg-1";
inline constexpr std::string_view playwright_version = "1.62.1";

} // namespace glyphrelay::dependency_versions
