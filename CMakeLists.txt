cmake_minimum_required(VERSION 3.25)

project(ParaMesh VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(PARAMESH_WERROR "Treat compiler warnings as errors" ON)
option(PARAMESH_CLANG_TIDY "Run clang-tidy on every compiled file" OFF)
option(PARAMESH_BUILD_TESTS "Build the unit and simulation tests" ON)
set(PARAMESH_SANITIZER "" CACHE STRING "Sanitizer for the whole build: address, undefined, thread or empty")
set_property(CACHE PARAMESH_SANITIZER PROPERTY STRINGS "" address undefined thread)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(ParameshToolchain)
include(ParameshModule)

# The public C API. Applications depend on this and on nothing else in the tree.
add_library(paramesh_api INTERFACE)
target_include_directories(paramesh_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(paramesh::api ALIAS paramesh_api)

# Before any directory is added, so that a test registered anywhere, apps/ included, is found.
if(PARAMESH_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(src)
add_subdirectory(apps)

if(PARAMESH_BUILD_TESTS)
    add_subdirectory(tests)
endif()
