# Wires the directories above into libparamesh and implements include/paramesh.h.
paramesh_module(lib NO_EXCEPTIONS DEPS
    paramesh::api paramesh::platform paramesh::wire paramesh::coh paramesh::mem
    paramesh::store paramesh::net paramesh::rt)
