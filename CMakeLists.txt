cmake_minimum_required(VERSION 3.28)

project(cutline-native
  VERSION 0.2.0
  DESCRIPTION "Native C++ non-linear video editor"
  LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "cutline-native targets Windows only (Direct3D 12 compositor).")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CUTLINE_BUILD_TESTS "Build unit tests" ON)
option(CUTLINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# The media layer pulls in FFmpeg, which is a long build from source. It stays
# opt-in so the core -- which needs none of it -- keeps configuring in seconds.
# Enable with the `media` preset, which also selects the vcpkg feature.
option(CUTLINE_WITH_MEDIA "Build the media layer (requires FFmpeg)" OFF)

# The GPU layer needs only the Windows SDK, so it costs nothing to build. It is
# still opt-in so a machine without a D3D12 adapter can work on the core.
option(CUTLINE_WITH_GPU "Build the Direct3D 12 layer" OFF)

# Skia is a long build, like FFmpeg. The UI's theme, metrics and layout are pure
# and need none of it -- only the painting does -- so the two are separable and
# most UI work does not pay this cost.
option(CUTLINE_WITH_SKIA "Build the Skia painting backend" OFF)

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
# Pure like the core, and built alongside it: a frame plan needs no GPU.
add_subdirectory(src/render)
# Likewise the audio DSP -- arithmetic over sample buffers, no FFmpeg needed.
add_subdirectory(src/audio)
# And the theme model: data and lookup, with Skia needed only to paint it.
add_subdirectory(src/ui)
# The join between the two: the only thing that knows both a project and a
# widget. Pure like they are.
add_subdirectory(src/editor)

# Titles. Skia draws the glyphs and nothing else is needed, so this follows the
# Skia option rather than the GPU one — and the engine composites titles only
# when it is here, which keeps the media preset building without Skia.
if(CUTLINE_WITH_SKIA)
  add_subdirectory(src/text)
endif()

if(CUTLINE_WITH_MEDIA)
  add_subdirectory(src/media)
  add_subdirectory(tools/decode_bench)
endif()

if(CUTLINE_WITH_GPU)
  add_subdirectory(src/gpu)
  # The engine joins decoding to compositing, so it needs both halves present.
  if(CUTLINE_WITH_MEDIA)
    add_subdirectory(src/engine)
    # The application layer needs decoding, compositing and the interface all
    # at once, so it only exists when the media half is present.
    add_subdirectory(src/app)
    add_subdirectory(tools/play_project)
    add_subdirectory(tools/preview_window)
    add_subdirectory(tools/render_frame)
    add_subdirectory(tools/export_project)
  endif()
endif()

# Last, because it links the application layer when that exists and does
# without it when it does not: under the skia preset it is a chrome harness,
# and under the ui preset it decodes and previews. One window that gains
# capability with the preset beats two windows that drift apart.
if(CUTLINE_WITH_SKIA)
  add_subdirectory(tools/cutline)
endif()

if(CUTLINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Last of all, because it packages whatever the rest of this decided to build.
include(Packaging)
