# Where the halves meet the media layer.
#
# Everything below this is deliberately partial: the UI draws without knowing
# what a project is, the editor edits without knowing what a decoder is, and
# both build and test with neither FFmpeg nor a GPU present. This is the only
# target that needs all of it at once, which is exactly why it is small and
# separate — pulling the media layer into either half would cost them their
# testability for the sake of two functions.
add_library(cutline_app
  src/preview.cpp
  src/proxies.cpp
  src/thumbnails.cpp
  src/updater.cpp
  src/media_cache.cpp
  src/waveforms.cpp)
add_library(cutline::app ALIAS cutline_app)

target_include_directories(cutline_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cutline_app PUBLIC
  cutline::core
  cutline::editor
  cutline::engine
  cutline::gpu
  cutline::media
  cutline::options
  cutline::ui)

# WinHTTP for the update check. Part of the Windows SDK, so it costs nothing to
# link and brings no dependency -- which is the point: an editor that pulled in
# a whole HTTP library to fetch one small JSON file would be paying a great deal
# for a button nobody presses often.
target_link_libraries(cutline_app PRIVATE winhttp)
