add_library(cutline_audio
  src/biquad.cpp
  src/chain.cpp
  src/compressor.cpp
  src/delay.cpp
  src/limiter.cpp
  src/loudness.cpp
  src/reverb.cpp
  src/meter.cpp
  src/stretch.cpp)
add_library(cutline::audio ALIAS cutline_audio)

target_include_directories(cutline_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# No FFmpeg and no GPU: the DSP is arithmetic over sample buffers, and keeping
# it that way is what lets a filter's response be asserted without a media file.
target_link_libraries(cutline_audio PUBLIC cutline::core cutline::options)
