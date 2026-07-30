# Turning a title into pixels.
#
# Its own library because both halves of the application need it and neither
# should own it: the engine composites a title into a frame, and the interface
# will want to draw the same text while it is being typed. Skia is private, so
# nothing above here inherits it.
add_library(cutline_text src/raster.cpp)
add_library(cutline::text ALIAS cutline_text)

find_package(unofficial-skia CONFIG REQUIRED)

target_include_directories(cutline_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cutline_text
  PUBLIC cutline::core cutline::options
  PRIVATE unofficial::skia::skia)
