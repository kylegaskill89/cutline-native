find_package(FFMPEG REQUIRED)

add_library(cutline_media
  src/audio.cpp
  src/decoder.cpp
  src/probe.cpp
  src/thumbnail.cpp)
add_library(cutline::media ALIAS cutline_media)

# FFmpeg's headers do not compile warning-free at /W4, and they are not ours to
# fix, so they come in as system headers.
target_include_directories(cutline_media
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})

target_link_libraries(cutline_media
  PUBLIC cutline::options
  PRIVATE ${FFMPEG_LIBRARIES})

target_link_directories(cutline_media PRIVATE ${FFMPEG_LIBRARY_DIRS})
