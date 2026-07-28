add_executable(theme_window main.cpp)

target_link_libraries(theme_window PRIVATE
  cutline::options
  cutline::ui
  cutline::ui_skia)

# Skia is private to cutline::ui_skia, so it does not leak into the widget
# layer. This tool needs it directly for one thing only: making the raster
# surface the interface is drawn into.
find_package(unofficial-skia CONFIG REQUIRED)
target_link_libraries(theme_window PRIVATE unofficial::skia::skia)
