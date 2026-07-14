cmake_minimum_required(VERSION 3.25)

project(MeshSimplifier
    VERSION 0.1.0
    LANGUAGES CXX C
    DESCRIPTION "Windows-first STEP mesh simplification backend with a web viewer"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MESH_SIMPLIFIER_BUILD_LEGACY_DESKTOP "Build the imported MeshForge SDL/ImGui desktop app" OFF)
option(MESH_SIMPLIFIER_USE_SYSTEM_MESHOPTIMIZER "Prefer meshoptimizer from vcpkg/package config" ON)

macro(ms_find_required_package package_name)
    find_package(${package_name} CONFIG QUIET)
    if(NOT ${package_name}_FOUND AND NOT TARGET ${package_name}::${package_name})
        message(FATAL_ERROR
            "${package_name} is required. On Windows install dependencies with vcpkg and configure with "
            "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"
        )
    endif()
endmacro()

find_package(Threads REQUIRED)
ms_find_required_package(glm)
ms_find_required_package(spdlog)
ms_find_required_package(nlohmann_json)
ms_find_required_package(Eigen3)
find_package(TBB CONFIG QUIET)
find_package(OpenCASCADE CONFIG QUIET)
find_package(draco CONFIG QUIET)

if(NOT TARGET TBB::tbb)
    message(FATAL_ERROR
        "TBB is required. On Windows install dependencies with vcpkg and configure with "
        "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"
    )
endif()

if(NOT OpenCASCADE_FOUND)
    find_package(OpenCASCADE QUIET)
endif()

if(OpenCASCADE_FOUND AND NOT TARGET OpenCASCADE::OpenCASCADE)
    add_library(OpenCASCADE::OpenCASCADE INTERFACE IMPORTED)
    target_include_directories(OpenCASCADE::OpenCASCADE INTERFACE
        ${OpenCASCADE_INCLUDE_DIR}
        ${OpenCASCADE_INCLUDE_DIRS}
    )
    target_link_libraries(OpenCASCADE::OpenCASCADE INTERFACE ${OpenCASCADE_LIBRARIES})
endif()

if(NOT TARGET OpenCASCADE::OpenCASCADE)
    message(FATAL_ERROR "OpenCASCADE is required. On Windows install dependencies with vcpkg and pass -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake")
endif()

if(MESH_SIMPLIFIER_USE_SYSTEM_MESHOPTIMIZER)
    find_package(meshoptimizer CONFIG QUIET)
endif()

if(NOT TARGET meshoptimizer::meshoptimizer AND TARGET meshoptimizer)
    add_library(meshoptimizer::meshoptimizer ALIAS meshoptimizer)
endif()

if(NOT TARGET meshoptimizer::meshoptimizer)
    include(FetchContent)
    FetchContent_Declare(
        meshoptimizer
        GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
        GIT_TAG v0.21
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(meshoptimizer)
    if(TARGET meshoptimizer AND NOT TARGET meshoptimizer::meshoptimizer)
        add_library(meshoptimizer::meshoptimizer ALIAS meshoptimizer)
    endif()
endif()

if(TARGET Draco::draco AND NOT TARGET draco::draco)
    add_library(draco::draco ALIAS Draco::draco)
elseif(TARGET draco AND NOT TARGET draco::draco)
    add_library(draco::draco ALIAS draco)
endif()

if(NOT TARGET draco::draco)
    add_library(draco_stub INTERFACE)
    target_include_directories(draco_stub INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/draco_stub)
    add_library(draco::draco ALIAS draco_stub)
endif()

add_library(fast_quadic_simplification INTERFACE)
target_include_directories(fast_quadic_simplification INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/FastQuadricMeshSimplification
)

add_subdirectory(src/Core)
add_subdirectory(src/Math)
add_subdirectory(src/Geometry)
add_subdirectory(src/Mesh)
add_subdirectory(src/Scene)
add_subdirectory(src/IO)
add_subdirectory(backend/native)

if(MESH_SIMPLIFIER_BUILD_LEGACY_DESKTOP)
    add_subdirectory(src/Renderer)
    add_subdirectory(src/UI)
    add_subdirectory(src/App)
    add_executable(MeshForgeApp src/main.cpp)
    target_link_libraries(MeshForgeApp PRIVATE MeshForgeAppLib)
    set_target_properties(MeshForgeApp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()

add_executable(StepProbe tools/StepProbe.cpp)
target_include_directories(StepProbe PRIVATE src)
target_link_libraries(StepProbe PRIVATE MeshForgeCore MeshForgeMath MeshForgeGeometry)
set_target_properties(StepProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

enable_testing()
