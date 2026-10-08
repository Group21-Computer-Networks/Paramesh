# The M2 test program: every node adds to one shared number, under a lock and then with
# pm_atomic_add. Run it with tests/multi/. It uses the test-only hook of src/lib/ until tasks
# exist (M3-7 removes it).
add_executable(counter main.c)
target_link_libraries(counter PRIVATE paramesh::api paramesh::lib)
paramesh_target_settings(counter)
