# Region mapping, userfaultfd, handler thread, install, zap, write-protect, wake.
# The fault-handler thread is a pthread.
find_package(Threads REQUIRED)
paramesh_module(mem NO_EXCEPTIONS DEPS paramesh::platform Threads::Threads)
