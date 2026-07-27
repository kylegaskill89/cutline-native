find_package(GTest CONFIG REQUIRED)

add_executable(cutline_core_tests
  core/keyframe_test.cpp
  core/time_test.cpp)

target_link_libraries(cutline_core_tests PRIVATE
  cutline::core
  GTest::gtest
  GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cutline_core_tests)
