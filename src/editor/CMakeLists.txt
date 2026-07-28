# The seam between the model and the interface.
#
# It is a library of its own rather than part of either, because it is the only
# thing that should know both: the core has no idea what is selected, and the UI
# has no idea what a project is. Keeping the join here is what stops a clip id
# leaking into a widget or a rectangle leaking into an edit operation.
#
# Pure, and needs no Skia, no GPU and no FFmpeg.
add_library(cutline_editor
  src/inspector.cpp
  src/session.cpp
  src/timeline_binding.cpp)
add_library(cutline::editor ALIAS cutline_editor)

target_include_directories(cutline_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cutline_editor PUBLIC cutline::core cutline::ui cutline::options)
