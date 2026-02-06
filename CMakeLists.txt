# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

cmake_minimum_required(VERSION 3.14) # Required for FetchContent

project(NosDxAppSampleAdv)
include(FetchContent)


set(NODOS_WORKSPACE_DIR "../../" CACHE PATH "Path to Nodos workspace")

message(STATUS "Nodos workspace directory: ${NODOS_WORKSPACE_DIR}")

set(NODOS_PROCESS_SDK_VERSION "18.4" CACHE STRING "Version of Nodos SDK to use")
set(NOS_SYS_VULKAN_VERSION "6.0" CACHE STRING "Version of nos.sys.vulkan module to use")
# Nodos SDK Configuration
if (EXISTS ${NODOS_WORKSPACE_DIR}/nodos${CMAKE_EXECUTABLE_SUFFIX})
    set(NOSMAN_WORKSPACE_DIR ${NODOS_WORKSPACE_DIR})
    set(NOSMAN_EXECUTABLE ${NODOS_WORKSPACE_DIR}/nodos${CMAKE_EXECUTABLE_SUFFIX})
    cmake_path(SET NOSMAN_EXECUTABLE ${NOSMAN_EXECUTABLE} NORMALIZE)
    execute_process(
        COMMAND ${NOSMAN_EXECUTABLE} --workspace "${NODOS_WORKSPACE_DIR}" sdk-info "${NODOS_PROCESS_SDK_VERSION}" "process"
        OUTPUT_VARIABLE NOSMAN_SDK_INFO
    )
    string(JSON NODOS_SDK_DIR GET ${NOSMAN_SDK_INFO} "path")
    string(STRIP ${NODOS_SDK_DIR} NODOS_SDK_DIR)
    set(NODOS_SDK_DIR ${NODOS_SDK_DIR} CACHE PATH "Path to Nodos SDK")
    string(REPLACE "\\" "/" NODOS_SDK_DIR ${NODOS_SDK_DIR})
else()
    message(FATAL_ERROR "Nodos CLI not found: '${NODOS_WORKSPACE_DIR}/nodos${CMAKE_EXECUTABLE_SUFFIX}'")
endif()

# SDL2 (Minimal setup)
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.8
)
FetchContent_MakeAvailable(SDL2)

# Set SDL configuration flags
set(SDL_ATOMIC OFF CACHE BOOL "" FORCE)
set(SDL_CPUINFO OFF CACHE BOOL "" FORCE)
set(SDL_EVENTS ON CACHE BOOL "" FORCE)
set(SDL_FILE OFF CACHE BOOL "" FORCE)
set(SDL_FILESYSTEM OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_LOCALE OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_TIMERS OFF CACHE BOOL "" FORCE)
set(SDL_DLOPEN OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_LOADSO ON CACHE BOOL "" FORCE)
set(SDL_VIDEO ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)

# DirectX-Headers
FetchContent_Declare(
    DirectX-Headers
    GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
    GIT_TAG v1.616.0
)
FetchContent_MakeAvailable(DirectX-Headers)

FetchContent_Declare(
    argparse
    GIT_REPOSITORY https://github.com/p-ranav/argparse.git
)
FetchContent_MakeAvailable(argparse)

list(APPEND CMAKE_MODULE_PATH ${NODOS_SDK_DIR}/cmake)

if (NOT EXISTS ${NODOS_WORKSPACE_DIR}/Toolchain/CMake)
    message(FATAL_ERROR "Nodos CMake tools not found in workspace")
endif()

include(${NODOS_WORKSPACE_DIR}/Toolchain/CMake/Scripts/Utils.cmake)
include(${NODOS_WORKSPACE_DIR}/Toolchain/CMake/Scripts/Projects.cmake)

# Get nos.sys.vulkan
nos_get_module("nos.sys.vulkan" "${NOS_SYS_VULKAN_VERSION}" NOS_SYS_VULKAN_TARGET)
message(STATUS "nos.sys.vulkan target: ${NOS_SYS_VULKAN_TARGET}")

add_library(nosAppSDK INTERFACE)
target_include_directories(nosAppSDK INTERFACE ${NODOS_SDK_DIR}/include)

set(CMAKE_CXX_STANDARD 20)

file(GLOB_RECURSE SOURCES Source/*.cpp Source/*.hpp)

add_executable(NosDxAppSampleAdv ${SOURCES})
target_include_directories(NosDxAppSampleAdv PRIVATE Source)
target_link_libraries(NosDxAppSampleAdv PRIVATE
    nosAppSDK
    ${NOS_SYS_VULKAN_TARGET}
    d3d12 dxgi d3dcompiler
    SDL2-static
    DirectX-Headers
    Shlwapi.lib
    argparse
)

target_compile_definitions(NosDxAppSampleAdv PRIVATE NODOS_APP_SDK_DLL="${NODOS_SDK_DIR}/bin/nosAppSDK.dll")

set_target_properties(NosDxAppSampleAdv PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
)
