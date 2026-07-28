add_executable(export_project main.cpp)

target_link_libraries(export_project PRIVATE
  cutline::core
  cutline::engine
  cutline::gpu
  cutline::options)

cutline_gpu_shaders(export_project)
