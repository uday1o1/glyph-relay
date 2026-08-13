cmake_minimum_required(VERSION 3.28)

project(GlyphRelay VERSION 0.1.0 LANGUAGES CXX)

option(GLYPHRELAY_ENABLE_CUDA "Enable CUDA when a CUDA compiler is available" ON)
option(GLYPHRELAY_ENABLE_SANITIZERS "Enable supported sanitizers" OFF)

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

add_library(glyphrelay_core src/app/doctor.cpp src/app/doctor_probes.cpp)
target_include_directories(glyphrelay_core PUBLIC include)
target_compile_definitions(glyphrelay_core PUBLIC
  GLYPHRELAY_VERSION="${PROJECT_VERSION}"
  GLYPHRELAY_HAS_CUDA_COMPILER=${GLYPHRELAY_HAS_CUDA_COMPILER}
  GLYPHRELAY_CUDA_COMPILER_VERSION="${CMAKE_CUDA_COMPILER_VERSION}"
)

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
endif()

install(TARGETS glyphrelay RUNTIME DESTINATION bin)
