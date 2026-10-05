# Interfaces for hashing, checksum, JSON, HTTP server, logging, clock and configuration,
# each with its adapter, and the factory. Part of libparamesh: exceptions off.
paramesh_module(platform NO_EXCEPTIONS DEPS paramesh::json paramesh::httplib paramesh::picosha2)
