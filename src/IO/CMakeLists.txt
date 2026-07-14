file(GLOB_RECURSE SOURCES *.cpp *.h)
add_library(MeshForgeIO STATIC ${SOURCES})
target_include_directories(MeshForgeIO PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MeshForgeIO PUBLIC MeshForgeCore MeshForgeMath MeshForgeMesh MeshForgeScene nlohmann_json::nlohmann_json draco::draco)
