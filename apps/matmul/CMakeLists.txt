# Matrix multiply: the M3 workload. It uses paramesh.h only, with no test hook.
add_executable(matmul main.c)
target_link_libraries(matmul PRIVATE paramesh::api paramesh::lib)
paramesh_target_settings(matmul)
