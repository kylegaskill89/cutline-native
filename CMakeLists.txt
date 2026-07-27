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

add_library(cutline_options INTERFACE)
add_library(cutline::options ALIAS cutline_options)

if(MSVC)
  target_compile_options(cutline_options INTERFACE
    /W4                 # high warning level
    /permissive-        # strict standard conformance
    /utf-8              # source and execution charset
    /Zc:preprocessor    # conforming preprocessor
    /Zc:__cplusplus     # report the real __cplusplus value
    /EHsc)
  target_compile_definitions(cutline_options INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE)
  if(CUTLINE_WARNINGS_AS_ERRORS)
    target_compile_options(cutline_options INTERFACE /WX)
  endif()
endif()

add_subdirectory(src/core)

if(CUTLINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
