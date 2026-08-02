add_library(cutline_gpu
  src/compositor.cpp
  src/device.cpp
  src/presenter.cpp)
add_library(cutline::gpu ALIAS cutline_gpu)

target_include_directories(cutline_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(cutline_gpu
  PUBLIC cutline::options
  PRIVATE d3d12 dxgi dxguid)

# Compiled shaders are loaded at runtime from beside the executable, so every
# target that links this library needs them built next to it. Listing the entry
# points here rather than in each consumer keeps one place to change when a
# shader is added.
include(CompileShaders)

function(cutline_gpu_shaders TARGET)
  # Resolved against the directory defining this function, not the caller's, so
  # consumers in other directories find the source.
  set(_shader "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shaders/composite.hlsl")

  cutline_compile_shader(${TARGET} "${_shader}" VSLayer vs_6_0 composite_layer_vs.cso)
  cutline_compile_shader(${TARGET} "${_shader}" VSFullscreen vs_6_0 composite_fullscreen_vs.cso)
  cutline_compile_shader(${TARGET} "${_shader}" PSLayer ps_6_0 composite_layer_ps.cso)
  cutline_compile_shader(${TARGET} "${_shader}" PSLayerBlend ps_6_0 composite_blend_ps.cso)
  cutline_compile_shader(${TARGET} "${_shader}" PSAdjustment ps_6_0 composite_adjustment_ps.cso)
  cutline_compile_shader(${TARGET} "${_shader}" PSSource ps_6_0 composite_source_ps.cso)
  cutline_compile_shader(${TARGET} "${_shader}" PSAdjustSource ps_6_0 composite_adjust_source_ps.cso)
  cutline_compile_shader(${TARGET} "${_shader}" PSEffect ps_6_0 composite_effect_ps.cso)
  cutline_compile_shader(${TARGET} "${_shader}" PSPresent ps_6_0 composite_present_ps.cso)
endfunction()
