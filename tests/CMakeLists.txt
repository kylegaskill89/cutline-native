find_package(GTest CONFIG REQUIRED)

add_executable(cutline_core_tests
  audio/biquad_test.cpp
  audio/chain_test.cpp
  audio/limiter_test.cpp
  audio/meter_test.cpp
  audio/stretch_test.cpp
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
  editor/autosave_test.cpp
  editor/browser_binding_test.cpp
  editor/commands_test.cpp
  editor/document_test.cpp
  editor/effects_binding_test.cpp
  editor/generators_test.cpp
  editor/import_test.cpp
  editor/inspector_test.cpp
  editor/keyframes_test.cpp
  editor/monitor_binding_test.cpp
  editor/bins_test.cpp
  editor/presets_test.cpp
  editor/session_test.cpp
  editor/timeline_binding_test.cpp
  editor/titles_test.cpp
  editor/transitions_test.cpp
  editor/updates_test.cpp
  editor/workspace_test.cpp
  render/effect_catalog_test.cpp
  render/effect_passes_test.cpp
  render/effects_test.cpp
  render/mix_test.cpp
  render/plan_test.cpp
  render/preview_test.cpp
  render/scopes_test.cpp
  ui/browser_test.cpp
  ui/color_picker_test.cpp
  ui/containers_test.cpp
  ui/controls_test.cpp
  ui/dock_test.cpp
  ui/dock_view_test.cpp
  ui/effects_browser_test.cpp
  ui/keyframe_view_test.cpp
  ui/layout_test.cpp
  ui/meter_view_test.cpp
  ui/monitor_test.cpp
  ui/painter_test.cpp
  ui/scopes_view_test.cpp
  ui/theme_test.cpp
  ui/timeline_test.cpp
  ui/timescale_test.cpp
  ui/widget_test.cpp
  ui/widgets_test.cpp)

target_link_libraries(cutline_core_tests PRIVATE
  cutline::audio
  cutline::core
  cutline::editor
  cutline::render
  cutline::ui
  GTest::gtest
  GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cutline_core_tests)

if(CUTLINE_WITH_SKIA)
  # Rendered to a CPU raster surface and read back, so these need no GPU and no
  # window. `RecordingPainter` proves the right calls are made in the right
  # order; these prove they land as the right pixels.
  # Skia is private to cutline::ui_skia so it does not leak upward; this test is
  # the one place that legitimately needs it, to make a surface to draw into.
  find_package(unofficial-skia CONFIG REQUIRED)

  # Titles: drawn to a raster surface and read back, so no GPU and no window.
  add_executable(cutline_text_tests text/raster_test.cpp)
  target_link_libraries(cutline_text_tests PRIVATE
    cutline::text
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(cutline_text_tests)

  add_executable(cutline_ui_skia_tests ui/skia_painter_test.cpp)
  target_link_libraries(cutline_ui_skia_tests PRIVATE
    cutline::ui
    cutline::ui_skia
    unofficial::skia::skia
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(cutline_ui_skia_tests)
endif()

if(CUTLINE_WITH_MEDIA)
  add_executable(cutline_media_tests
    media/audio_test.cpp
    media/encoder_test.cpp
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
    add_executable(cutline_engine_tests
      engine/audio_mixer_test.cpp
      engine/exporter_test.cpp
      engine/frame_renderer_test.cpp
      engine/player_test.cpp)
    target_link_libraries(cutline_engine_tests PRIVATE
      cutline::core
      cutline::engine
      cutline::media
      GTest::gtest
      GTest::gtest_main)
    cutline_gpu_shaders(cutline_engine_tests)
    gtest_discover_tests(cutline_engine_tests)

    # The whole stack: a file on disk becomes pixels for the monitor. Every
    # layer is already tested without media; this is the only place they are
    # checked against a real one, so it skips unless the footage is present.
    add_executable(cutline_app_tests
      app/preview_test.cpp
      app/thumbnails_test.cpp
      app/waveforms_test.cpp)
    target_link_libraries(cutline_app_tests PRIVATE
      cutline::app
      GTest::gtest
      GTest::gtest_main)
    cutline_gpu_shaders(cutline_app_tests)
    gtest_discover_tests(cutline_app_tests)
  endif()
endif()
