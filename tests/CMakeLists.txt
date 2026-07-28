find_package(GTest CONFIG REQUIRED)

add_executable(cutline_core_tests
  core/animate_test.cpp
  core/arrange_test.cpp
  core/edit_test.cpp
  core/keyframe_test.cpp
  core/layout_test.cpp
  core/properties_test.cpp
  core/query_test.cpp
  core/segments_test.cpp
  core/serialize_test.cpp
  core/time_test.cpp
  render/effects_test.cpp
  render/plan_test.cpp)

target_link_libraries(cutline_core_tests PRIVATE
  cutline::core
  cutline::render
  GTest::gtest
  GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cutline_core_tests)

if(CUTLINE_WITH_MEDIA)
  add_executable(cutline_media_tests
    media/audio_test.cpp
    media/probe_test.cpp)
  target_link_libraries(cutline_media_tests PRIVATE
    cutline::media
    GTest::gtest
    GTest::gtest_main)
  # These skip unless CUTLINE_TEST_MEDIA_DIR points at the reference footage.
  gtest_discover_tests(cutline_media_tests)
endif()

if(CUTLINE_WITH_GPU)
  add_executable(cutline_gpu_tests gpu/compositor_test.cpp)
  target_link_libraries(cutline_gpu_tests PRIVATE
    cutline::gpu
    GTest::gtest
    GTest::gtest_main)
  # The compositor loads its compiled shaders from beside the executable, and
  # falls back to WARP so this runs on machines with no GPU.
  cutline_gpu_shaders(cutline_gpu_tests)
  gtest_discover_tests(cutline_gpu_tests)

  if(CUTLINE_WITH_MEDIA)
    # End to end: a project in, pixels out. Most of these use generated media
    # only, so they run without footage; the ones that need it skip.
    add_executable(cutline_engine_tests engine/frame_renderer_test.cpp)
    target_link_libraries(cutline_engine_tests PRIVATE
      cutline::core
      cutline::engine
      GTest::gtest
      GTest::gtest_main)
    cutline_gpu_shaders(cutline_engine_tests)
    gtest_discover_tests(cutline_engine_tests)
  endif()
endif()
