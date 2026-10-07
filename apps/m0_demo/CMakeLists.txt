# The M0 demo: a program that reads and writes memory it never allocated. It uses the memory
# engine directly, because libparamesh does not exist before M1-7; every later application
# uses paramesh.h only.
add_executable(m0_demo main.cpp)
target_link_libraries(m0_demo PRIVATE paramesh::mem)
paramesh_target_settings(m0_demo)

# Run it as a test too, except under ThreadSanitizer, which cannot map the region (M0-2).
# Exit status 77 means the machine has no user-mode userfaultfd, and shows as "skipped".
if(PARAMESH_BUILD_TESTS AND NOT PARAMESH_SANITIZER STREQUAL "thread")
    add_test(NAME app.m0_demo COMMAND ${PARAMESH_TEST_LAUNCHER} $<TARGET_FILE:m0_demo>)
    set_tests_properties(app.m0_demo PROPERTIES SKIP_RETURN_CODE 77)
endif()
