# M3 gate

- Milestone: M3
- Status: closed
- Closed by: Amirishetty Sai Vignesh
- Closed on: 2026-10-10

<!--
Only the person closes a gate. An agent may prepare this file and fill in checks it ran,
but sets "Status: closed", "Closed by" and "Closed on" only when the person says the gate is
closed, using the name and date they give.
plan.py check refuses a closed gate while any check is fail or pending or any task in this
milestone is not done.

The plan's gate text:
**Gate.** FR-3.1 to FR-3.3 by M3-1, M3-2 and M3-4. FR-9.1: at least two nodes run chunks of one job, shown by the per-node chunk counts. FR-9.2: matrix multiply at n = 4096 is faster on the two cabled laptops than on one. The SRS target of 2 to 2.5 times on three laptops is measured when the switch arrives.
-->

## Checks

<!--
One row per acceptance check named in the gate text. Result is one of: pass, fail, pending, n/a.
Evidence: the command, the log or result file, the trace, or "seen by <who>".
-->

| Check | Setup | Result | Evidence |
| --- | --- | --- | --- |
| FR-3.1: tasks are found by the hash of their names | unit tests | pass | `unit.paramesh_h_test` (a task from a C file and one from a C++ file, found by hash); `unit.rt_test` (the hash against published FNV-1a values); `unit.lib_test` (`pm_init` stops on two names with one ID, naming both). M3-1 |
| FR-3.2: `pm_parallel_for` | three processes on localhost | pass | `unit.lib_test`, "every index of a range is run exactly once across three processes": 76,800 indexes, each run once, every node ran some. M3-2 |
| FR-3.3: the same binary on every node, checked by hash | a daemon and a peer | pass | `unit.pmd_test`, "a binary with a different hash is refused with "binary mismatch"": `SPAWN_DECLINE`, status `BINARY_MISMATCH`, no process started. M3-4 |
| FR-9.1: at least two nodes run chunks of one job | three simulated nodes, through `pmrun` | pass | matrix multiply, n = 4096: `chunks by node: 1:28 2:11 3:10`; runs below |
| FR-9.2 on one machine: two simulated nodes are faster than one (not the gate's own measurement) | one, two and three simulated nodes, four threads each | pass | n = 4096, median of three runs: one node 8.22 s, two nodes 6.82 s (1.21 times faster), three nodes 7.72 s (1.06 times); runs below |
| FR-9.2: n = 4096 is faster on two cabled laptops than on one | two cabled laptops | n/a | waived by Amirishetty Sai Vignesh in chat, 2026-10-10 ("Waive for now"); the run is in `docs/TODO.md`, commands under "Hardware runs" |

## Hardware runs

<!-- Which laptops, kernel versions, the cable or switch, and anything unusual. -->
Waived at this gate; not run. On each laptop, a Release build at the same path, and a daemon. With A at
10.0.0.1 and B at 10.0.0.2:

```bash
# once, on both
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DPARAMESH_WERROR=OFF
cmake --build build/bench --target pmd pmrun matmul

# on B
build/bench/src/pmd/pmd --node-id 2
# on A, in one terminal
build/bench/src/pmd/pmd --node-id 1 --peers 10.0.0.2
# on A, in another: one laptop, then two
build/bench/src/tools/pmrun -n 1 build/bench/apps/matmul/matmul 4096
build/bench/src/tools/pmrun -n 2 build/bench/apps/matmul/matmul 4096
```

Each run prints `matmul: job, n 4096: <seconds> s, checksum 412316811270` and the chunks by
node. The check passes if the second time is below the first. Five runs of each and the
median would match the benchmark script.

| Laptop | System | Kernel | Result | Date |
| --- | --- | --- | --- | --- |
| A | | | not run | |
| B | | | not run | |

## Runs

On `main` at 580d898, host `ouroboros` (Ubuntu 24.04.5, kernel 7.0.0-38, 16 processors),
2026-10-10. A Release build; every node is a `pmd` with `--cap-cores 4` on this machine, and
the job is started by `pmrun` (`tests/multi/run_pmrun.sh -c 4 -b build/bench`).

| Nodes | Threads | Run 1 | Run 2 | Run 3 | Median | Against one node | Chunks by node |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 4 | 8.060 s | 8.222 s | 8.280 s | 8.222 s | 1.00 | 1:16 |
| 2 | 2 x 4 | 6.952 s | 6.824 s | 6.766 s | 6.824 s | 1.21 | 1:20 2:12 |
| 3 | 3 x 4 | 9.468 s | 7.721 s | 7.465 s | 7.721 s | 1.06 | 1:28 2:11 3:10 |

Every run gave checksum 412316811270, the plain program's. `ctest --preset dev`: 16 of 16
tests pass.

The gain is small, and smaller with three nodes than with two. The launcher fills A and B,
so it holds every page of them, and each worker fetches all of B (32,768 pages) one page for
each fault, through the page's home. The work a worker saves is partly spent waiting for
pages. `docs/TODO.md` has the row.

## What M3 added beyond its seven cards, and what it left

- **Files outside the cards**, each approved by the person and recorded in the task's log:
  `src/lib/` for M3-1 to M3-3; payload encoders in `src/wire/`; `--node-id`,
  `--control-port` and `--state-dir` for the daemon; `frame_io` in `src/net/`;
  `apps/readshare` on tasks; `tests/multi/run_pmrun.sh`.
- **The frozen `rt/runtime.h` was not built as written.** Its `Runtime` class and `rt_open()`
  do not exist: the runtime is three pieces owned by `src/lib/job.cpp` (`Sync`, `Tasks`, the
  registry), and allocation and `pm_atomic_add` are in `job.cpp`. The person chose on
  2026-10-09 to decide at this gate.
- **A key the plan does not list:** `PARAMESH_CFG_TASK_AFFINITY`, for M3-3's comparison.

## Answers to open points

<!--
Each "Still open" point or flagged question the person answered at this gate:
the point's number, the answer, and where it is now recorded (the frozen file, AGENTS.md, docs/TODO.md).
"None" if none.
-->
Asked in chat on 2026-10-10 and answered by Amirishetty Sai Vignesh:

| Point | Answer | Recorded in |
| --- | --- | --- |
| The frozen `src/rt/runtime.h` describes a `Runtime` class that was never built | "Trim it to what exists": one small extra task, X-m3-runtime-api, edits `rt/runtime.h` and `docs/INTERNAL_API.md` to describe the code as it is. No behaviour changes. This is the approval for that change to two frozen files. | `docs/TODO.md` until the task is done; then its log |
| `PARAMESH_CFG_TASK_AFFINITY`, a key the plan's Tunables table does not have | Kept, as `task.affinity`, on by default | `docs/TODO.md`: the plan's table is the person's to edit |
| FR-9.2 on two cabled laptops | Waived for now | `docs/TODO.md` |

**How the gate was closed.** To "Do you close the M3 gate?" the answer was "Yes, close it".

## Follow-ups

<!-- Items for docs/TODO.md or for the next milestone that came out of the review. "None" if none. -->
- `docs/TODO.md` holds what M3 left: the speed-up; the Release build that fails with warnings
  as errors; tunables that do not come from the daemon; what the minimal daemon leaves out;
  G13 (a chunk run again while its thread holds a lock).
- Not tested anywhere yet: `src/lib/job.cpp` under ThreadSanitizer; a job abort caused by a
  reply timeout; two job processes with different binaries meeting on the data plane (the
  daemon's check is tested; the check in `JOIN_JOB` is not).
- `unit.pmd_test` takes 33 to 46 s plain and up to 139 s under a sanitizer.
