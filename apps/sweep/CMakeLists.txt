# Sweep: rounds over one shared array, each chunk its own slice; the workload of
# X-locality-sched. It uses paramesh.h only, with no test hook.
add_executable(sweep main.c)
target_link_libraries(sweep PRIVATE paramesh::api paramesh::lib)
paramesh_target_settings(sweep)
