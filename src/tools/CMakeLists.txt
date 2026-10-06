# pm and pmrun. Exceptions allowed, caught at every thread entry point. The executables are
# added here by the tasks that first give them a main().
paramesh_module(tools DEPS paramesh::platform paramesh::wire)
