add_library(cutline_track
  src/tracker.cpp)
add_library(cutline::track ALIAS cutline_track)

target_include_directories(cutline_track PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Pure arithmetic over grey pictures, with nothing to decode or draw: what
# lets it be tested on pictures made in the test.
target_link_libraries(cutline_track PUBLIC cutline::options)
