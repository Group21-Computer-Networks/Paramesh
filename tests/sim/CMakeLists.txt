# The fuzz harness: the real state machines of src/coh/ on simulated nodes joined by a fake
# message bus, with the invariants of docs/STATE_MACHINES.md checked after every step.
add_executable(sim_fuzz fuzz.cpp)
target_link_libraries(sim_fuzz PRIVATE paramesh::coh)
paramesh_target_settings(sim_fuzz)

# fuzz.schedules: 2,000 on every push.
add_test(NAME sim.fuzz COMMAND ${PARAMESH_TEST_LAUNCHER} $<TARGET_FILE:sim_fuzz> --schedules 2000)
# The harness can fail, and a failing seed fails the same way again.
add_test(NAME sim.fuzz_self_test COMMAND ${PARAMESH_TEST_LAUNCHER} $<TARGET_FILE:sim_fuzz> --self-test)
