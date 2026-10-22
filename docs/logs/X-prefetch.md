# X-prefetch: Predictive page prefetching

- Task: X-prefetch
- Milestone: none
- Stream: -
- Status: in-progress
- Branch: x-prefetch
- Started: 2026-10-22
- Finished:

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
item 1 of the group's feature list, predictive page prefetching from earlier access and fault
patterns, compared against the existing system on fault count, fault latency and run time. It
is also the stride prefetcher the plan defers (Changes #22, docs/TODO.md).

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-22 — session 1

- Started the task (item 1, the person, 2026-10-22: "leave 3 to the other session, start item
  1"). Read the read-fault path in `src/lib/job.cpp` and the node machine: `kNeedRead` on an
  Invalid page sends READ_REQ; on ReadPending a fault (read or write) just waits. So a guess
  goes in as a `kNeedRead` for a page no thread has faulted on: no protocol, state machine or
  frozen-file change.
- `src/lib/prefetch.{h,cpp}`: `Prefetcher`, pure. A stream starts on three read faults one
  stride apart (any stride, up or down) among the last 4 x streams faults, and asks for two
  strides ahead; a fault inside what it asked for (or one stride past) doubles how far it runs
  ahead, up to `prefetch.depth`. Up to `prefetch.streams` streams, least recently used dropped,
  so threads reading their own rows keep their own streams.
- `src/lib/job.cpp`: after each read fault's `kNeedRead`, the guesses that are Invalid go in as
  `kNeedRead` too (not while leaving or aborting). Write faults are not followed: a page fetched
  to read and then written costs an upgrade round trip. `prefetched` joins `job_pages`.
  `PARAMESH_CFG_PREFETCH_DEPTH` (default 0, off) and `PARAMESH_CFG_PREFETCH_STREAMS` (4).
- Tests (`lib_test.cpp`): the stride and the doubling, any stride and down to page 0, two
  interleaved streams and noise, depth 0; and the M1 program (two workers read 6 MiB in order).
  First form of that test checked for half the faults: failed, 4,599 against 4,608. The reader
  is faster than a round trip, so it still faults on most pages, but they are on their way: the
  wait is what drops (worker latency buckets 128-256 us without, mostly below 2 us with). The
  test now counts faults that waited 64 us or more: 421/378/384 with depth 16 (3,098 pages
  prefetched, against 3,072 read), 3,171/3,265/3,205 without, in three runs; it checks 4x.
- `tools/bench/evaluate.sh prefetch` (matmul 512, three nodes in namespaces, 1 thread):

  | depth | caps | run s | faults | p50 us | p99 us | pages in |
  | --- | --- | --- | --- | --- | --- | --- |
  | 0 | default | 12.109 | 265,856 | 128 | 256 | 136,077 |
  | 8 | default | 8.370 | 166,725 | 2 | 2,048 | 134,399 |
  | 32 | default | 9.260 | 149,785 | 2 | 4,096 | 142,371 |
  | 0 | 1M | 17.541 | 273,024 | 256 | 512 | 162,103 |
  | 8 | 1M | 10.396 | 171,638 | 2 | 2,048 | 138,308 |
  | 32 | 1M | 13.255 | 117,157 | 2 | 4,096 | 168,373 |

  Depth 8 is best: 31% and 41% faster, no more pages moved. Depth 32 at 1M fetches pages the
  cache gives up before they are read. The tail grows: a fault can wait behind prefetches.
- `PARAMESH_CFG_PREFETCH_DEPTH=8 ctest --preset dev`: 16 of 16 pass.
- Asked the person about the default: on, depth 8. `prefetch_depth` defaults to 8. The stride
  prefetcher row leaves `docs/TODO.md`; the two keys and the tail go in.
- clang-tidy: `run_ahead` made static. clang-format applied. Both clean.
- Matrix, first try: gcc none 16 of 16; gcc address 15 of 16, `unit.lib_test` failed on the new
  test, `slow_on * 4 < slow_off` with 1,822 against 4,608. Under ASan the launcher's 1,536 write
  faults (which prefetching leaves alone) wait 64 us or more in both runs, and even a prefetched
  page takes that long: a fixed 64 us line depends on the build. Matrix stopped. The test now
  takes the median wait of the two workers' faults (lines not from node 1) and checks it is at
  least two buckets (4x) lower: ASan below 4 us against below 512 us, dev below 1 us against
  below 256 us, twice each. What it checks is unchanged (shorter waits for the same numbers
  read right); how it measures them no longer depends on the build.
- Matrix, second try: gcc none, undefined, thread and clang none pass; gcc address: one
  failure, `unit.pmd_test`, "a job whose region is twice the RAM caps", the job aborted with
  "the home store is full" (the clang builds after it were broken by a debug print of mine
  and are not results). Never seen before in any matrix log.
- The hunt (debug prints since removed): the case passes alone; under heavy load (other
  suites running beside it) it failed 7 of 35 runs with prefetching on and 0 of 24 with
  `PARAMESH_CFG_PREFETCH_DEPTH=0`. Three different failures, all in that case:
  - "the home store is full", on the launcher (its stderr goes to /dev/null in this test;
    routed to a file for the hunt): RAM 1,536 of 1,536, spill slots 2,560 of 2,560, 4,095 pages
    kept. A store bug: put() refused a page with kNoSpace while the last spill slot was being
    written, instead of kState. Prefetching makes the launcher read its spilled pages back
    faster, so new pages meet that write more often. Fixed with its test in its own pull
    request, "M4-3 fix: the store says wait, not full, while its last spill is written",
    logged in M4-3's log: the store is that task's.
  - "job refused ... the 1 node chosen gives 21 MB": admission ran before the daemons had
    listed each other. Before any job process: not prefetching.
  - "terminate called without an active exception": `pm_init` returns after the 10 s start
    timeout with the network thread running, and the program's return ends in
    std::terminate. Before any fault: not prefetching.
  The last two go to docs/TODO.md.
- Next: merge main once the M4-3 fix is in, the loaded runs again, the matrix, the pull request.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/lib/prefetch.h` — the prefetcher
- `src/lib/prefetch.cpp` — streams, strides, how far ahead
- `src/lib/job.cpp` — guesses asked for as read faults; the two keys; `prefetched` in `job_pages`
- `tests/unit/lib_test.cpp` — the prefetcher; the M1 program with and without it
- `tools/bench/evaluate.sh` — the `prefetch` experiment
- `docs/TODO.md` — the two keys for the Tunables table; the stride prefetcher entry
- `docs/logs/X-prefetch.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- Guesses go through the node machine as `kNeedRead`: one path for faults and prefetches, and
  the node machine already lets a fault wait on a page on its way.
- Read faults only train the prefetcher; a write fault asks for nothing ahead.
- The end-to-end test checks the waits, not the fault count: on this machine the reader always
  catches up with its prefetches (session 1).

## Questions and blockers

<!--
Each item starts with [open] or [answered YYYY-MM-DD by <who>], followed by the question and,
once answered, the answer. M0 drafting tasks may also use [for gate] for a Still-open point
drafted for the person to decide at the M0 gate. Write "None" if there are none.
-->
- [answered 2026-10-22 by Amirishetty Sai Vignesh] Prefetching on by default (depth 8), or off
  as an option? Answer: on, depth 8.

## Approvals

<!--
Approvals from the person for anything the rules reserve to them, such as a change to a frozen
file (include/paramesh.h, docs/PROTOCOL.md, docs/STATE_MACHINES.md, docs/INTERNAL_API.md) that
this card does not list. Name the file, who approved, and the date. "None" if there are none.
-->
- The work itself: Amirishetty Sai Vignesh, 2026-10-21 ("lets finish off 7, 3, 1"), started
  2026-10-22. No frozen file changes.
- Prefetching on by default, depth 8: Amirishetty Sai Vignesh in chat, 2026-10-22.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
- The plan's deferred row says "after 3 sequential faults"; any stride is followed, sequential
  being stride 1, as the feature list asks for patterns, not only runs.

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- The two keys in the plan's Tunables table, and the longer 99th-percentile wait
  (docs/TODO.md). The plan's "Stride prefetcher" row leaves docs/TODO.md: this is it.
