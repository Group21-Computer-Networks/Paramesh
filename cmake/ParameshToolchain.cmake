# Whole-build settings: sanitizers and clang-tidy. Included once from the root CMakeLists.txt,
# before any target is created, so third-party code is instrumented the same way as ours.
#
# PARAMESH_TEST_LAUNCHER is a command prefix for every add_test(); empty unless a sanitizer
# needs one. Tests elsewhere use it too: add_test(NAME x COMMAND ${PARAMESH_TEST_LAUNCHER} x).

set(PARAMESH_TEST_LAUNCHER "")

if(PARAMESH_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif(PARAMESH_SANITIZER STREQUAL "undefined")
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined -fno-sanitize-recover=all)
elseif(PARAMESH_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
    # GCC 13's TSan runtime aborts with "unexpected memory mapping" on kernels with high mmap
    # randomisation (Ubuntu 24.04 and later). Running the test without ASLR avoids it and needs
    # no root, unlike lowering vm.mmap_rnd_bits. Clang's runtime re-executes itself the same way.
    find_program(PARAMESH_SETARCH setarch REQUIRED)
    set(PARAMESH_TEST_LAUNCHER ${PARAMESH_SETARCH} ${CMAKE_HOST_SYSTEM_PROCESSOR} -R)
elseif(NOT PARAMESH_SANITIZER STREQUAL "")
    message(FATAL_ERROR "PARAMESH_SANITIZER must be address, undefined, thread or empty, "
                        "not '${PARAMESH_SANITIZER}'")
endif()

if(PARAMESH_CLANG_TIDY)
    find_program(PARAMESH_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set(CMAKE_C_CLANG_TIDY ${PARAMESH_CLANG_TIDY_EXE})
    set(CMAKE_CXX_CLANG_TIDY ${PARAMESH_CLANG_TIDY_EXE})
endif()
