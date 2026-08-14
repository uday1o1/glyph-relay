cmake_minimum_required(VERSION 3.28)

project(GlyphRelay VERSION 0.1.0 LANGUAGES CXX)

option(GLYPHRELAY_ENABLE_CUDA "Enable CUDA when a CUDA compiler is available" ON)
option(GLYPHRELAY_ENABLE_SANITIZERS "Enable supported sanitizers" OFF)
option(GLYPHRELAY_OPTIMIZE_PROTOCOL_CHECKS "Optimize deterministic protocol hashing" ON)
option(GLYPHRELAY_ENABLE_WEBRTC_CONTRACTS
       "Build the exact patched libdatachannel transport contracts" OFF)
set(GLYPHRELAY_LIBDATACHANNEL_ROOT "" CACHE PATH
    "Root of the exact patched libdatachannel v0.24.1 checkout")

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

set(GLYPHRELAY_HAS_LINUX_CAPTURE 0)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLYPHRELAY_GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
  pkg_check_modules(GLYPHRELAY_PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  set(GLYPHRELAY_HAS_LINUX_CAPTURE 1)
endif()

add_library(glyphrelay_core
  src/app/doctor.cpp
  src/app/doctor_probes.cpp
  src/app/record_command.cpp
  src/app/share_command.cpp
  src/capture/capture.cpp
  src/core/benchmark_gate.cpp
  src/core/color_conversion.cpp
  src/core/m0_browser_source.cpp
  src/core/m0_protocol.cpp
  src/core/nv12_scaler.cpp
  src/core/quality_metrics.cpp
  src/core/sha256.cpp
  src/core/synthetic_source.cpp
  src/gpu/cuda_context.cpp
  src/gpu/gpu_contracts.cpp
  src/gpu/nvenc_benchmark.cpp
  src/gpu/nvenc_browser_fixture.cpp
  src/gpu/nvenc_probe.cpp
  src/media/annex_b.cpp
  src/media/h264_sps.cpp
  src/media/i420.cpp
  src/media/openh264_encoder.cpp
  src/media/recording_profile.cpp
  src/recording/recording_common.cpp
  src/transport/media_egress.cpp
  src/transport/encoded_fanout.cpp
  src/transport/rtp_transport.cpp
)
if(GLYPHRELAY_HAS_LINUX_CAPTURE)
  target_sources(glyphrelay_core PRIVATE
    src/app/record_command_linux.cpp
    src/capture/linux_capture_gdbus.cpp
    src/capture/linux_capture_pipewire.cpp
  )
else()
  target_sources(glyphrelay_core PRIVATE
    src/app/record_command_stub.cpp
    src/capture/linux_capture_stub.cpp)
endif()
if(NOT GLYPHRELAY_ENABLE_WEBRTC_CONTRACTS)
  target_sources(glyphrelay_core PRIVATE src/app/share_command_stub.cpp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(glyphrelay_core PRIVATE src/recording/recording_linux.cpp)
  set(GLYPHRELAY_HAS_DURABLE_RECORDING 1)
else()
  target_sources(glyphrelay_core PRIVATE src/recording/recording_stub.cpp)
  set(GLYPHRELAY_HAS_DURABLE_RECORDING 0)
endif()
target_include_directories(glyphrelay_core PUBLIC include)
target_compile_definitions(glyphrelay_core PUBLIC
  GLYPHRELAY_VERSION="${PROJECT_VERSION}"
  GLYPHRELAY_HAS_CUDA_COMPILER=${GLYPHRELAY_HAS_CUDA_COMPILER}
  GLYPHRELAY_HAS_CUDA_DRIVER=${GLYPHRELAY_HAS_CUDA_DRIVER}
  GLYPHRELAY_CUDA_COMPILER_VERSION="${CMAKE_CUDA_COMPILER_VERSION}"
  GLYPHRELAY_HAS_NVENC=${GLYPHRELAY_HAS_NVENC}
  GLYPHRELAY_HAS_OPENH264=${GLYPHRELAY_HAS_OPENH264}
  GLYPHRELAY_HAS_LINUX_CAPTURE=${GLYPHRELAY_HAS_LINUX_CAPTURE}
  GLYPHRELAY_HAS_DURABLE_RECORDING=${GLYPHRELAY_HAS_DURABLE_RECORDING}
  GLYPHRELAY_M0_PROTOCOL_SHA256="5443417595e3ccb88c89adc3a2d22842fde3206c736d3069042b62cd1c8ab708"
)
if(GLYPHRELAY_HAS_CUDA_DRIVER)
  target_link_libraries(glyphrelay_core PRIVATE CUDA::cuda_driver)
endif()
if(GLYPHRELAY_HAS_LINUX_CAPTURE)
  target_link_libraries(glyphrelay_core PRIVATE
    PkgConfig::GLYPHRELAY_GIO
    PkgConfig::GLYPHRELAY_PIPEWIRE)
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

if(GLYPHRELAY_ENABLE_WEBRTC_CONTRACTS)
  if(NOT EXISTS "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
      "GLYPHRELAY_ENABLE_WEBRTC_CONTRACTS requires the exact patched libdatachannel root")
  endif()
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_MEDIA OFF CACHE BOOL "" FORCE)
  set(NO_TESTS OFF CACHE BOOL "" FORCE)
  set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
  set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)
  set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
  set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
  set(USE_NICE OFF CACHE BOOL "" FORCE)
  set(USE_SYSTEM_JSON OFF CACHE BOOL "" FORCE)
  set(USE_SYSTEM_JUICE OFF CACHE BOOL "" FORCE)
  set(USE_SYSTEM_PLOG OFF CACHE BOOL "" FORCE)
  set(USE_SYSTEM_SRTP OFF CACHE BOOL "" FORCE)
  set(USE_SYSTEM_USRSCTP OFF CACHE BOOL "" FORCE)
  set(WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
  add_subdirectory("${GLYPHRELAY_LIBDATACHANNEL_ROOT}"
                   "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel-v0.24.1")
  if(NOT MSVC)
    # v0.24.1 uses std::bind paths that libstdc++ 13 marks deprecated.
    # Keep vendor warnings fatal except for that pinned upstream diagnostic.
    target_compile_options(datachannel PRIVATE -Wno-deprecated-declarations)
  endif()

  add_library(glyphrelay_libdatachannel_contract
    src/app/share_command_webrtc.cpp
    src/transport/control_protocol.cpp
    src/transport/owner_signaling.cpp
    src/transport/peer_sender.cpp
    vendor/libdatachannel-v0.24.1/glyphrelay_media_handlers.cpp)
  target_include_directories(glyphrelay_libdatachannel_contract PUBLIC
    vendor/libdatachannel-v0.24.1)
  target_include_directories(glyphrelay_libdatachannel_contract SYSTEM PRIVATE
    "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/include"
    "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/deps/json/single_include")
  target_link_libraries(glyphrelay_libdatachannel_contract PUBLIC glyphrelay_core datachannel)
  if(MSVC)
    target_compile_options(glyphrelay_libdatachannel_contract PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(glyphrelay_libdatachannel_contract PRIVATE
      -Wall -Wconversion -Werror -Wextra -Wpedantic -Wshadow)
  endif()

  add_executable(glyphrelay_m0_webrtc_sender tools/m0_webrtc_sender.cpp)
  target_include_directories(glyphrelay_m0_webrtc_sender SYSTEM PRIVATE
    "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/include")
  target_link_libraries(glyphrelay_m0_webrtc_sender PRIVATE
    glyphrelay_libdatachannel_contract)
  if(MSVC)
    target_compile_options(glyphrelay_m0_webrtc_sender PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(glyphrelay_m0_webrtc_sender PRIVATE
      -Wall -Wconversion -Werror -Wextra -Wpedantic -Wshadow)
  endif()

  add_executable(glyphrelay_m0_transport_fixture tools/m0_transport_fixture.cpp)
  target_include_directories(glyphrelay_m0_transport_fixture SYSTEM PRIVATE
    "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/include")
  target_link_libraries(glyphrelay_m0_transport_fixture PRIVATE
    glyphrelay_libdatachannel_contract)
  if(MSVC)
    target_compile_options(glyphrelay_m0_transport_fixture PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(glyphrelay_m0_transport_fixture PRIVATE
      -Wall -Wconversion -Werror -Wextra -Wpedantic -Wshadow)
  endif()

  add_executable(glyphrelay_owner_signaling_fixture
    tools/owner_signaling_fixture.cpp)
  target_include_directories(glyphrelay_owner_signaling_fixture SYSTEM PRIVATE
    "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/deps/json/single_include")
  target_link_libraries(glyphrelay_owner_signaling_fixture PRIVATE
    glyphrelay_libdatachannel_contract)
  if(MSVC)
    target_compile_options(glyphrelay_owner_signaling_fixture PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(glyphrelay_owner_signaling_fixture PRIVATE
      -Wall -Wconversion -Werror -Wextra -Wpedantic -Wshadow)
  endif()
endif()

add_executable(glyphrelay src/app/main.cpp)
if(GLYPHRELAY_ENABLE_WEBRTC_CONTRACTS)
  target_link_libraries(glyphrelay PRIVATE glyphrelay_libdatachannel_contract)
else()
  target_link_libraries(glyphrelay PRIVATE glyphrelay_core)
endif()

add_executable(glyphrelay_hash_m0_browser_source tools/hash_m0_browser_source.cpp)
target_link_libraries(glyphrelay_hash_m0_browser_source PRIVATE glyphrelay_core)

include(CTest)
if(BUILD_TESTING)
  add_executable(glyphrelay_native_tests tests/native/test_doctor.cpp)
  target_link_libraries(glyphrelay_native_tests PRIVATE glyphrelay_core)
  add_test(NAME native.doctor COMMAND glyphrelay_native_tests)
  add_test(NAME cli.help COMMAND glyphrelay --help)
  add_test(NAME cli.doctor_json COMMAND glyphrelay doctor --json)
  add_test(NAME cli.record_rejects_source_override
    COMMAND "${CMAKE_COMMAND}"
      -DGLYPHRELAY_PROGRAM=$<TARGET_FILE:glyphrelay>
      -DGLYPHRELAY_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/forbidden-record.h264
      -P "${CMAKE_SOURCE_DIR}/tests/cmake/assert_record_source_rejected.cmake")
  add_test(NAME cli.share_unconfigured_and_rejects_origin_override
    COMMAND "${CMAKE_COMMAND}"
      -DGLYPHRELAY_PROGRAM=$<TARGET_FILE:glyphrelay>
      -P "${CMAKE_SOURCE_DIR}/tests/cmake/assert_share_configuration.cmake")

  add_executable(glyphrelay_m0_protocol_tests tests/native/test_m0_protocol.cpp)
  target_link_libraries(glyphrelay_m0_protocol_tests PRIVATE glyphrelay_core)
  target_compile_definitions(glyphrelay_m0_protocol_tests PRIVATE
    GLYPHRELAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  add_test(NAME native.m0_protocol COMMAND glyphrelay_m0_protocol_tests)

  add_executable(glyphrelay_media_contract_tests tests/native/test_media_contracts.cpp)
  target_link_libraries(glyphrelay_media_contract_tests PRIVATE glyphrelay_core)
  add_test(NAME native.media_contracts COMMAND glyphrelay_media_contract_tests)

  add_executable(glyphrelay_gpu_contract_tests tests/native/test_gpu_contracts.cpp)
  target_link_libraries(glyphrelay_gpu_contract_tests PRIVATE glyphrelay_core)
  add_test(NAME native.gpu_contracts COMMAND glyphrelay_gpu_contract_tests)

  add_executable(glyphrelay_capture_contract_tests tests/native/test_capture_contracts.cpp)
  target_link_libraries(glyphrelay_capture_contract_tests PRIVATE glyphrelay_core)
  add_test(NAME native.capture_contracts COMMAND glyphrelay_capture_contract_tests)

  if(GLYPHRELAY_HAS_LINUX_CAPTURE)
    add_executable(glyphrelay_linux_capture_contract_tests
      tests/native/test_linux_capture_contracts.cpp)
    target_include_directories(glyphrelay_linux_capture_contract_tests PRIVATE
      src/capture)
    target_link_libraries(glyphrelay_linux_capture_contract_tests PRIVATE
      glyphrelay_core
      PkgConfig::GLYPHRELAY_PIPEWIRE)
    add_test(NAME integration.linux_capture_contracts
      COMMAND glyphrelay_linux_capture_contract_tests)
  endif()

  if(GLYPHRELAY_HAS_DURABLE_RECORDING AND GLYPHRELAY_HAS_OPENH264)
    add_executable(glyphrelay_recording_contract_tests
      tests/native/test_recording_contracts.cpp)
    target_link_libraries(glyphrelay_recording_contract_tests PRIVATE glyphrelay_core)
    set(GLYPHRELAY_DURABLE_RECORDING_TEST_STREAM
        "${CMAKE_CURRENT_BINARY_DIR}/durable-recording.h264")
    add_test(NAME integration.durable_recording
      COMMAND glyphrelay_recording_contract_tests
              "${GLYPHRELAY_DURABLE_RECORDING_TEST_STREAM}")
    find_program(GLYPHRELAY_RECORDING_FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)
    add_test(NAME integration.ffmpeg_decodes_durable_recording
      COMMAND "${GLYPHRELAY_RECORDING_FFMPEG_EXECUTABLE}" -v error -f h264
              -i "${GLYPHRELAY_DURABLE_RECORDING_TEST_STREAM}" -f null -)
    set_tests_properties(integration.durable_recording PROPERTIES
      FIXTURES_SETUP durable_recording_stream)
    set_tests_properties(integration.ffmpeg_decodes_durable_recording PROPERTIES
      DEPENDS integration.durable_recording
      FIXTURES_REQUIRED durable_recording_stream)

    add_executable(glyphrelay_record_command_tests tests/native/test_record_command.cpp)
    target_link_libraries(glyphrelay_record_command_tests PRIVATE glyphrelay_core)
    set(GLYPHRELAY_RECORD_COMMAND_TEST_STREAM
        "${CMAKE_CURRENT_BINARY_DIR}/record-command.h264")
    add_test(NAME integration.record_command
      COMMAND glyphrelay_record_command_tests "${GLYPHRELAY_RECORD_COMMAND_TEST_STREAM}")
    add_test(NAME cli.inspect_recording_json
      COMMAND glyphrelay inspect --recording "${GLYPHRELAY_RECORD_COMMAND_TEST_STREAM}" --json)
    add_test(NAME integration.ffmpeg_decodes_record_command
      COMMAND "${GLYPHRELAY_RECORDING_FFMPEG_EXECUTABLE}" -v error -f h264
              -i "${GLYPHRELAY_RECORD_COMMAND_TEST_STREAM}" -f null -)
    set_tests_properties(integration.record_command PROPERTIES
      FIXTURES_SETUP record_command_stream)
    set_tests_properties(cli.inspect_recording_json
      integration.ffmpeg_decodes_record_command PROPERTIES
      DEPENDS integration.record_command
      FIXTURES_REQUIRED record_command_stream)
  endif()

  add_executable(glyphrelay_egress_contract_tests tests/native/test_media_egress.cpp)
  target_link_libraries(glyphrelay_egress_contract_tests PRIVATE glyphrelay_core)
  add_test(NAME native.media_egress COMMAND glyphrelay_egress_contract_tests)

  add_executable(glyphrelay_rtp_transport_tests tests/native/test_rtp_transport.cpp)
  target_link_libraries(glyphrelay_rtp_transport_tests PRIVATE glyphrelay_core)
  add_test(NAME native.rtp_transport COMMAND glyphrelay_rtp_transport_tests)

  add_executable(glyphrelay_encoded_fanout_tests tests/native/test_encoded_fanout.cpp)
  target_link_libraries(glyphrelay_encoded_fanout_tests PRIVATE glyphrelay_core)
  add_test(NAME native.encoded_fanout COMMAND glyphrelay_encoded_fanout_tests)

  add_executable(glyphrelay_share_command_tests tests/native/test_share_command.cpp)
  target_link_libraries(glyphrelay_share_command_tests PRIVATE glyphrelay_core)
  if(GLYPHRELAY_HAS_DURABLE_RECORDING AND GLYPHRELAY_HAS_OPENH264)
    set(GLYPHRELAY_SHARE_COMMAND_TEST_STREAM
        "${CMAKE_CURRENT_BINARY_DIR}/share-command.h264")
    add_test(NAME integration.share_command
      COMMAND glyphrelay_share_command_tests "${GLYPHRELAY_SHARE_COMMAND_TEST_STREAM}")
    add_test(NAME integration.ffmpeg_decodes_share_command
      COMMAND "${GLYPHRELAY_RECORDING_FFMPEG_EXECUTABLE}" -v error -f h264
              -i "${GLYPHRELAY_SHARE_COMMAND_TEST_STREAM}" -f null -)
    set_tests_properties(integration.share_command PROPERTIES
      FIXTURES_SETUP share_command_stream)
    set_tests_properties(integration.ffmpeg_decodes_share_command PROPERTIES
      DEPENDS integration.share_command
      FIXTURES_REQUIRED share_command_stream)
  else()
    add_test(NAME native.share_command COMMAND glyphrelay_share_command_tests)
  endif()

  if(GLYPHRELAY_ENABLE_WEBRTC_CONTRACTS)
    add_executable(glyphrelay_libdatachannel_contract_tests
      tests/native/test_libdatachannel_contract.cpp)
    target_link_libraries(glyphrelay_libdatachannel_contract_tests PRIVATE
      glyphrelay_libdatachannel_contract)
    target_include_directories(glyphrelay_libdatachannel_contract_tests SYSTEM PRIVATE
      "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/include/rtc"
      "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/src")
    add_test(NAME integration.libdatachannel_contract
      COMMAND glyphrelay_libdatachannel_contract_tests)
    add_executable(glyphrelay_owner_signaling_tests
      tests/native/test_owner_signaling.cpp)
    target_include_directories(glyphrelay_owner_signaling_tests SYSTEM PRIVATE
      "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/deps/json/single_include")
    target_link_libraries(glyphrelay_owner_signaling_tests PRIVATE
      glyphrelay_libdatachannel_contract)
    add_test(NAME integration.owner_signaling_protocol
      COMMAND glyphrelay_owner_signaling_tests)
    add_executable(glyphrelay_control_protocol_tests
      tests/native/test_control_protocol.cpp)
    target_include_directories(glyphrelay_control_protocol_tests SYSTEM PRIVATE
      "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/deps/json/single_include")
    target_link_libraries(glyphrelay_control_protocol_tests PRIVATE
      glyphrelay_libdatachannel_contract)
    add_test(NAME integration.control_protocol
      COMMAND glyphrelay_control_protocol_tests)
    add_executable(glyphrelay_peer_sender_tests
      tests/native/test_peer_sender.cpp)
    target_include_directories(glyphrelay_peer_sender_tests SYSTEM PRIVATE
      "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/include"
      "${GLYPHRELAY_LIBDATACHANNEL_ROOT}/deps/json/single_include")
    target_link_libraries(glyphrelay_peer_sender_tests PRIVATE
      glyphrelay_libdatachannel_contract)
    add_test(NAME integration.peer_sender
      COMMAND glyphrelay_peer_sender_tests)
    add_test(NAME cli.m0_webrtc_sender_help COMMAND glyphrelay_m0_webrtc_sender --help)
    add_test(NAME cli.m0_transport_fixture_help COMMAND glyphrelay_m0_transport_fixture --help)
    add_test(NAME cli.owner_signaling_fixture_help
      COMMAND glyphrelay_owner_signaling_fixture --help)
  endif()

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
