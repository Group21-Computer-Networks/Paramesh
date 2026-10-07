# The M1 test program: the launcher writes, every worker reads. Run it with tests/multi/.
# It uses the test-only hook of src/lib/ until tasks exist (M3-7 removes it).
add_executable(readshare main.c)
target_link_libraries(readshare PRIVATE paramesh::api paramesh::lib)
paramesh_target_settings(readshare)
