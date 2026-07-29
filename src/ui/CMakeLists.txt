add_library(cutline_ui
  src/browser.cpp
  src/controls.cpp
  src/dock.cpp
  src/dock_view.cpp
  src/layout.cpp
  src/monitor.cpp
  src/painter.cpp
  src/recording_painter.cpp
  src/theme.cpp
  src/themes.cpp
  src/timeline.cpp
  src/timescale.cpp
  src/widget.cpp
  src/widgets.cpp)
add_library(cutline::ui ALIAS cutline_ui)

target_include_directories(cutline_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# No Skia, no GPU, no FFmpeg. A theme is data and a lookup, and keeping it that
# way is what lets the whole look of the application be tested without a window
# -- and what keeps most UI work off the long Skia build.
#
# The one exception is the core, for time: frame durations and timecode. Those
# are already written and tested there, and reimplementing them here to keep a
# layering rule that costs nothing to break would be silly. The core is pure and
# brings no dependencies of its own.
target_link_libraries(cutline_ui PUBLIC cutline::core cutline::options)

# The Skia backend is a separate target so the pure half above stays buildable
# without it. Nothing in the widget layer will link this directly: it is chosen
# once, where the window is created.
if(CUTLINE_WITH_SKIA)
  find_package(unofficial-skia CONFIG REQUIRED)

  add_library(cutline_ui_skia src/skia_painter.cpp)
  add_library(cutline::ui_skia ALIAS cutline_ui_skia)

  target_include_directories(cutline_ui_skia PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(cutline_ui_skia
    PUBLIC cutline::ui cutline::options
    PRIVATE unofficial::skia::skia)
endif()
