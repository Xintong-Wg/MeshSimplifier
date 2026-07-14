file(GLOB_RECURSE SOURCES *.cpp *.h)
add_library(MeshForgeRenderer STATIC ${SOURCES})
target_include_directories(MeshForgeRenderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MeshForgeRenderer PUBLIC MeshForgeCore MeshForgeMath)
