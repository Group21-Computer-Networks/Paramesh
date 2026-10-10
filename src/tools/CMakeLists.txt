# pm and pmrun. Exceptions allowed, caught at every thread entry point. The executables are
# added here by the tasks that first give them a main().
paramesh_module(tools DEPS paramesh::platform paramesh::wire paramesh::net)

# Each tool's main() is in <tool>_main.cpp and stays out of the library, which the unit tests
# also link.
get_target_property(tools_sources paramesh_tools SOURCES)
list(FILTER tools_sources EXCLUDE REGEX "_main\\.cpp$")
set_target_properties(paramesh_tools PROPERTIES SOURCES "${tools_sources}")
add_executable(pmrun pmrun_main.cpp)
target_link_libraries(pmrun PRIVATE paramesh::tools)
paramesh_target_settings(pmrun)
