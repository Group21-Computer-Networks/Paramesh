# The M2 program, on tasks since M3-7: the nodes of a job add to one shared number, under a
# lock and then with pm_atomic_add. It uses paramesh.h only.
add_executable(counter main.c)
target_link_libraries(counter PRIVATE paramesh::api paramesh::lib)
paramesh_target_settings(counter)
