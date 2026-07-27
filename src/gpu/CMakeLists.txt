add_library(cutline_gpu src/renderer.cpp)
add_library(cutline::gpu ALIAS cutline_gpu)

target_include_directories(cutline_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(cutline_gpu
  PUBLIC cutline::options
  PRIVATE d3d12 dxgi dxguid)

# Shaders are compiled next to whichever executable links this library, so the
# path is exposed for those targets to use.
set(CUTLINE_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl"
    CACHE INTERNAL "Path to the presentation shader source")
