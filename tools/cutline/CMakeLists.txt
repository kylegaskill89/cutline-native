# The icon and the version block. Windows takes an executable's identity from
# its resources, so without this the application is a blank sheet of paper in
# Explorer, in the taskbar and in Alt+Tab — and the release page shows the same
# blank sheet beside the download.
#
# Compiled only under MSVC-like toolchains; nothing else here reads an .rc, and
# the window it decorates is a Win32 window anyway.
set(CUTLINE_RESOURCES)
if(WIN32)
  list(APPEND CUTLINE_RESOURCES cutline.rc)
endif()

add_executable(cutline main.cpp ${CUTLINE_RESOURCES})

if(WIN32)
  # The version block wants the number twice in two spellings, and getting them
  # from the project rather than typing them into the .rc is what stops the
  # executable claiming a version the build does not have.
  string(REPLACE "." "," CUTLINE_VERSION_COMMAS "${PROJECT_VERSION}")
  set_source_files_properties(cutline.rc PROPERTIES COMPILE_DEFINITIONS
    "CUTLINE_VERSION_COMMAS=${CUTLINE_VERSION_COMMAS},0;CUTLINE_VERSION_STRING=\"${PROJECT_VERSION}\"")
endif()

target_link_libraries(cutline PRIVATE
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
  comdlg32
  # timeBeginPeriod, so the playback loop can pace itself to a frame rather
  # than to Windows' default 15.6 ms scheduler tick.
  winmm)

# Skia is private to cutline::ui_skia, so it does not leak into the widget
# layer. This tool needs it directly for one thing only: making the raster
# surface the interface is drawn into.
find_package(unofficial-skia CONFIG REQUIRED)
target_link_libraries(cutline PRIVATE unofficial::skia::skia)

# Decoding and previewing when the media layer is present, and a chrome harness
# when it is not. The window is the same either way; what it can do is not.
if(TARGET cutline::app)
  target_link_libraries(cutline PRIVATE cutline::app)
  target_compile_definitions(cutline PRIVATE CUTLINE_HAVE_PREVIEW=1)

  # The compositor loads its shaders from beside the executable at runtime, so
  # they have to be built next to this one as well.
  #
  # Every other consumer already did this; the window did not, and so the
  # preview in the actual application has never once rendered a frame — it
  # failed at "cannot open composite_layer_vs.cso" and showed the colour bars
  # it starts with, which look exactly like a preview waiting for something to
  # decode. Nothing caught it because no test runs the window, and the tools
  # that do render are the ones that had the shaders.
  cutline_gpu_shaders(cutline)
endif()
