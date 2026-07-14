# ------------------------------------------------------------------
# Third-party library builds: bx, bgfx, imgui
# ------------------------------------------------------------------

# --- bx (base library for bgfx) ---
add_library(bx STATIC
    bgfx/bx/src/amalgamated.cpp
)
target_include_directories(bx PUBLIC
    bgfx/bx/include
    bgfx/bx/3rdparty
    ${CMAKE_CURRENT_SOURCE_DIR}/bx_compat
)
target_compile_definitions(bx PRIVATE
    __STDC_LIMIT_MACROS
    __STDC_FORMAT_MACROS
    __STDC_CONSTANT_MACROS
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(bx PUBLIC BX_CONFIG_DEBUG=1)
else()
    target_compile_definitions(bx PUBLIC BX_CONFIG_DEBUG=0)
endif()
set_target_properties(bx PROPERTIES CXX_VISIBILITY_PRESET hidden)

# --- bimg stub (minimal symbols bgfx needs) ---
add_library(bimg STATIC
    bimg_stub/bimg_stub.cpp
)
target_include_directories(bimg PUBLIC
    bgfx/bimg/include
    bgfx/bx/include
    bimg_stub
)
target_link_libraries(bimg PUBLIC bx)
set_target_properties(bimg PROPERTIES CXX_VISIBILITY_PRESET hidden)

# --- bgfx ---
add_library(bgfx STATIC
    bgfx/src/amalgamated.cpp
)
target_include_directories(bgfx PUBLIC
    bgfx/include
    bgfx/bx/include
    bgfx/bimg/include
    bgfx/3rdparty
)
target_link_libraries(bgfx PUBLIC bx bimg)
target_link_libraries(bgfx PUBLIC
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Cocoa"
    "-framework IOKit"
)
target_compile_definitions(bgfx PRIVATE
    BGFX_CONFIG_RENDERER_METAL=1
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(bgfx PUBLIC BGFX_CONFIG_DEBUG=1)
else()
    target_compile_definitions(bgfx PUBLIC BGFX_CONFIG_DEBUG=1)  # Enable debug text for now
endif()
# Metal renderer needs Objective-C++ compilation on macOS
set_source_files_properties(
    bgfx/src/amalgamated.cpp
    PROPERTIES
    COMPILE_FLAGS "-x objective-c++"
)
set_target_properties(bgfx PROPERTIES CXX_VISIBILITY_PRESET hidden)

# --- imgui core ---
set(IMGUI_SOURCES
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC imgui)
set_target_properties(imgui PROPERTIES CXX_VISIBILITY_PRESET hidden)

# --- imgui GLFW backend ---
add_library(imgui_glfw STATIC
    imgui/backends/imgui_impl_glfw.cpp
)
target_include_directories(imgui_glfw PUBLIC
    imgui
    imgui/backends
    /opt/homebrew/include  # for GLFW/glfw3.h
)
find_library(GLFW_LIBRARY glfw PATHS /opt/homebrew/lib)
if(GLFW_LIBRARY)
    target_link_libraries(imgui_glfw PUBLIC imgui ${GLFW_LIBRARY})
endif()
set_target_properties(imgui_glfw PROPERTIES CXX_VISIBILITY_PRESET hidden)

# --- imgui bgfx backend (custom minimal implementation) ---
add_library(imgui_bgfx STATIC
    imgui_bgfx_backend/imgui_bgfx_backend.cpp
)
target_include_directories(imgui_bgfx PUBLIC
    imgui
    imgui_bgfx_backend
    bgfx/include
    bgfx/bx/include
    bgfx/examples/common/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(imgui_bgfx PUBLIC imgui bgfx)
set_target_properties(imgui_bgfx PROPERTIES CXX_VISIBILITY_PRESET hidden)
