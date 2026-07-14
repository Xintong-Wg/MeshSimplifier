file(GLOB_RECURSE SOURCES *.cpp *.h)
add_library(MeshForgeMesh STATIC ${SOURCES})
target_include_directories(MeshForgeMesh PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/third_party/FastQuadricMeshSimplification
)
target_link_libraries(MeshForgeMesh PUBLIC MeshForgeCore MeshForgeMath MeshForgeGeometry meshoptimizer::meshoptimizer fast_quadic_simplification)
