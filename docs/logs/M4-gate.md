# M4 gate

- Milestone: M4
- Status: open
- Closed by:
- Closed on:

<!--
Only the person closes a gate. An agent may prepare this file and fill in checks it ran,
but sets "Status: closed", "Closed by" and "Closed on" only when the person says the gate is
closed, using the name and date they give.
plan.py check refuses a closed gate while any check is fail or pending or any task in this
milestone is not done.

The plan's gate text:
**Gate.** FR-1.1, FR-1a, FR-2.1 to FR-2.3, FR-8.1, FR-8.2 and FR-10.1 by the checks above, on three simulated nodes and the two cabled laptops. Pulling the cable out mid-run is the hardware test for FR-10.1. FR-1.2 by M4-7.
-->

## Checks

<!--
One row per acceptance check named in the gate text. Result is one of: pass, fail, pending, n/a.
Evidence: the command, the log or result file, the trace, or "seen by <who>".
-->

| Check | Setup | Result | Evidence |
| --- | --- | --- | --- |
| FR-1.1: a node joins and appears within 2 s | three daemons on one machine | pass | `unit.pmd_test`, "three daemons list each other within 2 s, by multicast": listed each other after 1,006 to 1,020 ms (M4-7's local runs, 2026-10-15); "with multicast off, the seed list is enough". M4-1 |
| FR-1.2: a node joins a running job | three simulated nodes, then a fourth | pass | `unit.pmd_test`, "after a worker leaves, a fourth node joins the running job and runs chunks": node 4 ran 12 to 18 chunks, every result right, `member_joined` (M4-7, PR #50); `tests/multi/run_m4.sh join`, six of six settings (M4-8) |
| FR-1a: caps are never exceeded | three simulated nodes, `--cap-ram` | pass | `unit.pmd_test`, "with --cap-ram, no process maps more of the region than its cache's share": at most 4,096 KB, the cache's share (M4-2, PR #42); the thread quota by M3-7 |
| FR-2.1 to FR-2.3: graceful leave | three simulated nodes | pass | `unit.pmd_test`, "a worker leaves during matrix multiply: the job finishes with the single-machine result" (five of five, checksum equal to `matmul --plain`); "on a launcher pm leave is refused while its job runs, and pm leave --force ends it"; "Ctrl+C on pmd is a graceful leave"; `unit.lib_test`, the unfinished chunk goes back after 10 s (10.0 to 11.0 s) (M4-5, PRs #46 to #48); `run_m4.sh leave`, six of six settings (M4-8) |
| FR-8.1: spill to disk | three simulated nodes, caps of 8 MB, 48 MB region | pass | `unit.pmd_test`, "a job whose region is twice the RAM caps completes with the right answer": largest spill file 14,340 KB (M4-3, PR #43); `run_m4.sh spill`, six of six settings (M4-8) |
| FR-8.2: admission headroom | one and three daemons | pass | `unit.pmd_test`, "one node alone refuses a region past 90% of its own RAM cap and spill budget" and "a job that fits is admitted"; `pm_init()` prints the refusal with the sizes (M4-4, PR #44) |
| FR-10.1: clean abort within 3 s | three simulated nodes | pass | `unit.pmd_test`, "killing a node mid-run ends the job everywhere within 3 s, naming the node": 0.20 s; "a node that goes silent ends the job after 3 s without a heartbeat": 2.6 to 2.7 s (M4-6, PR #45); `run_m4.sh abort`, six of six settings, 0 to 1 s |
| The above under delay and loss | network namespaces, 0, 5 and 25 ms, 0 and 2% loss | pass | `tests/multi/run_m4.sh`, 24 of 24 (M4-8, PR #51; table in docs/logs/M4-8.md) |
| FR-1.1, FR-1a, FR-2, FR-8.1, FR-8.2 on hardware | two cabled laptops | pending | not run; commands below |
| FR-10.1 on hardware: pull the cable mid-run | two cabled laptops | pending | not run; commands below |

## Hardware runs

<!-- Which laptops, kernel versions, the cable or switch, and anything unusual. -->
Not run yet. On each laptop, a build at the same path and the same binary. With A at
10.0.0.1 and B at 10.0.0.2, on the cable:

- A: `build/dev/src/pmd/pmd --node-id 1 --peers 10.0.0.2`; B: `build/dev/src/pmd/pmd
  --node-id 2 --peers 10.0.0.1`. FR-1.1: `build/dev/src/tools/pm status` on A lists node 2
  within 2 s of B's daemon starting.
- FR-8.2: on A, `build/dev/src/tools/pmrun -n 2 build/dev/apps/matmul/matmul 8192` with both
  daemons started with `--cap-ram 256M`: refused, with the sizes.
- FR-1a and FR-8.1: both daemons with `--cap-ram 64M`, then on A `pmrun -n 2
  build/dev/apps/matmul/matmul 2048`: the checksum equals `matmul --plain 2048` on one
  laptop (51539578872).
- FR-2: on A `pmrun -n 2 build/dev/apps/matmul/matmul 4096 1`; while it runs, on B
  `build/dev/src/tools/pm leave`: the job ends with the right checksum and B's daemon goes.
- FR-10.1: on A `pmrun -n 2 build/dev/apps/matmul/matmul 4096 1`; pull the cable mid-run:
  "job N aborted: node 2 lost" within 3 s, and nothing of the job is left on either laptop.

## Answers to open points

<!--
Each "Still open" point or flagged question the person answered at this gate:
the point's number, the answer, and where it is now recorded (the frozen file, AGENTS.md, docs/TODO.md).
"None" if none.
-->
None yet.

## Follow-ups

<!-- Items for docs/TODO.md or for the next milestone that came out of the review. "None" if none. -->
None yet.
