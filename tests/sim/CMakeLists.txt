# Fake message bus, fuzz harness and invariant checker, over coh and wire only.
# M1-5 adds the sources and the test executable.
paramesh_module(sim DEPS paramesh::coh paramesh::wire)
