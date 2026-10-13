# Home store, CLOCK eviction, spill file, local-cache accounting.
# The spill file is read and written by a thread of the store's own.
find_package(Threads REQUIRED)
paramesh_module(store NO_EXCEPTIONS DEPS paramesh::platform Threads::Threads)
