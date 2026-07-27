find_package(GTest CONFIG REQUIRED)

add_executable(cutline_core_tests
  core/animate_test.cpp
  core/arrange_test.cpp
  core/edit_test.cpp
  core/keyframe_test.cpp
  core/properties_test.cpp
  core/query_test.cpp
  core/segments_test.cpp
  core/time_test.cpp)

target_link_libraries(cutline_core_tests PRIVATE
  cutline::core
  GTest::gtest
  GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cutline_core_tests)
