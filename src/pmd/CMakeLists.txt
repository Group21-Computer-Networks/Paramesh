# Daemon: discovery, control channel, admission, spawn, ledger, slot allocation, status API,
# local socket. Exceptions allowed, caught at every thread entry point. The pmd executable is
# added here by the task that first gives it a main().
paramesh_module(pmd DEPS paramesh::wire paramesh::net paramesh::platform)

# The daemon itself. Its main() stays out of the library, which the unit tests also link.
get_target_property(pmd_sources paramesh_pmd SOURCES)
list(FILTER pmd_sources EXCLUDE REGEX "/main\\.cpp$")
set_target_properties(paramesh_pmd PROPERTIES SOURCES "${pmd_sources}")
add_executable(pmd main.cpp)
target_link_libraries(pmd PRIVATE paramesh::pmd)
paramesh_target_settings(pmd)
