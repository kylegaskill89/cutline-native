add_library(cutline_ui
  src/theme.cpp
  src/themes.cpp)
add_library(cutline::ui ALIAS cutline_ui)

target_include_directories(cutline_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# No Skia, no GPU, no FFmpeg. A theme is data and a lookup, and keeping it that
# way is what lets the whole look of the application be tested without a window
# -- and what keeps most UI work off the long Skia build.
target_link_libraries(cutline_ui PUBLIC cutline::options)
