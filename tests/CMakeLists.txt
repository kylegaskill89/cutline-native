find_package(GTest CONFIG REQUIRED)

add_executable(cutline_core_tests
  audio/biquad_test.cpp
  audio/chain_test.cpp
  audio/limiter_test.cpp
  audio/loudness_test.cpp
  audio/meter_test.cpp
  audio/music_test.cpp
  audio/repair_test.cpp
  audio/stretch_test.cpp
  audio/sync_test.cpp
  core/animate_test.cpp
  core/arrange_test.cpp
  core/attributes_test.cpp
  core/captions_test.cpp
  core/edit_test.cpp
  core/graphic_test.cpp
  core/interpret_test.cpp
  core/keyframe_test.cpp
  core/layout_test.cpp
  core/mask_path_test.cpp
  core/multicam_test.cpp
  core/nesting_test.cpp
  core/pool_test.cpp
  core/properties_test.cpp
  core/query_test.cpp
  core/remix_test.cpp
  core/roles_test.cpp
  core/routing_test.cpp
  core/segments_test.cpp
  core/sequences_test.cpp
  core/serialize_test.cpp
  core/subclips_test.cpp
  core/time_test.cpp
  editor/autosave_test.cpp
  editor/browser_binding_test.cpp
  editor/commands_test.cpp
  editor/document_test.cpp
  editor/edit_names_test.cpp
  editor/effects_binding_test.cpp
  editor/generators_test.cpp
  editor/export_presets_test.cpp
  editor/import_test.cpp
  editor/inspector_test.cpp
  editor/keyframes_test.cpp
  editor/monitor_binding_test.cpp
  editor/bins_test.cpp
  editor/presets_test.cpp
  editor/render_previews_test.cpp
  editor/session_test.cpp
  editor/settings_test.cpp
  editor/shortcuts_test.cpp
  editor/timeline_binding_test.cpp
  editor/graphics_test.cpp
  editor/graphic_templates_test.cpp
  editor/lumetri_test.cpp
  editor/mask_tracking_test.cpp
  editor/zip_test.cpp
  editor/transitions_test.cpp
  editor/trim_test.cpp
  editor/updates_test.cpp
  editor/workspace_test.cpp
  render/effect_catalog_test.cpp
  render/effect_passes_test.cpp
  render/effects_test.cpp
  render/lumetri_test.cpp
  render/transform_effect_test.cpp
  render/ultra_key_test.cpp
  render/mix_test.cpp
  render/plan_test.cpp
  render/preview_test.cpp
  render/scopes_test.cpp
  track/scenes_test.cpp
  track/stabilize_test.cpp
  track/tracker_test.cpp
  ui/animator_test.cpp
  ui/browser_test.cpp
  ui/color_picker_test.cpp
  ui/grading_test.cpp
  ui/containers_test.cpp
  ui/controls_test.cpp
  ui/dialog_test.cpp
  ui/dock_test.cpp
  ui/dock_view_test.cpp
  ui/effects_browser_test.cpp
  ui/keyboard_view_test.cpp
  ui/keyframe_view_test.cpp
  ui/layout_test.cpp
  ui/meter_view_test.cpp
  ui/monitor_test.cpp
  ui/painter_test.cpp
  ui/scopes_view_test.cpp
  ui/scrub_bar_test.cpp
  ui/theme_test.cpp
  ui/timeline_test.cpp
  ui/timescale_test.cpp
  ui/waveform_view_test.cpp
  ui/widget_test.cpp
  ui/widgets_test.cpp)

target_link_libraries(cutline_core_tests PRIVATE
  cutline::audio
  cutline::core
  cutline::editor
  cutline::render
  cutline::track
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
    media/formats_test.cpp
    media/probe_test.cpp
    media/transcode_test.cpp)
  target_link_libraries(cutline_media_tests PRIVATE
    cutline::media
    GTest::gtest
    GTest::gtest_main)
  # The small committed files formats_test opens; see media/fixtures/make.sh.
  target_compile_definitions(cutline_media_tests PRIVATE
    CUTLINE_MEDIA_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/media/fixtures")
  # These skip unless CUTLINE_TEST_MEDIA_DIR points at the reference footage.
  gtest_discover_tests(cutline_media_tests)
endif()

if(CUTLINE_WITH_GPU)
  add_executable(cutline_gpu_tests gpu/compositor_test.cpp gpu/frame_ring_test.cpp)
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
      engine/mask_tracker_test.cpp
      engine/multicam_sync_test.cpp
      engine/music_analysis_test.cpp
      engine/scene_detection_test.cpp
      engine/stabilizer_test.cpp
      engine/player_test.cpp)
    target_link_libraries(cutline_engine_tests PRIVATE
      cutline::core
      cutline::engine
      cutline::media
      cutline::render
      GTest::gtest
      GTest::gtest_main)
    cutline_gpu_shaders(cutline_engine_tests)
    # The 10-bit, PQ and HLG footage the colour tests render; see
    # media/fixtures/make.sh.
    target_compile_definitions(cutline_engine_tests PRIVATE
      CUTLINE_MEDIA_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/media/fixtures")
    # The player's clock is checked for steadiness against the wall clock, and
    # with every core busy running the rest of the suite its audio thread is
    # held up by a whole buffer period now and then -- a fact about a
    # saturated machine, not about the clock. So that one runs on its own.
    set(clock_test "PlayerTest.TheClockMovesSmoothlyRatherThanInSteps")
    gtest_discover_tests(cutline_engine_tests TEST_FILTER "-${clock_test}")
    gtest_discover_tests(cutline_engine_tests TEST_FILTER "${clock_test}"
      PROPERTIES RUN_SERIAL TRUE)

    # The whole stack: a file on disk becomes pixels for the monitor. Every
    # layer is already tested without media; this is the only place they are
    # checked against a real one, so it skips unless the footage is present.
    add_executable(cutline_app_tests
      app/media_cache_test.cpp
      app/preview_engine_test.cpp
      app/preview_test.cpp
      app/proxies_test.cpp
      app/thumbnails_test.cpp
      app/waveforms_test.cpp)
    target_link_libraries(cutline_app_tests PRIVATE
      cutline::app
      cutline::media
      cutline::engine
      GTest::gtest
      GTest::gtest_main)
    cutline_gpu_shaders(cutline_app_tests)
    target_compile_definitions(cutline_app_tests PRIVATE
      CUTLINE_MEDIA_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/media/fixtures")
    gtest_discover_tests(cutline_app_tests)
  endif()
endif()
