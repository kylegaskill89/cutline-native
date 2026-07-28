add_executable(preview_window main.cpp)

target_link_libraries(preview_window PRIVATE
  cutline::core
  cutline::gpu
  cutline::media
  cutline::options)

# The tool converts AVFrames into the compositor's frame view, so it needs the
# libav headers even though the GPU layer never sees them.
find_package(FFMPEG REQUIRED)
target_include_directories(preview_window SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})

cutline_gpu_shaders(preview_window)
