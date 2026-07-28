add_library(cutline_engine
  src/audio_mixer.cpp
  src/exporter.cpp
  src/frame_renderer.cpp)
add_library(cutline::engine ALIAS cutline_engine)

target_include_directories(cutline_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The only layer that depends on everything, which is the point of it: it is
# where the model, the plan, the decoder, and the compositor meet.
# cutline::media is public because the exporter's settings name its encoder
# types; everything else about it stays an implementation detail.
target_link_libraries(cutline_engine
  PUBLIC cutline::core cutline::gpu cutline::media cutline::options
  PRIVATE cutline::audio cutline::render)

# Decoded frames are described for the compositor here, so this needs the libav
# headers even though nothing above it does.
find_package(FFMPEG REQUIRED)
target_include_directories(cutline_engine SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
