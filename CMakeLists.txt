cmake_minimum_required(VERSION 3.28)

project(GlyphRelay VERSION 0.1.0 LANGUAGES CXX)

option(GLYPHRELAY_ENABLE_CUDA "Enable CUDA when a CUDA compiler is available" ON)
option(GLYPHRELAY_ENABLE_SANITIZERS "Enable supported sanitizers" OFF)
option(GLYPHRELAY_OPTIMIZE_PROTOCOL_CHECKS "Optimize deterministic protocol hashing" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CheckLanguage)
if(GLYPHRELAY_ENABLE_CUDA AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    set(GLYPHRELAY_HAS_CUDA_COMPILER 1)
  else()
    set(GLYPHRELAY_HAS_CUDA_COMPILER 0)
  endif()
else()
  set(GLYPHRELAY_HAS_CUDA_COMPILER 0)
endif()

set(GLYPHRELAY_HAS_CUDA_DRIVER 0)
set(GLYPHRELAY_HAS_NVENC 0)
set(GLYPHRELAY_NV_CODEC_HEADERS_ROOT "" CACHE PATH
    "Root of the exact pinned nv-codec-headers checkout")
if(GLYPHRELAY_HAS_CUDA_COMPILER)
  find_package(CUDAToolkit REQUIRED)
  set(GLYPHRELAY_HAS_CUDA_DRIVER 1)
  find_path(GLYPHRELAY_NVENC_INCLUDE_DIR NAMES ffnvcodec/nvEncodeAPI.h
    PATHS "${GLYPHRELAY_NV_CODEC_HEADERS_ROOT}/include"
    NO_DEFAULT_PATH)
  if(NOT GLYPHRELAY_NVENC_INCLUDE_DIR)
    message(FATAL_ERROR
      "linux-gpu requires nv-codec-headers n13.1.15.0 at GLYPHRELAY_NV_CODEC_HEADERS_ROOT")
  endif()
  file(SHA256 "${GLYPHRELAY_NVENC_INCLUDE_DIR}/ffnvcodec/nvEncodeAPI.h"
       GLYPHRELAY_NVENC_HEADER_ACTUAL_SHA256)
  set(GLYPHRELAY_NVENC_HEADER_EXPECTED_SHA256
      "8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228")
  if(NOT GLYPHRELAY_NVENC_HEADER_ACTUAL_SHA256 STREQUAL
         GLYPHRELAY_NVENC_HEADER_EXPECTED_SHA256)
    message(FATAL_ERROR "nvEncodeAPI.h does not match the locked n13.1.15.0 header")
  endif()
  set(GLYPHRELAY_HAS_NVENC 1)
endif()

find_path(GLYPHRELAY_OPENH264_INCLUDE_DIR NAMES wels/codec_api.h)
find_library(GLYPHRELAY_OPENH264_LIBRARY NAMES openh264)
if(GLYPHRELAY_OPENH264_INCLUDE_DIR AND GLYPHRELAY_OPENH264_LIBRARY)
  set(GLYPHRELAY_HAS_OPENH264 1)
  message(STATUS "System OpenH264 adapter enabled: ${GLYPHRELAY_OPENH264_LIBRARY}")
else()
  set(GLYPHRELAY_HAS_OPENH264 0)
  message(STATUS "System OpenH264 adapter unavailable")
endif()

add_library(glyphrelay_core
  src/app/doctor.cpp
  src/app/doctor_probes.cpp
  src/core/m0_protocol.cpp
  src/core/quality_metrics.cpp
  src/core/sha256.cpp
  src/core/synthetic_source.cpp
  src/gpu/cuda_context.cpp
  src/gpu/gpu_contracts.cpp
  src/gpu/nvenc_probe.cpp
  src/media/annex_b.cpp
  src/media/h264_sps.cpp
  src/media/i420.cpp
  src/media/openh264_encoder.cpp
  src/media/recording_profile.cpp
  src/transport/media_egress.cpp
)
target_include_directories(glyphrelay_core PUBLIC include)
target_compile_definitions(glyphrelay_core PUBLIC
  GLYPHRELAY_VERSION="${PROJECT_VERSION}"
  GLYPHRELAY_HAS_CUDA_COMPILER=${GLYPHRELAY_HAS_CUDA_COMPILER}
  GLYPHRELAY_HAS_CUDA_DRIVER=${GLYPHRELAY_HAS_CUDA_DRIVER}
  GLYPHRELAY_CUDA_COMPILER_VERSION="${CMAKE_CUDA_COMPILER_VERSION}"
  GLYPHRELAY_HAS_NVENC=${GLYPHRELAY_HAS_NVENC}
  GLYPHRELAY_HAS_OPENH264=${GLYPHRELAY_HAS_OPENH264}
)
if(GLYPHRELAY_HAS_CUDA_DRIVER)
  target_link_libraries(glyphrelay_core PRIVATE CUDA::cuda_driver)
endif()
if(GLYPHRELAY_HAS_NVENC)
  target_include_directories(glyphrelay_core PRIVATE ${GLYPHRELAY_NVENC_INCLUDE_DIR})
endif()
if(GLYPHRELAY_HAS_OPENH264)
  target_include_directories(glyphrelay_core PRIVATE ${GLYPHRELAY_OPENH264_INCLUDE_DIR})
  target_link_libraries(glyphrelay_core PRIVATE ${GLYPHRELAY_OPENH264_LIBRARY})
endif()

if(MSVC)
  target_compile_options(glyphrelay_core PRIVATE /W4 /WX /permissive-)
else()
  target_compile_options(glyphrelay_core PRIVATE
    -Wall
    -Wconversion
    -Werror
    -Wextra
    -Wpedantic
    -Wshadow
  )
  if(GLYPHRELAY_OPTIMIZE_PROTOCOL_CHECKS)
    set_source_files_properties(
      src/core/sha256.cpp
      src/core/synthetic_source.cpp
      PROPERTIES COMPILE_OPTIONS "-O3"
    )
  endif()
endif()

if(GLYPHRELAY_ENABLE_SANITIZERS AND NOT MSVC)
  target_compile_options(glyphrelay_core PUBLIC -fno-omit-frame-pointer -fsanitize=address,undefined)
  target_link_options(glyphrelay_core PUBLIC -fno-omit-frame-pointer -fsanitize=address,undefined)
endif()

if(CMAKE_DL_LIBS)
  target_link_libraries(glyphrelay_core PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(glyphrelay src/app/main.cpp)
target_link_libraries(glyphrelay PRIVATE glyphrelay_core)

include(CTest)
if(BUILD_TESTING)
  add_executable(glyphrelay_native_tests tests/native/test_doctor.cpp)
  target_link_libraries(glyphrelay_native_tests PRIVATE glyphrelay_core)
  add_test(NAME native.doctor COMMAND glyphrelay_native_tests)
  add_test(NAME cli.help COMMAND glyphrelay --help)
  add_test(NAME cli.doctor_json COMMAND glyphrelay doctor --json)

  add_executable(glyphrelay_m0_protocol_tests tests/native/test_m0_protocol.cpp)
  target_link_libraries(glyphrelay_m0_protocol_tests PRIVATE glyphrelay_core)
  target_compile_definitions(glyphrelay_m0_protocol_tests PRIVATE
    GLYPHRELAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    GLYPHRELAY_M0_PROTOCOL_SHA256="3428958bf30b487e34c106614f83b59fe2526cfd46e1beb9a9249b70f2b1c717"
  )
  add_test(NAME native.m0_protocol COMMAND glyphrelay_m0_protocol_tests)

  add_executable(glyphrelay_media_contract_tests tests/native/test_media_contracts.cpp)
  target_link_libraries(glyphrelay_media_contract_tests PRIVATE glyphrelay_core)
  add_test(NAME native.media_contracts COMMAND glyphrelay_media_contract_tests)

  add_executable(glyphrelay_gpu_contract_tests tests/native/test_gpu_contracts.cpp)
  target_link_libraries(glyphrelay_gpu_contract_tests PRIVATE glyphrelay_core)
  add_test(NAME native.gpu_contracts COMMAND glyphrelay_gpu_contract_tests)

  add_executable(glyphrelay_egress_contract_tests tests/native/test_media_egress.cpp)
  target_link_libraries(glyphrelay_egress_contract_tests PRIVATE glyphrelay_core)
  add_test(NAME native.media_egress COMMAND glyphrelay_egress_contract_tests)

  if(GLYPHRELAY_HAS_OPENH264)
    add_executable(glyphrelay_openh264_integration tests/native/test_openh264_integration.cpp)
    target_link_libraries(glyphrelay_openh264_integration PRIVATE glyphrelay_core)
    set(GLYPHRELAY_OPENH264_TEST_STREAM
        "${CMAKE_CURRENT_BINARY_DIR}/openh264-record-only.h264")
    add_test(NAME integration.openh264_record_only
      COMMAND glyphrelay_openh264_integration "${GLYPHRELAY_OPENH264_TEST_STREAM}")
    find_program(GLYPHRELAY_FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)
    add_test(NAME integration.ffmpeg_decodes_openh264
      COMMAND "${GLYPHRELAY_FFMPEG_EXECUTABLE}" -v error -f h264
              -i "${GLYPHRELAY_OPENH264_TEST_STREAM}" -f null -)
    set_tests_properties(integration.openh264_record_only PROPERTIES
      FIXTURES_SETUP openh264_record_only_stream)
    set_tests_properties(integration.ffmpeg_decodes_openh264 PROPERTIES
      DEPENDS integration.openh264_record_only
      FIXTURES_REQUIRED openh264_record_only_stream)
  endif()

  add_executable(glyphrelay_freeze_m0_protocol tools/freeze_m0_protocol.cpp)
  target_link_libraries(glyphrelay_freeze_m0_protocol PRIVATE glyphrelay_core)

  if(GLYPHRELAY_HAS_NVENC)
    add_executable(glyphrelay_probe_nvenc tools/probe_nvenc.cpp)
    target_link_libraries(glyphrelay_probe_nvenc PRIVATE glyphrelay_core)
  endif()
endif()

install(TARGETS glyphrelay RUNTIME DESTINATION bin)
