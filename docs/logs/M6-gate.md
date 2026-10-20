# M6 gate

- Milestone: M6
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
**Gate.** UC-5 and NFR-6 by M6-2 to M6-4. NFR-9 (a fault served by its home in under 1 ms on wired gigabit) by the histogram in M6-5. The "novelty features" the SRS mentions for M6 are not defined and are parked in `docs/TODO.md`.
-->

## Checks

<!--
One row per acceptance check named in the gate text. Result is one of: pass, fail, pending, n/a.
Evidence: the command, the log or result file, the trace, or "seen by <who>".
-->

| Check | Setup | Result | Evidence |
| --- | --- | --- | --- |
| UC-5 teaching testbed: a run can be watched live and its frames read | three daemons on one machine; three nodes in network namespaces | pass | M6-2 (PR #66): `pm top` redraws once a second and restores the terminal on Ctrl+C (`unit.pmd_test`, 3 of 3); M6-3 (PR #67): the dashboard at `127.0.0.1:7070` showed the same numbers as `pm top` at the same second, and the migration bar (0 of 3 to 2 of 3), seen in the built-in browser; M6-4 (PR #68): `tools/dissector/capture.sh` decoded 1,910 ParaMesh frames of a three-node run with TShark 4.2.2, 0 malformed |
| NFR-6 observability: status API, live rates, per-job pages, event stream | daemons on one machine | pass | M6-1 (PR #65): `/v1/jobs`, `/v1/jobs/{id}/pages`, `/v1/events` return the JSON of docs/STATUS_API.md and a change shows within 1 s (`unit.pmd_test`, 3 of 3); M6-2 to M6-4 above read them |
| NFR-9: a fault served by its home in under 1 ms | three nodes in network namespaces, no impairment | pass | `tools/bench/evaluate.sh histogram` (M6-5, PR #69): 856 faults of `matmul 256`; 854 under 512 us, 2 in 512-1024 us, none above 1 ms. No wire: veth on one machine |
| NFR-9 on wired gigabit | two cabled laptops | pending | Not run: no laptops here. `tools/bench/evaluate.sh` runs there with the daemons started on each laptop (docs/TODO.md) |
| Impairment matrix finishes without abort | three nodes in network namespaces | pass | After X-retry-dropped (PR #70): all 44 settings of delay, jitter and loss finished, 0 aborts (before it, one impossible-event abort in 44 at 50 ms / 5% loss) |

## Hardware runs

<!-- Which laptops, kernel versions, the cable or switch, and anything unusual. -->
None yet. Every check above ran on one machine (Ubuntu 24.04, kernel 7.0, `build/dev`); the
NFR-9 row on wired gigabit is pending until the laptops are cabled, as the hardware checks of
the M4 gate (waived, docs/TODO.md).

## Answers to open points

<!--
Each "Still open" point or flagged question the person answered at this gate:
the point's number, the answer, and where it is now recorded (the frozen file, AGENTS.md, docs/TODO.md).
"None" if none.
-->
None.

## Follow-ups

<!-- Items for docs/TODO.md or for the next milestone that came out of the review. "None" if none. -->
- The "novelty features" of M6 stay parked in docs/TODO.md, as the gate text says.
- The NFR-9 laptop run, if the person does not waive it.
