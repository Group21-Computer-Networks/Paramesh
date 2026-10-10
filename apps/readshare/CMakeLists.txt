# The M1 program, on tasks since M3-7: the launcher writes, tasks on every node read. It uses
# paramesh.h only.
add_executable(readshare main.c)
target_link_libraries(readshare PRIVATE paramesh::api paramesh::lib)
paramesh_target_settings(readshare)
