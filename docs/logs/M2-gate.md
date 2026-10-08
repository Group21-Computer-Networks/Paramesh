# M2 gate

- Milestone: M2
- Status: closed
- Closed by: Amirishetty Sai Vignesh
- Closed on: 2026-10-09

<!--
Only the person closes a gate. An agent may prepare this file and fill in checks it ran,
but sets "Status: closed", "Closed by" and "Closed on" only when the person says the gate is
closed, using the name and date they give.
plan.py check refuses a closed gate while any check is fail or pending or any task in this
milestone is not done.

The plan's gate text:
**Gate.** FR-5.1 and FR-5.2 by M2-6. FR-5.3 and UC-2 by M2-7, on three simulated nodes and on the two cabled laptops.
-->

## Checks

<!--
One row per acceptance check named in the gate text. Result is one of: pass, fail, pending, n/a.
Evidence: the command, the log or result file, the trace, or "seen by <who>".
-->

| Check | Setup | Result | Evidence |
| --- | --- | --- | --- |
| FR-5.1, FR-5.2: one writer or many readers, by M2-6 | three simulated nodes on the fake bus | pass | `build/dev/tests/sim/sim_fuzz --schedules 100000` on `main` (3a9aaf2), 2026-10-09: `100000 schedules passed`, 47 s. The six invariants of `docs/STATE_MACHINES.md` section 4 are checked after every step. Five planted bugs were each caught: `docs/logs/M2-6.md` |
| FR-5.1, FR-5.2 in real processes | three processes on localhost | pass | `unit.lib_test`, "three processes write one shared page and lose no update, with and without a lock", green in Actions on PR #26 and since |
| FR-5.3: locks | three simulated nodes | pass | `tests/multi/counter.sh` on `main`, 2026-10-09: `3000 with a lock` of 3 x 1,000 on localhost; `300` of 3 x 100 in network namespaces with 2 ms per link |
| FR-5.3: atomics | three simulated nodes | pass | the same two runs: `3000 with pm_atomic_add` and `300`; and `unit.lib_test`, "concurrent atomic adds from three processes sum exactly" |
| FR-5.3: barriers | three processes on localhost | pass | `unit.lib_test`, the same case as the lock: two workers meet at a barrier and each sees the other's mark. The counter program has no barrier |
| UC-2: the shared counter stays exact | three simulated nodes | pass | `PASS: counter, both runs`; output under "Runs" |
| FR-5.3 and UC-2 on hardware | two cabled laptops | n/a | waived by Amirishetty Sai Vignesh in chat, 2026-10-09 ("Waive for now"); the run is in `docs/TODO.md`, commands under "Hardware runs" |

## Hardware runs

<!-- Which laptops, kernel versions, the cable or switch, and anything unusual. -->
Waived at this gate; not run. There is no `pmd` before M3, so each laptop starts its process by hand. With the
same build of `counter` at the same path on both, A at 10.0.0.1 and B at 10.0.0.2:

```bash
# on B, first (the worker)
PARAMESH_ROLE=worker PARAMESH_JOB_ID=1 PARAMESH_NODE_ID=2 PARAMESH_LISTEN=10.0.0.2:47100 \
    PARAMESH_PEERS=1@10.0.0.1:47100,2@10.0.0.2:47100 build/dev/apps/counter/counter
# on A (the launcher)
PARAMESH_ROLE=launcher PARAMESH_JOB_ID=1 PARAMESH_NODE_ID=1 PARAMESH_LISTEN=10.0.0.1:47100 \
    PARAMESH_PEERS=1@10.0.0.1:47100,2@10.0.0.2:47100 build/dev/apps/counter/counter
```

A prints `counter: 2 nodes, 1000 adds each: 2000 with a lock, 2000 with pm_atomic_add: ok` and
both exit 0.

| Laptop | System | Kernel | Result | Date |
| --- | --- | --- | --- | --- |
| A | | | not run | |
| B | | | not run | |

## Runs

On `main` at 3a9aaf2, host `ouroboros` (Ubuntu 24.04.5, kernel 7.0.0-38), 2026-10-09.

```text
$ build/dev/tests/sim/sim_fuzz --schedules 100000
100000 schedules passed

$ tests/multi/counter.sh
    | counter: 3 nodes, 1000 adds each: 3000 with a lock, 3000 with pm_atomic_add: ok
PASS: counter on 3 nodes (localhost)
    | counter: 3 nodes, 100 adds each: 300 with a lock, 300 with pm_atomic_add: ok
PASS: counter on 3 nodes (network namespaces, delay 2 ms, loss 0%)
PASS: counter, both runs

$ ctest --preset dev
100% tests passed, 0 tests failed out of 15
```

## What M2 added beyond its seven cards

- **X-m2-wiring**, approved on 2026-10-09: the job process carries out the M2 page actions and
  the C API has the lock and barrier calls. No M2 card listed `src/lib/`.
- **Payloads in `src/wire/`** for the lock, barrier and atomic messages, approved the same day
  for M2-4 and M2-5, whose cards do not list that directory.
- **"`pm_wait_all` as a phase barrier"** (M2-4) was settled as a count of outstanding work on
  the launcher, which M3-2 feeds.

## Answers to open points

<!--
Each "Still open" point or flagged question the person answered at this gate:
the point's number, the answer, and where it is now recorded (the frozen file, AGENTS.md, docs/TODO.md).
"None" if none.
-->
None. No "Still open" point came up at this gate; the three questions answered during M2 are
listed under "What M2 added beyond its seven cards".

**How the gate was closed.** Asked in chat on 2026-10-09, Amirishetty Sai Vignesh waived the two-laptop
run and answered "Yes, close it".

## Follow-ups

<!-- Items for docs/TODO.md or for the next milestone that came out of the review. "None" if none. -->
- `docs/TODO.md` holds what M2 left for later tasks: `pm_wait_all` and `Runtime::atomic_add`
  (M3-2), and the abort when a chunk is run again while its thread holds a lock (G13).
- Not tested anywhere yet: `src/lib/job.cpp` under ThreadSanitizer (the region cannot be mapped
  under it); a job abort caused by a reply timeout; a refused member with a different binary
  hash. The first two were already open at the M1 gate.
- An abort that `src/rt/` asks for on the network thread exits without `JOB_END`; the other
  processes then report "node lost" (`docs/logs/X-m2-wiring.md`).
