# Task registry, queue, worker threads, locks, barriers, atomics, allocator.
# The one file that calls application task bodies is compiled with exceptions on (M3-1).
paramesh_module(rt NO_EXCEPTIONS DEPS paramesh::wire paramesh::net)
