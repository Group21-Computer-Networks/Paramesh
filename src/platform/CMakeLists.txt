# Interfaces for hashing, checksum, JSON, HTTP server, logging, clock and configuration,
# each with its adapter, and the factory. Part of libparamesh: exceptions off.
paramesh_module(platform NO_EXCEPTIONS DEPS paramesh::json paramesh::httplib paramesh::picosha2)
# The two vendored libraries that throw are told the build has no exceptions.
target_compile_definitions(paramesh_platform PRIVATE JSON_NOEXCEPTION CPPHTTPLIB_NO_EXCEPTIONS)
