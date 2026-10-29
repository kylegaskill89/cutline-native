add_library(cutline_render
  src/auto_reframe.cpp
  src/effect_catalog.cpp
  src/effect_passes.cpp
  src/effects.cpp
  src/lumetri.cpp
  src/morph_cut.cpp
  src/transform_effect.cpp
  src/ultra_key.cpp
  src/warp_stabilizer.cpp
  src/mix.cpp
  src/plan.cpp
  src/preview.cpp
  src/scopes.cpp)
add_library(cutline::render ALIAS cutline_render)

target_include_directories(cutline_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Deliberately no GPU dependency: a frame plan is a pure function of the model,
# and keeping it that way is what lets it be tested without a device.
target_link_libraries(cutline_render PUBLIC cutline::core cutline::track cutline::options)
