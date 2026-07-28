add_executable(play_project main.cpp)

target_link_libraries(play_project PRIVATE
  cutline::core
  cutline::engine
  cutline::gpu
  cutline::options)

cutline_gpu_shaders(play_project)
