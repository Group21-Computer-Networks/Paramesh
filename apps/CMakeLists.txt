# Example programs, one subdirectory each (counter, matmul, falseshare, m0_demo), each with its
# own CMakeLists.txt. Applications use include/paramesh.h only: link paramesh::api and
# paramesh::lib, never an internal directory's target.
paramesh_add_subdirectories()
