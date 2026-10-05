# doctest unit tests. Every <name>_test.cpp in this directory becomes its own executable and its
# own ctest entry, so tasks add test files without editing this one. main.cpp supplies main().

add_library(paramesh_doctest_main OBJECT main.cpp)
target_link_libraries(paramesh_doctest_main PUBLIC paramesh::doctest)
paramesh_target_settings(paramesh_doctest_main)

file(GLOB unit_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
foreach(source IN LISTS unit_tests)
    get_filename_component(stem ${source} NAME_WE)
    add_executable(unit_${stem} ${source})
    target_link_libraries(unit_${stem} PRIVATE
        paramesh_doctest_main paramesh::lib paramesh::pmd paramesh::tools)
    target_compile_definitions(unit_${stem} PRIVATE
        PARAMESH_SANITIZER_NAME="${PARAMESH_SANITIZER}")
    paramesh_target_settings(unit_${stem})
    add_test(NAME unit.${stem} COMMAND ${PARAMESH_TEST_LAUNCHER} $<TARGET_FILE:unit_${stem}>)
endforeach()
