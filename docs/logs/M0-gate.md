# M0 gate

- Milestone: M0
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
**Gate.** A person approves `paramesh.h`, `PROTOCOL.md`, `STATE_MACHINES.md` and `INTERNAL_API.md`, answers every flagged point, runs `check_env.sh` on all six laptops, and sees the demo run. After this gate the four files change only with approval.
-->

## Checks

| Check | Setup | Result | Evidence |
| --- | --- | --- | --- |
| `include/paramesh.h` approved | the person | pending | draft merged in PR #2 |
| `docs/PROTOCOL.md` approved | the person | pending | draft merged in PR #3 |
| `docs/STATE_MACHINES.md` approved | the person | pending | draft merged in PR #4 |
| `docs/INTERNAL_API.md` approved | the person | pending | draft merged in PR #5 |
| Every flagged point answered | the person | pending | the list below |
| `tools/check_env.sh` passes on all six laptops | six laptops | pending | 1 of 6 so far, see Hardware runs |
| The demo is seen running | the person | pending | `./build/dev/apps/m0_demo/m0_demo`; passes locally and in Actions (PR #7) |

## Hardware runs

| Laptop | System | Kernel | `check_env.sh` | Date |
| --- | --- | --- | --- | --- |
| `ouroboros` | Ubuntu 24.04.5 | 7.0.0-38 | 13 pass, 0 fail, 1 n/a, exit 0 | 2026-10-07 |
| 2 | | | not run | |
| 3 | | | not run | |
| 4 | | | not run | |
| 5 | | | not run | |
| 6 | | | not run | |

## Answers to open points

### Already answered during M0

| Point | Answer | By, on | Recorded in |
| --- | --- | --- | --- |
| Still open #6, in-flight node states | Three stable, four in flight: `I→S`, `I→M`, `S→M`, `M→I` | Amirishetty Sai Vignesh, 2026-10-07 | `docs/STATE_MACHINES.md` 1.1 |
| Still open #17, sanitizers with the 4 GB mapping | ASan and UBSan yes; TSan cannot map the address. The address stays; region tests skip TSan | Amirishetty Sai Vignesh, 2026-10-07 | `docs/logs/M0-2.md`, `tools/check_env.sh` |
| G2, where the region size is admitted | In the launcher's `pm_init()`, before any worker starts | Amirishetty Sai Vignesh, 2026-10-06 | `docs/PROTOCOL.md` 8.1 |
| G12, check the API against the SRS | No: the plan and the HLD are the only sources | Amirishetty Sai Vignesh, 2026-10-07 | `docs/logs/M0-5.md` |
| Stale read on eviction | After `WRITEBACK` the home keeps the evictor as a reader | Amirishetty Sai Vignesh, 2026-10-07 | `docs/STATE_MACHINES.md` 2.4 |
| Order on `FETCH_INV` | The HLD's: no wait for an acknowledgement | Amirishetty Sai Vignesh, 2026-10-07 | `docs/STATE_MACHINES.md` 1.4 |
| Hold window | Only writers wait | Amirishetty Sai Vignesh, 2026-10-07 | `docs/STATE_MACHINES.md` 2.6 |
| Shared types | In `src/platform/` | Amirishetty Sai Vignesh, 2026-10-07 | `docs/INTERNAL_API.md` 2 |
| Interface headers for `lib`, `pmd`, `tools` | None | Amirishetty Sai Vignesh, 2026-10-07 | `docs/INTERNAL_API.md` 9 |

Two of these change text only the person edits: M4-4 in `docs/PLAN.md` ("refused before any
process starts" becomes "before any worker starts"), and HLD home rule 5.

### To answer at the gate: these need a decision

Each has the draft's answer; "as drafted" accepts it. Alternatives are in the log named.

| Point | Question | As drafted | Answer |
| --- | --- | --- | --- |
| Still open #12 | Data affinity by "cached at", or by home only? | Home only, as M3-3 is worded | |
| Still open #14 | Credit for work taken from me = my own measured CPU time on the peer's chunks? | Yes, as the plan says | |
| Still open #15 | Split of `--cap-ram`; is the spill budget a flag? | 25% cache / 75% home store; spill equals `--cap-ram`, a configuration key, not a flag | |
| G2 (M0-4) | Keep `threads_per_node` now that `pmd` sets the quota? | Kept, as an upper bound only | |
| G4 (M0-4) | Add `pm_parallel_for_data` so chunks can be page-aligned and placed by home? | Added | |
| G5, Still open #13 (M0-4) | Unit of work in a chunk for credit | Number of indexes; no API for it | |
| G7 (M0-4) | A barrier API: `pm_barrier_create`, `pm_barrier_wait`? | Added | |
| G8 (M0-4) | `pm_touch` takes read or write access and returns `int`? | Yes | |
| G13 (M0-4) | A task holding a lock when its node leaves is run again: what of the lock? | Not settled: nothing drafted | |
| P3, Still open #4 (M0-5) | `TASK_DONE` carries CPU time, though M5-2's "median over workers" then feeds a credit from a reported value | Kept | |
| P6, Still open #8 (M0-5) | Job ID | Launcher's `pmd`: node ID in the high 16 bits, a saved counter in the low 16 | |
| P7, Still open #9 (M0-5) | Discovery interface | Every multicast-capable IPv4 interface; `--iface` restricts | |
| P8, Still open #10 (M0-5) | Fewer than N nodes accept | Run on those that did if capacity still passes | |
| P9, Still open #11 (M0-5) | Local `pmd` dies mid-job | The job ends with `PMD_LOST` | |
| P11 (M0-5) | Leave: new `SEG_MAP` before migration, not after as in the HLD | Before | |
| P12 (M0-5) | Requests outside the 2 s reply timer; new key `job.start_timeout` = 10 s | As listed in section 11 | |
| P16 (M0-5) | Admission sums values received from peers, against the Trust rule | Rule read as covering ledger and weights | |
| S2 (M0-6) | M1-3 lists only the read path, but M1 needs a node to write | Write rows marked M1 | |
| S6 (M0-6) | AT-1 must report "the two tasks"; page requests carry no task | Report the two nodes | |
| I2 (M0-7) | `mem`, `store`, `net`, `rt` as abstract classes, not the HLD's free functions | Abstract classes | |
| I5 (M0-7) | `rt` includes `paramesh.h`, beyond the layout's `wire` and `net` | Yes | |

### To answer at the gate: form, proposed for approval as drafted

M0-4 (`docs/logs/M0-4.md`): G1 `pm_init` signature; G3 task function and `PM_TASK`; G6
`pm_lock_t` as a 64-bit ID in a struct; G9 error codes and `pm_strerror`; G10 `pm_atomic_add`
aborts on a bad address; G11 `pm_malloc` alignment, no `pm_free`.

M0-5 (`docs/logs/M0-5.md`): P1 payloads, opcode values, `SPAWN_DECLINE`, `SEG_MAP_ACK`; P2
`HELLO` contents (Still open #3); P4 the local-link messages (Still open #5); P5 copyset slots
(Still open #7); P10 start sequence; P13 `SEG_MAP` carries the home table; P14 1 MiB payload
limit; P15 status codes; P17 environment variable names; P18 a worker reports a fatal
condition with `JOB_END`.

M0-6 (`docs/logs/M0-6.md`): S1 one `NEED_W` event, wake on stale faults, Rule 0; S3 a late
`WRITEBACK` is acknowledged and dropped; S4 `ATOMIC_OP` invalidates every read copy; S5
`W_LOAD`; S7 segment states; S8 an impossible pair aborts the job; S9 a leaving node stops
serving faults.

M0-7 (`docs/logs/M0-7.md`): I1 one namespace; I3 `home_step` takes the directory; I4
`coh_step` by value; I6 placement in `store`; I7 the transport's extra duties; I8 payload
structs left to M1-1; I9 the nine `Errc` codes; I10 follows #12.

Still open #16 (starting balance of a new peer) is already in `docs/TODO.md`.

## Follow-ups

- The person edits `docs/PLAN.md` (M4-4 wording) and decides whether the HLD is brought in
  line (home rule 5, leave order, FETCH_INV wording in M2-2).
- After approval, the `[GATE ...]` and `DRAFT` marks come out of the four files in one change.
