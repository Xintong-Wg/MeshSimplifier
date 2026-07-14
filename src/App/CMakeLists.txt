file(GLOB_RECURSE SOURCES *.cpp *.h)
add_library(MeshForgeAppLib STATIC ${SOURCES})
target_include_directories(MeshForgeAppLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MeshForgeAppLib PUBLIC
    MeshForgeCore MeshForgeMath MeshForgeGeometry MeshForgeMesh
    MeshForgeScene MeshForgeRenderer MeshForgeIO MeshForgeUI
    SDL2::SDL2
    "-framework OpenGL"
)
if(TBB_FOUND)
    target_link_libraries(MeshForgeAppLib PUBLIC TBB::tbb)
endif()
