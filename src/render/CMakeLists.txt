add_library(cutline_render src/plan.cpp)
add_library(cutline::render ALIAS cutline_render)

target_include_directories(cutline_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Deliberately no GPU dependency: a frame plan is a pure function of the model,
# and keeping it that way is what lets it be tested without a device.
target_link_libraries(cutline_render PUBLIC cutline::core cutline::options)
