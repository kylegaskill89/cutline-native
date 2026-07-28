add_library(cutline_core
  src/animate.cpp
  src/edit.cpp
  src/effects.cpp
  src/history.cpp
  src/id.cpp
  src/keyframe.cpp
  src/layout.cpp
  src/properties.cpp
  src/query.cpp
  src/segments.cpp
  src/serialize.cpp
  src/time.cpp)
add_library(cutline::core ALIAS cutline_core)

target_include_directories(cutline_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(cutline_core PUBLIC cutline::options PRIVATE nlohmann_json::nlohmann_json)
