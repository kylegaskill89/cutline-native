add_library(cutline_core
  src/edit.cpp
  src/id.cpp
  src/keyframe.cpp
  src/query.cpp
  src/segments.cpp
  src/time.cpp)
add_library(cutline::core ALIAS cutline_core)

target_include_directories(cutline_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cutline_core PUBLIC cutline::options)
