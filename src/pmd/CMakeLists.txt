# Daemon: discovery, control channel, admission, spawn, ledger, slot allocation, status API,
# local socket. Exceptions allowed, caught at every thread entry point. The pmd executable is
# added here by the task that first gives it a main().
paramesh_module(pmd DEPS paramesh::wire paramesh::net paramesh::platform)
