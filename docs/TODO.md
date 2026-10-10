# Pending work

Everything deferred lives here. A pull request that leaves something undone adds it here and
lists it under "Deferred" in its log. Remove an item in the pull request that does it, and say so
in that log.

## From the plan

These are deferred by `docs/PLAN.md` itself. The "Plan reference" column names the row that defers them.

| Item | Plan reference | Revisit when |
| --- | --- | --- |
| Get a gigabit switch so three or more laptops can be tested together. | How to use this plan, "Three-laptop runs" | Now; it gates the items below that need three laptops. |
| Repeat and record the tests the SRS words as "3 laptops", including the 2 to 2.5 times matrix-multiply speed-up (FR-9.2). | How to use this plan; M3 gate; Changes #28 | A switch is available. |
| Security work beyond the ledger fix (the ledger fix itself is M5). | Decisions, "Security" | After this semester, or when the person schedules it. |
| Probation weight for a peer seen for the first time: start at balance 0 (weight 1) or lower. | Still open #16 | With the security work above. |
| Stride prefetcher after 3 sequential faults. | Decisions, "Stride prefetcher"; Changes #22 | After the M3 speed-up is measured. |
| The five demo workloads beyond counter, matrix multiply and the false-sharing demo (named in Doc 1). | Decisions, "Workloads"; Changes #29 | When the person schedules them. |
| UC-3 stencil workload and its test. | Changes #29; Requirement traceability | With the workloads above. |
| MPI comparison, and so a test for NFR-1 (transparency). | Changes #29; Requirement traceability | With the workloads above. |
| FR-10.2 crash recovery (Future in the SRS). | Requirement traceability | Not planned this semester. |
| M6 "novelty features" from the SRS. | Decisions, "M6 novelty features"; M6 gate | Once the person defines them. |

## Added by tasks

| Item | Added by | Notes |
| --- | --- | --- |
| Run `tools/check_env.sh` on the other five laptops, and have the person see `m0_demo` run. | M0 gate | Waived at the gate on 2026-10-07; only `ouroboros` has run it. Record the results in `docs/logs/M0-gate.md`. |
| Run `readshare` on two cabled laptops (the M1 gate's hardware check for FR-4.1 and FR-4.2). | M1 gate | Waived at the gate on 2026-10-08. The commands are in `docs/logs/M1-gate.md`, "Hardware runs"; record the result there. |
| Run `counter` on two cabled laptops (the M2 gate's hardware check for FR-5.3 and UC-2). | M2 gate | Waived at the gate on 2026-10-09. The commands are in `docs/logs/M2-gate.md`, "Hardware runs"; record the result there. |
| Run matrix multiply at n = 4096 on one laptop and on two cabled laptops (the M3 gate's hardware check for FR-9.2). | M3 gate | Waived at the gate on 2026-10-10. The commands are in `docs/logs/M3-gate.md`, "Hardware runs"; record the times there. |
| G13: the code that runs a chunk again must call `Sync::holds_lock` and end the job if the chunk's thread holds a lock. | M2-4 | With M3-2 (`member_leaving`) or M4-5; decided at the M0 gate. |
| The frozen `Runtime` class and `rt_open()` of `src/rt/runtime.h` are not implemented: the runtime is three pieces owned by `src/lib/job.cpp` (`Sync`, `Tasks`, the task registry), and allocation and `pm_atomic_add` live in `job.cpp` itself. | M2-5, M3-2 | Decided at the M3 gate, 2026-10-10: trim the frozen file and `docs/INTERNAL_API.md` to what exists, in the approved extra task X-m3-runtime-api. |
| The task tunables (`task.no_task_backoff`, `task.prefetch`, `task.default_grain`) are the plan's defaults in `TasksConfig` (`src/rt/tasks.h`); nothing reads them from `PARAMESH_CFG_*`, and `pmd` has no configuration file to take them from. The default grain takes every node to have this node's thread count. | M3-2, M3-7 | When `pmd` reads a configuration file (M4). The thread quota itself comes from `pmd` since M3-7. |
| `pm_parallel_for_data` checks that the array lies in the shared region, not that it lies in one allocation: the bump allocator keeps no record of allocations. | M3-2 | If a program is ever caught by it. |
| Nothing sends `NO_TASK` with reason 2, pushes a quota (`set_quota`) or puts a leaving member's chunks back (`member_leaving`); the worker side of reason 2 exists. | M3-2 | M4-5 (leave) and M3-7 (quota). |
| `task.affinity` (`PARAMESH_CFG_TASK_AFFINITY`, on by default) is a tunable the plan's Tunables table does not list. | M3-3 | Kept at the M3 gate, 2026-10-10. The person adds it to the table in `docs/PLAN.md`. |
| A job process logs at `warn` unless `PARAMESH_CFG_LOG_LEVEL` is set; the plan's default for `log.level` is `info`. | M3-3 | With M3-7, when settings come from `pmd`; the tests that read a process's stderr then need the level set. |
| A Release build fails with warnings as errors: with optimisation on, GCC 13 reports possible null dereferences (`-Wnull-dereference`) in `src/rt/tasks.cpp`, `src/lib/job.cpp` and others, which the Debug builds of CI do not show. `tools/bench/matmul.sh` builds with `PARAMESH_WERROR=OFF`. | M3-6 | Before any release build is relied on: fix or silence each, and add a Release build to CI. |
| First speed-up measurement (M3-6, one machine): matrix multiply at n = 4096 on three nodes of four threads takes 8.7 s against 8.05 s on one node. Workers fetch all of B a page at a time, and idle workers ask again only every 200 ms. | M3-6 | Before the M3 gate's FR-9.2 run on two laptops: the person decides what to do (the stride prefetcher above, filling the matrices by tasks, waking idle workers). |
| The minimal `pmd` (M3-4) commits no memory in `SPAWN_OK` (`ram_commit` and `spill_commit` are 0), reads no configuration file, does not time out a worker that never registers (it only notices one that exits), and writes its replies with blocking calls on its one thread. | M3-4 | Caps: M4-2. Configuration file and the start timeout: M3-7 or M4. The write queue: when the daemon carries more than a few frames. |
| Launching through `pmd` (M3-5): peers are asked in the order of `--peers`, not by free capacity; `pmrun` does not print how many nodes the job got when fewer than N accepted; the daemon connects to a peer with a blocking call and uses one connection for each request; `L_JOB_END` is not read (a job ends for the daemon when its launcher's connection closes). | M3-5 | Ranking by capacity: M4-1 (`HELLO`). The rest: M3-7 or M4, as they are met. |
| A job process under `pmd` (M3-7): `JOIN_JOB` announces port 0 when the system chose the port; it sends no `L_JOB_END`, `L_CHUNK` or `L_SEGMENTS`; and an `L_QUOTA` that arrives after start is ignored. | M3-7 | `L_CHUNK` and the changing quota: M5 (ledger, fair sharing). `L_SEGMENTS`, the port: M4. |
