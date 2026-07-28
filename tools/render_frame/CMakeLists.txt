add_executable(render_frame main.cpp)

target_link_libraries(render_frame PRIVATE
  cutline::core
  cutline::engine
  cutline::gpu
  cutline::options)

cutline_gpu_shaders(render_frame)
