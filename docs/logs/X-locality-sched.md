# X-locality-sched: Locality- and capacity-aware chunk scheduling

- Task: X-locality-sched
- Milestone: none
- Stream: -
- Status: done
- Branch: x-locality-sched
- Started: 2026-10-22
- Finished: 2026-10-22

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-21 ("lets finish off 7, 3, 1"):
item 3 of the group's feature list, scheduling that weighs data locality, processing capacity
and fairness, compared against the existing scheduler.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-22 — session 1

- Read `src/rt/tasks.{h,cpp}`: M3-3's affinity gives an asking node the first chunk whose first
  page it is home for, else the oldest. Fairness between jobs is `pmd`'s slot share-out
  (M5-3), which this task leaves as it is. `TasksConfig` and `Tasks` are not in INTERNAL_API.md:
  no frozen file changes.
- `task.affinity` 2 (`src/rt/tasks.cpp`): each chunk counts its pages per home when queued;
  an asking node gets the chunk with most pages homed on it. With none, it takes the last
  chunk of the home whose queued pages, over the indexes that home has run, are most: the home
  that would finish last at the rate it has run. 1 is M3-3 as it was (the default), 0 FIFO.
  `src/lib/job.cpp` reads the value (it read only zero or not).
- `unit.rt_test`: the M3-3 test also runs affinity 2 (nodes ask 3, 2, 3, 3, 2, 2: "536241"
  against "536142"), and a case where the rate decides (node 1 has run a chunk, node 2 none;
  node 3 takes node 2's).
- matmul 512 on three nodes in namespaces, affinity 0/1/2: 10.3/11.5/11.5 s, then
  11.1/10.7/10.7 s; 265,856 faults and 136,077 pages in for all three. Every chunk reads all
  of B, so placement does not change matmul's traffic: not a workload for this.
- `apps/sweep`: rounds over one shared array, each index written by a task (v[i] = 3i +
  round, the same however often a chunk runs), untouched before the first round so each page
  starts at its home. `sweep 8 3` on one node: checksum 4,096 (2,048 pages x round 2).
- `tools/bench/fairness.sh jain` (three matmul 1024 jobs at once): Jain's index 0.998 with
  affinity 1 and 0.998 with affinity 2. The share-out between jobs is unchanged.

### 2026-10-22 — the comparison; affinity 2 the default

- Rebased on `main` after X-cache-policy (PR #74). `tools/bench/evaluate.sh`: `run_job` runs
  `$APP` when set, else matmul; a `sched` experiment runs `sweep 64 4` with affinity 0, 1 and
  2, with no added delay and with 1 ms on every link.
- `evaluate.sh sched` (three nodes in namespaces), run s / faults / pages in / pages out:
  no delay: affinity 0 6.277 / 82,048 / 57,488 / 109,043; 1 5.402 / 82,048 / 48,048 /
  102,289; 2 3.633 / 82,048 / 36,594 / 94,169. 1 ms: 0 99.585 / 60,742 in; 1 59.303 / 47,368;
  2 37.750 / 36,594. Affinity 2 is about a third faster than M3-3's, with a quarter fewer pages
  received; the faults are the same (every page is first touched once a round).
- Made affinity 2 the default (`TasksConfig`). `ctest --preset dev`: 16 of 16.
- clang-tidy: a widening multiplication in `apps/sweep/main.c`; fixed, clean. clang-format:
  clean.
- Matrix: all eight builds green (below). Next: the plan check and the pull request.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/rt/tasks.h` — `affinity` 0/1/2, a chunk's pages per home, the rate each node has run
- `src/rt/tasks.cpp` — the locality and capacity choice
- `src/lib/job.cpp` — reads `task.affinity` as a number
- `tests/unit/rt_test.cpp` — affinity 2 cases
- `apps/sweep/CMakeLists.txt` — the sweep workload
- `apps/sweep/main.c` — the same
- `tools/bench/evaluate.sh` — `$APP`, the `sched` experiment
- `docs/TODO.md` — the `task.affinity` row
- `docs/logs/X-locality-sched.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- Capacity is the indexes a node has run so far in the job, against the pages waiting at its
  home: no clock in `src/rt/`, and every node starts with the job, so what it has run tracks
  its rate. A node that joins mid-run starts at 0 and looks slow until it catches up.
- A node with no local work takes the last chunk of the slowest home, so the home itself
  keeps taking from the front.
- Affinity 2 the default: measured a third faster than 1 on the locality workload and no
  slower on matmul; 1 and 0 stay for comparison.
- The comparison workload is a new app, `apps/sweep`: matmul's traffic does not depend on
  placement.

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
- The work itself: Amirishetty Sai Vignesh, 2026-10-21 ("lets finish off 7, 3, 1"). No frozen
  file changes.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
None.

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
No card, so no Done-when check of its own. On this machine (Ubuntu 24.04, gcc 13, clang 18):

- Matrix, `build/nc-<compiler>-<sanitizer>`, Debug, `ctest -j2`: gcc none 16/16, address
  16/16, undefined 16/16, thread 15/15; clang none 16/16, address 16/16, undefined 16/16,
  thread 15/15 (`app.m0_demo` is not registered under ThreadSanitizer, as on `main`).
- The comparison against the existing scheduler, `tools/bench/evaluate.sh sched`, and the
  fairness run, `tools/bench/fairness.sh jain`, in the sessions above.
- `unit.rt_test`'s affinity 2 cases fail on the old code: it had no mode 2 and handed out
  "536142", and it took no account of what a node had run.
- `cmake --build --preset tidy`: no finding. clang-format `--dry-run --Werror`: clean.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- The values of `task.affinity` in the plan's Tunables table (the existing TODO row, updated).
