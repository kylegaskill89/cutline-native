add_executable(decode_bench main.cpp)

target_link_libraries(decode_bench PRIVATE
  cutline::media
  cutline::options)
