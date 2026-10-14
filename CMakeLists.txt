cmake_minimum_required(VERSION 3.22)
project(voxel_engine VERSION 4.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(VOXEL_BUILD_TESTS "Build M0-M4 unit and integration test suites" ON)
option(VOXEL_BUILD_GDEXTENSION "Build Godot 4.2+ GDExtension shared library (requires godot-cpp)" OFF)
option(VOXEL_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer for tests" OFF)
option(VOXEL_ENABLE_TSAN "Enable ThreadSanitizer for concurrency tests" OFF)

# ==============================================================================
# 1. Pure C++20 Core Library Target (`voxel_core`, HLD v4.0 Section 6.1)
#    Zero Godot headers; statically linked into tests and libvoxel_engine.
# ==============================================================================
add_library(voxel_core INTERFACE)
target_include_directories(voxel_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(Threads REQUIRED)
target_link_libraries(voxel_core INTERFACE Threads::Threads)

if(MSVC)
    target_compile_options(voxel_core INTERFACE /W4 /permissive-)
else()
    target_compile_options(voxel_core INTERFACE -Wall -Wextra -pedantic)
endif()

# ==============================================================================
# 2. Godot 4.2+ GDExtension Shared Library (`libvoxel_engine`, Section 6.1)
# ==============================================================================
if(VOXEL_BUILD_GDEXTENSION)
    if(NOT TARGET godot::cpp)
        if(DEFINED GODOT_CPP_DIR AND EXISTS "${GODOT_CPP_DIR}/CMakeLists.txt")
            add_subdirectory("${GODOT_CPP_DIR}" godot-cpp EXCLUDE_FROM_ALL)
        else()
            message(FATAL_ERROR "VOXEL_BUILD_GDEXTENSION=ON requires godot::cpp target or -DGODOT_CPP_DIR=/path/to/godot-cpp")
        endif()
    endif()

    add_library(voxel_engine SHARED
        src/register_types.cpp
        src/voxel_terrain_3d.cpp
        src/voxel_ai_companion.cpp
    )
    target_compile_definitions(voxel_engine PRIVATE VOXEL_WITH_GODOT_CPP=1)
    target_link_libraries(voxel_engine PRIVATE voxel_core godot::cpp)

    if(MSVC)
        target_compile_options(voxel_engine PRIVATE /WX-)
    else()
        target_compile_options(voxel_engine PRIVATE -Wno-error)
    endif()

    set_target_properties(voxel_engine PROPERTIES
        PREFIX "lib"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    if(WIN32)
        add_custom_command(TARGET voxel_engine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:voxel_engine>"
                "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<IF:$<CONFIG:Debug>,libvoxel_engine.windows.debug.x86_64.dll,libvoxel_engine.windows.release.x86_64.dll>"
        )
    endif()
endif()

# ==============================================================================
# 3. CTest Unit & Integration Test Suites (Milestones M0 - M4, Section 7.1 & 7.2)
# ==============================================================================
if(VOXEL_BUILD_TESTS)
    enable_testing()

    set(VOXEL_SANITIZER_FLAGS "")
    if(VOXEL_ENABLE_ASAN AND NOT MSVC)
        list(APPEND VOXEL_SANITIZER_FLAGS -fsanitize=address,undefined)
    elseif(VOXEL_ENABLE_TSAN AND NOT MSVC)
        list(APPEND VOXEL_SANITIZER_FLAGS -fsanitize=thread)
    endif()

    function(add_voxel_test target_name source_file)
        add_executable(${target_name} ${source_file} ${ARGN})
        target_link_libraries(${target_name} PRIVATE voxel_core)
        if(VOXEL_SANITIZER_FLAGS)
            target_compile_options(${target_name} PRIVATE ${VOXEL_SANITIZER_FLAGS})
            target_link_options(${target_name} PRIVATE ${VOXEL_SANITIZER_FLAGS})
        endif()
        add_test(NAME ${target_name} COMMAND ${target_name})
    endfunction()

    add_voxel_test(test_chunk_key                tests/test_chunk_key.cpp)
    add_voxel_test(test_chunk_store              tests/test_chunk_store.cpp)
    add_voxel_test(test_scratch_and_dirty        tests/test_scratch_and_dirty.cpp)
    add_voxel_test(test_edit_and_worker_pool     tests/test_edit_and_worker_pool.cpp)
    add_voxel_test(test_procgen_and_surface_nets tests/test_procgen_and_surface_nets.cpp)
    add_voxel_test(test_persistence_and_streaming tests/test_persistence_and_streaming.cpp)
    add_voxel_test(test_integration_m3_m4        tests/test_integration_m3_m4.cpp)
    add_voxel_test(test_gdextension_bindings
        tests/test_gdextension_bindings.cpp
        src/register_types.cpp
        src/voxel_terrain_3d.cpp
        src/voxel_ai_companion.cpp
    )
endif()
