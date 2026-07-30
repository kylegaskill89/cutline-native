add_library(cutline_engine
  src/audio_mixer.cpp
  src/exporter.cpp
  src/frame_renderer.cpp
  src/player.cpp)
add_library(cutline::engine ALIAS cutline_engine)

target_include_directories(cutline_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The only layer that depends on everything, which is the point of it: it is
# where the model, the plan, the decoder, and the compositor meet.
# cutline::media is public because the exporter's settings name its encoder
# types; everything else about it stays an implementation detail.
# ole32 and propsys are for WASAPI: the device enumerator is a COM object, and
# the device's friendly name comes from a property store.
target_link_libraries(cutline_engine
  PUBLIC cutline::core cutline::gpu cutline::media cutline::options
  PRIVATE cutline::audio cutline::render ole32 propsys)

# Titles, when there is something to draw glyphs with. Without it the renderer
# skips text layers, exactly as it did before there was a rasteriser at all: the
# media preset carries no Skia and still exports everything else.
if(TARGET cutline_text)
  target_link_libraries(cutline_engine PRIVATE cutline::text)
  target_compile_definitions(cutline_engine PRIVATE CUTLINE_HAVE_TEXT=1)
endif()

# Decoded frames are described for the compositor here, so this needs the libav
# headers even though nothing above it does.
find_package(FFMPEG REQUIRED)
target_include_directories(cutline_engine SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
