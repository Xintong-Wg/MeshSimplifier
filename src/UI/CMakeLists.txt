file(GLOB_RECURSE SOURCES *.cpp *.h *.mm)
add_library(MeshForgeUI STATIC ${SOURCES})
target_include_directories(MeshForgeUI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MeshForgeUI PUBLIC MeshForgeCore MeshForgeScene MeshForgeRenderer imgui imgui_sdl imgui_gl
    "-framework AppKit" "-framework Foundation"
)
