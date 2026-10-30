# The user guide's example (docs/API.md): built here so that it keeps compiling.
add_executable(quickstart main.c)
target_link_libraries(quickstart PRIVATE paramesh::api paramesh::lib)
paramesh_target_settings(quickstart)
