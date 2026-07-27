add_executable(preview_window main.cpp)

target_link_libraries(preview_window PRIVATE
  cutline::gpu
  cutline::media
  cutline::options)

# The tool converts AVFrames into the renderer's frame view, so it needs the
# libav headers even though the GPU layer never sees them.
find_package(FFMPEG REQUIRED)
target_include_directories(preview_window SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})

include(CompileShaders)
cutline_compile_shader(preview_window "${CUTLINE_SHADER_SOURCE}" VSMain vs_6_0 present_vs.cso)
cutline_compile_shader(preview_window "${CUTLINE_SHADER_SOURCE}" PSVideo ps_6_0 present_video_ps.cso)
cutline_compile_shader(preview_window "${CUTLINE_SHADER_SOURCE}" PSPresent ps_6_0 present_ps.cso)
