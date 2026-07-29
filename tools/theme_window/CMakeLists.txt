add_executable(theme_window main.cpp)

target_link_libraries(theme_window PRIVATE
  cutline::core
  cutline::editor
  cutline::options
  cutline::ui
  cutline::ui_skia
  # The system caption is turned off and drawn by the interface instead. This
  # is only for handing one pixel of frame back, so the window keeps the drop
  # shadow and rounded corners the compositor draws around it.
  dwmapi
  # The open and save dialogs.
  comdlg32)

# Skia is private to cutline::ui_skia, so it does not leak into the widget
# layer. This tool needs it directly for one thing only: making the raster
# surface the interface is drawn into.
find_package(unofficial-skia CONFIG REQUIRED)
target_link_libraries(theme_window PRIVATE unofficial::skia::skia)

# Decoding and previewing when the media layer is present, and a chrome harness
# when it is not. The window is the same either way; what it can do is not.
if(TARGET cutline::app)
  target_link_libraries(theme_window PRIVATE cutline::app)
  target_compile_definitions(theme_window PRIVATE CUTLINE_HAVE_PREVIEW=1)
endif()
