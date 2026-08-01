# The seam between the model and the interface.
#
# It is a library of its own rather than part of either, because it is the only
# thing that should know both: the core has no idea what is selected, and the UI
# has no idea what a project is. Keeping the join here is what stops a clip id
# leaking into a widget or a rectangle leaking into an edit operation.
#
# Pure, and needs no Skia, no GPU and no FFmpeg.
add_library(cutline_editor
  src/autosave.cpp
  src/browser_binding.cpp
  src/commands.cpp
  src/document.cpp
  src/effects_binding.cpp
  src/generators.cpp
  src/import.cpp
  src/inspector.cpp
  src/keyframes.cpp
  src/monitor_binding.cpp
  src/session.cpp
  src/timeline_binding.cpp
  src/titles.cpp
  src/transitions.cpp
  src/updates.cpp
  src/workspace.cpp)
add_library(cutline::editor ALIAS cutline_editor)

target_include_directories(cutline_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(nlohmann_json CONFIG REQUIRED)

# `render` for the visual effect catalogue and `audio` for the audio registry:
# what effects exist and what they take is part of the effects themselves, and
# both are as pure as the rest of this — no GPU, no FFmpeg.
target_link_libraries(cutline_editor
  PUBLIC cutline::audio cutline::core cutline::render cutline::ui cutline::options
  # bcrypt for SHA-256: the update check decides whether to run a downloaded
  # executable, and a hash function is exactly the kind of thing that is subtly
  # wrong in a way no ordinary test finds. The system ships a correct one.
  PRIVATE nlohmann_json::nlohmann_json bcrypt)
