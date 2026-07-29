# The seam between the model and the interface.
#
# It is a library of its own rather than part of either, because it is the only
# thing that should know both: the core has no idea what is selected, and the UI
# has no idea what a project is. Keeping the join here is what stops a clip id
# leaking into a widget or a rectangle leaking into an edit operation.
#
# Pure, and needs no Skia, no GPU and no FFmpeg.
add_library(cutline_editor
  src/browser_binding.cpp
  src/commands.cpp
  src/document.cpp
  src/effects_binding.cpp
  src/import.cpp
  src/inspector.cpp
  src/session.cpp
  src/timeline_binding.cpp
  src/workspace.cpp)
add_library(cutline::editor ALIAS cutline_editor)

target_include_directories(cutline_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(nlohmann_json CONFIG REQUIRED)

# `render` for the effect catalogue: what effects exist and what they take is
# part of the effects themselves, and it is as pure as the rest of this — no
# GPU, no FFmpeg.
target_link_libraries(cutline_editor
  PUBLIC cutline::core cutline::render cutline::ui cutline::options
  PRIVATE nlohmann_json::nlohmann_json)
