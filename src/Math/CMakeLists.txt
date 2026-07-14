file(GLOB_RECURSE SOURCES *.cpp *.h)
add_library(MeshForgeMath STATIC ${SOURCES})
target_include_directories(MeshForgeMath PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MeshForgeMath PUBLIC MeshForgeCore Eigen3::Eigen)
