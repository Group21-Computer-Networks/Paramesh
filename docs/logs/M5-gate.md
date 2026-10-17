# M5 gate

- Milestone: M5
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
**Gate.** FR-6 by M5-4. FR-7.1: Jain's index at least 0.9. FR-7.2: the free-rider's share falls and stays above zero. FR-7.3: no change in fault latency under contention. The trust rule: the forged-totals test.
-->

## Checks

<!--
One row per acceptance check named in the gate text. Result is one of: pass, fail, pending, n/a.
Evidence: the command, the log or result file, the trace, or "seen by <who>".
-->

| Check | Setup | Result | Evidence |
| --- | --- | --- | --- |
| FR-6: each node keeps a ledger from its own records, announces raw totals, and flags disagreement | daemons on one machine | pass | M5-1 (PR #54): the ledger survives a restart and a kill during a save leaves a whole file (20 of 20 kills mid-write); M5-2 (PR #58): credits from fixed counts, balance 223 core-seconds as worked by hand, nothing taken from a received field; M5-4 (PR #60): totals on `HELLO`, `LEDGER_SYNC`, `GET /v1/ledger`, `pm status`; after a real job both daemons' `/v1/ledger` agree and each other's `LEDGER_SYNC` matches (`unit.pmd_test`) |
| FR-7.1: Jain's index at least 0.9 | three daemons on one machine | pass | `tools/bench/fairness.sh jain` (M5-5, PR #61): three jobs of `matmul 1024`, 1.368, 1.303 and 1.181 s: Jain's index 0.996 |
| FR-7.2: the free-rider's share falls and stays above zero | four daemons on one machine, `ledger.scale` 10 core-seconds | pass | `tools/bench/fairness.sh freerider`: over 10 rounds its weight at node 1 fell 0.96 to 0.60 and its share of throughput 0.21 to 0.14, never 0 (table in docs/logs/M5-5.md); unit tests of the share-out with a fake ledger (M5-3, PR #59): a free-rider at the 0.25 floor gets less but some, no job waits more than its turn |
| FR-7.3: no change in fault latency under contention | three daemons on one machine | pass | `tools/bench/fairness.sh latency` with `apps/faultlat`: 896 remote first reads each; alone p50 293.7, p90 561.1, p99 737.0 us; beside two matrix multiplies p50 225.4, p90 383.4, p99 652.7 us: no increase (the processors are shared, so the run-to-run spread is wide: another run gave 180/298/607 alone and 262/469/821 us beside) |
| The trust rule: forged totals move no weight | one daemon | pass | `unit.pmd_test`, "forged totals on HELLO are shown and flagged, and move no weight": a billion claimed chunks are shown at `/v1/ledger` and in `pm status`, flagged as disagreeing, balance 0, weight 1 (M5-4) |

## Hardware runs

<!-- Which laptops, kernel versions, the cable or switch, and anything unusual. -->
The gate text names no laptop check for M5; every check above ran on one machine (Ubuntu
24.04, `build/dev`). The same script runs across laptops by starting the daemons there.

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
