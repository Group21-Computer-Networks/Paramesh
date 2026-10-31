# Interfaces for hashing, checksum, JSON, HTTP server, logging, clock and configuration,
# each with its adapter, and the factory. Part of libparamesh: exceptions off.
paramesh_module(platform NO_EXCEPTIONS DEPS paramesh::json paramesh::httplib paramesh::picosha2)
# The two vendored libraries that throw are told the build has no exceptions.
target_compile_definitions(paramesh_platform PRIVATE JSON_NOEXCEPTION CPPHTTPLIB_NO_EXCEPTIONS)
# Every job process hashes its whole executable before it listens: unoptimised, about 1 s of CPU
# for 19 MB, a delay that counts against job.start_timeout (M1-7 fix). Built -O2 in every build.
set_source_files_properties(adapters/sha256_picosha2.cpp PROPERTIES COMPILE_OPTIONS
    "-O2;$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")
