# M5-5: how long a page fault takes to fetch a page from another node. It uses paramesh.h only.
add_executable(faultlat main.c)
target_link_libraries(faultlat PRIVATE paramesh::api paramesh::lib)
paramesh_target_settings(faultlat)
