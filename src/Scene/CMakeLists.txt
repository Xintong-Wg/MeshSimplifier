file(GLOB_RECURSE SOURCES *.cpp *.h)
add_library(MeshForgeScene STATIC ${SOURCES})
target_include_directories(MeshForgeScene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MeshForgeScene PUBLIC MeshForgeCore MeshForgeMath MeshForgeMesh)
