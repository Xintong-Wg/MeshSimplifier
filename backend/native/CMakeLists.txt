add_executable(MeshSimplifierCli
    MeshSimplifierCli.cpp
)

target_include_directories(MeshSimplifierCli PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(MeshSimplifierCli PRIVATE
    MeshForgeCore
    MeshForgeMath
    MeshForgeGeometry
    MeshForgeMesh
    MeshForgeScene
    MeshForgeIO
    nlohmann_json::nlohmann_json
)

set_target_properties(MeshSimplifierCli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(MeshSimplifierServer
    MeshSimplifierServer.cpp
)

if(WIN32)
    target_link_libraries(MeshSimplifierServer PRIVATE Shell32 Ws2_32)
endif()

set_target_properties(MeshSimplifierServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
