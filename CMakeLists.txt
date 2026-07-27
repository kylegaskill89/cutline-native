cmake_minimum_required(VERSION 3.28)

project(cutline-native
  VERSION 0.0.1
  DESCRIPTION "Native C++ non-linear video editor"
  LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "cutline-native targets Windows only (Direct3D 12 compositor).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CUTLINE_BUILD_TESTS "Build unit tests" ON)
option(CUTLINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# The media layer pulls in FFmpeg, which is a long build from source. It stays
# opt-in so the core -- which needs none of it -- keeps configuring in seconds.
# Enable with the `media` preset, which also selects the vcpkg feature.
option(CUTLINE_WITH_MEDIA "Build the media layer (requires FFmpeg)" OFF)

add_library(cutline_options INTERFACE)
add_library(cutline::options ALIAS cutline_options)

if(MSVC)
  target_compile_options(cutline_options INTERFACE
    /W4                 # high warning level
    /permissive-        # strict standard conformance
    /utf-8              # source and execution charset
    /Zc:preprocessor    # conforming preprocessor
    /Zc:__cplusplus     # report the real __cplusplus value
    /external:W0        # third-party headers are not ours to keep warning-free
    /EHsc)
  target_compile_definitions(cutline_options INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    # MSVC deprecates standard C++ functions such as std::getenv in favour of
    # its own alternatives. Silencing that is not the same as silencing a real
    # warning, and keeping the standard spelling keeps the code portable.
    _CRT_SECURE_NO_WARNINGS)
  if(CUTLINE_WARNINGS_AS_ERRORS)
    target_compile_options(cutline_options INTERFACE /WX)
  endif()
endif()

add_subdirectory(src/core)

if(CUTLINE_WITH_MEDIA)
  add_subdirectory(src/media)
  add_subdirectory(tools/decode_bench)
endif()

if(CUTLINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
