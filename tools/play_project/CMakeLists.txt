add_executable(play_project main.cpp)

# winmm is for timeBeginPeriod: the default 15.6 ms scheduler tick is most of a
# frame at 60 Hz, and the playback loop cannot pace itself without a finer one.
target_link_libraries(play_project PRIVATE
  cutline::core
  cutline::engine
  cutline::gpu
  cutline::options
  winmm)

cutline_gpu_shaders(play_project)
