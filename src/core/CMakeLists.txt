add_library(cutline_core
  src/animate.cpp
  src/attributes.cpp
  src/edit.cpp
  src/effects.cpp
  src/history.cpp
  src/id.cpp
  src/interpret.cpp
  src/keyframe.cpp
  src/layout.cpp
  src/mask_path.cpp
  src/nesting.cpp
  src/sequences.cpp
  src/pool.cpp
  src/properties.cpp
  src/query.cpp
  src/ripple.cpp
  src/roles.cpp
  src/routing.cpp
  src/segments.cpp
  src/serialize.cpp
  src/time.cpp)
add_library(cutline::core ALIAS cutline_core)

# The version, written down once and generated from the project's own. A number
# kept in two places disagrees with itself the first time somebody bumps only
# one of them -- and this one is compared against what a release manifest says.
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/cutline/core/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/cutline/core/version.hpp
  @ONLY)

target_include_directories(cutline_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(cutline_core PUBLIC cutline::options PRIVATE nlohmann_json::nlohmann_json)
