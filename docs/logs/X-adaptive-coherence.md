# X-adaptive-coherence: adaptive coherence: read-write fights held too

- Task: X-adaptive-coherence
- Milestone: none
- Stream: -
- Status: done
- Branch: x-adaptive-coherence
- Started: 2026-10-31
- Finished: 2026-10-31

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Item 2 of the course's extension list, "Implement an Adaptive Memory Coherence Optimization Mechanism": identify frequently shared and modified pages, adjust ownership according to observed read-write contention, reduce repeated invalidations and transfers while keeping consistency, and evaluate coherence traffic, transfer frequency and performance. Approved by the person in chat on 2026-10-31.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-31 — session 1

- AT-1's hold window counted only writer-to-writer transfers, and any read ended it. So a page
  with one writer and one reader on two nodes was never seen as contended, and under writers'
  fights a reader kept ending the window.
- `src/coh/home_machine.cpp`: with `thrash.hold_readers`, a `READ_REQ` that takes the page from
  its owner by `FETCH` counts as a transfer. Over the threshold the page is in a read fight: the
  home reports it, the next write grant opens or doubles the window, even to the same writer,
  and reads are held back with the writers. Bounded by `thrash.hold_max`, as writes are.
- `docs/STATE_MACHINES.md` 2.6 changed (see Approvals). `src/lib/job.cpp` reads
  `PARAMESH_CFG_THRASH_HOLD_READERS`; default on.
- `apps/falseshare`: third argument `read`, one thread a node. `tools/bench/evaluate.sh`:
  experiment `coherence`.
- Tests: three cases in `coh_home_test.cpp`; one in `lib_test.cpp` with one writer and one reader
  for 300 ms on two nodes. Earlier versions of that test did not always fight: short tasks did
  not overlap, or one node took both (`task.prefetch`); it now sets `PARAMESH_CFG_TASK_PREFETCH=0`
  and both tasks wait for each other. 10/10 runs: 40-120 transfers held, 2,646-2,920 not.
- `tools/bench/evaluate.sh coherence`, local, three namespaces on one machine, run seconds and
  pages received, three runs each:

  | Workload | Hold off | Writers only | Readers too |
  | --- | --- | --- | --- |
  | falseshare, read fight | 9.4-9.9 s, ~100,000 | 15.9-43.3 s, 107,000-267,000 | 0.45-0.54 s, 93-103 |
  | falseshare, write fight | 3.6-8.0 s, 19,000-47,000 | 0.55-0.94 s, 150-290 | 0.92-1.03 s, 240-330 |
  | matmul 256 | 0.36-0.38 s | 0.39-0.41 s | 0.37-0.39 s |
  | sweep 16 MB | 0.81-0.96 s | 0.81-0.94 s | 0.93-1.14 s |

  The write-fight rows come from the first run of the experiment, before one thread a node in
  read mode, which does not affect them. Writers-only made the read fight worse than no window.
  Sweep was slower in 2 of 3 runs with readers held, about 5% more pages; three runs do not
  settle that.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/coh/home_machine.cpp` — read fights in the hold window
- `src/coh/home_machine.h` — `HomeConfig::hold_readers`
- `src/lib/job.cpp` — `thrash.hold_readers`
- `docs/STATE_MACHINES.md` — section 2.6
- `docs/INTERNAL_API.md` — `HomeConfig`
- `docs/TODO.md` — the new key
- `apps/falseshare/main.c` — read mode
- `tools/bench/evaluate.sh` — experiment `coherence`
- `tests/unit/coh_home_test.cpp` — read fights at the home
- `tests/unit/lib_test.cpp` — a read fight on three processes

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- Reads share the transfer counter and `thrash.threshold` rather than having their own: one
  count, no new number.
- Default on: the read fight is about 20 times faster than with no window, and the cost seen
  elsewhere is small. `PARAMESH_CFG_THRASH_HOLD_READERS=0` restores AT-1's behaviour exactly.
- Ownership, not task placement: no new message is needed.

## Questions and blockers

<!--
Each item starts with [open] or [answered YYYY-MM-DD by <who>], followed by the question and,
once answered, the answer. M0 drafting tasks may also use [for gate] for a Still-open point
drafted for the person to decide at the M0 gate. Write "None" if there are none.
-->
None

## Approvals

<!--
Approvals from the person for anything the rules reserve to them, such as a change to a frozen
file (include/paramesh.h, docs/PROTOCOL.md, docs/STATE_MACHINES.md, docs/INTERNAL_API.md) that
this card does not list. Name the file, who approved, and the date. "None" if there are none.
-->
- `docs/STATE_MACHINES.md` 2.6, the [DECIDED] rule "Only writers wait": changed so readers wait
  in a read fight, bounded by the window. Approved by the person in chat, 2026-10-31.
- `docs/INTERNAL_API.md`: one line, `hold_readers` in `HomeConfig`, to match the code. Approved by
  the person in chat, 2026-10-31.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
None

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
No card. The comparison the item asks for is in the session above (coherence traffic and
transfers as pages received, performance as run seconds; three namespaces on one machine, not
real hardware). Local, 2026-10-31: clang-format clean; `tidy` preset exit 0; `ctest` with `dev`,
`asan` and `ubsan` 16/16 each, `tsan` 15/15. The `lib_test` read-fight case skips itself under
TSan, as every three-process case does; `coh_home_test` runs there.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- `thrash.hold_readers` is not in the plan's Tunables table; a program that polls a page another
  node writes can wait up to `thrash.hold_max` per read. Both in docs/TODO.md.
