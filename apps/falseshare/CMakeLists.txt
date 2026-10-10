# The false-sharing demo of AT-1: two tasks write neighbouring numbers in one page.
add_executable(falseshare main.c)
target_link_libraries(falseshare PRIVATE paramesh::api paramesh::lib)
paramesh_target_settings(falseshare)
