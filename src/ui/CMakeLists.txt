add_library(cutline_ui
  src/browser.cpp
  src/color_picker.cpp
  src/controls.cpp
  src/dock.cpp
  src/dock_view.cpp
  src/keyframe_view.cpp
  src/layout.cpp
  src/meter_view.cpp
  src/monitor.cpp
  src/painter.cpp
  src/recording_painter.cpp
  src/scopes_view.cpp
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
# Three exceptions, all pure and none bringing a dependency of its own.
#
# The core, for time: frame durations and timecode are already written and
# tested there, and reimplementing them here to keep a layering rule that costs
# nothing to break would be silly.
#
# And the render layer, for what a scope measures. `ScopesView` draws a
# histogram; it does not count one. Keeping the counting in `render` is what
# lets the arithmetic be tested against a frame built in three lines with no
# widget, and what guarantees a scope can never affect an export — the only
# thing that flows between them is a set of tallies.
#
# And the audio layer, for the same reason and in the same shape: `MeterView`
# draws a level, and `audio::Meter` decides what a level is. The ballistics are
# where the judgement lives and they are asserted against a buffer of samples,
# with no widget anywhere near them.
target_link_libraries(cutline_ui
  PUBLIC cutline::core cutline::render cutline::audio cutline::options)

# The Skia backend is a separate target so the pure half above stays buildable
# without it. Nothing in the widget layer will link this directly: it is chosen
# once, where the window is created.
if(CUTLINE_WITH_SKIA)
  find_package(unofficial-skia CONFIG REQUIRED)

  add_library(cutline_ui_skia
    src/skia_painter.cpp
    src/skia_texture.cpp
    src/skia_window.cpp)
  add_library(cutline::ui_skia ALIAS cutline_ui_skia)

  target_include_directories(cutline_ui_skia PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(cutline_ui_skia
    PUBLIC cutline::ui cutline::options
    PRIVATE unofficial::skia::skia d3d12 dxgi)
endif()
