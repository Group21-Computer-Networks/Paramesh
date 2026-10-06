# Helpers that give every ParaMesh target the same settings.
#
# Source files are found by glob, so a task adds a .cpp file and never edits a CMakeLists.txt
# that another stream also edits. CONFIGURE_DEPENDS makes the build re-glob when files appear.

# paramesh_target_settings(<target> [NO_EXCEPTIONS])
#   Warnings (as errors unless PARAMESH_WERROR is OFF) and, for libparamesh code, exceptions off.
function(paramesh_target_settings target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NO_EXCEPTIONS" "" "")
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wcast-align
        -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wundef
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual>
        $<$<BOOL:${PARAMESH_WERROR}>:-Werror>
        $<$<BOOL:${arg_NO_EXCEPTIONS}>:$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>>)
endfunction()

# paramesh_module(<name> [NO_EXCEPTIONS] [DEPS <target>...])
#   Defines paramesh_<name> (alias paramesh::<name>) for the calling src/ directory.
#   With no .c or .cpp files yet it is an INTERFACE target that carries the include root and the
#   dependency edges; once sources exist it becomes a static library. Headers are included as
#   "<dir>/<file>.h" relative to src/.
function(paramesh_module name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NO_EXCEPTIONS" "" "DEPS")
    set(target paramesh_${name})
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
    if(sources)
        add_library(${target} STATIC ${sources})
        target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(${target} PUBLIC ${arg_DEPS})
        if(arg_NO_EXCEPTIONS)
            paramesh_target_settings(${target} NO_EXCEPTIONS)
        else()
            paramesh_target_settings(${target})
        endif()
    else()
        add_library(${target} INTERFACE)
        target_include_directories(${target} INTERFACE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(${target} INTERFACE ${arg_DEPS})
    endif()
    add_library(paramesh::${name} ALIAS ${target})
endfunction()

# paramesh_add_subdirectories()
#   add_subdirectory() for every child directory that has a CMakeLists.txt, so apps/ and similar
#   directories gain members without a shared file being edited.
function(paramesh_add_subdirectories)
    file(GLOB children LIST_DIRECTORIES true CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*)
    list(SORT children)
    foreach(child IN LISTS children)
        if(IS_DIRECTORY ${child} AND EXISTS ${child}/CMakeLists.txt)
            add_subdirectory(${child})
        endif()
    endforeach()
endfunction()
