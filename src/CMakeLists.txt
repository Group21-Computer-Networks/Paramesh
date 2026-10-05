# One target per directory, in dependency order. The direction is one-way, as in docs/PLAN.md
# "Repository layout": a directory never includes headers from one that depends on it.
# Each directory owns its own CMakeLists.txt; this file only lists the fixed set of directories.

add_subdirectory(platform)
add_subdirectory(wire)
add_subdirectory(coh)
add_subdirectory(mem)
add_subdirectory(store)
add_subdirectory(net)
add_subdirectory(rt)
add_subdirectory(lib)
add_subdirectory(pmd)
add_subdirectory(tools)
