# Task registry, queue, worker threads, locks, barriers, atomics, allocator.
# The one file that calls application task bodies is compiled with exceptions on (M3-1).
# rt/runtime.h uses the handle and task types of paramesh.h, so rt also needs the public header.
paramesh_module(rt NO_EXCEPTIONS DEPS paramesh::api paramesh::wire paramesh::net)

# A task body may throw; the caller catches everything and ends the job (engineering rules).
set_source_files_properties(task_call.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
