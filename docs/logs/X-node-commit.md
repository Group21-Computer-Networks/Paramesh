# X-node-commit: Per-node RAM and spill commitments for admission and caps

- Task: X-node-commit
- Milestone: none
- Stream: -
- Status: done
- Branch: x-node-commit
- Started: 2026-10-20
- Finished: 2026-10-20

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-20 ("lets fix 3rd"), after the
review of the functional requirements: a node gave every job the whole of `--cap-ram` and the
spill budget and its `free_ram`/`free_spill` never fell, so two jobs on one node could use
twice its caps and admission could not see a busy node (FR-1a, FR-8.2, NFR-7; docs/TODO.md,
the Caps (M4-2) and Admission (M4-4) rows). Cores are already shared per node by M5-3.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-20 — session 1

- Read the TODO rows, M4-2 and M4-4 in docs/PLAN.md, and the admission code in
  `src/pmd/pmd.cpp`. PROTOCOL.md already has `free_ram`, `ram_commit` and `NO_CAPACITY`, but
  says nothing of how much a node commits, and a worker's `pmd` is not told N. Asked the
  person twice (below): even share; approve the PROTOCOL.md change.
- `src/pmd/commit.{h,cpp}`: `job_share`, `job_need`, `job_commit`. `pmd` keeps a commitment on
  each `Spawn`, `Worker` and `Run`; `free_now()` is the caps less their sum and goes out in
  `HELLO`. `SPAWN_REQ` is declined with `NO_CAPACITY` when the share does not fit;
  `on_admit_req` commits the launcher's own share or refuses. `SPAWN_OK` and every `L_QUOTA`
  carry the commitment; a launcher gets its `L_QUOTA` right after `L_ADMIT_OK`.
- Wire: `SpawnReqPayload::nodes`, `LQuotaPayload::spill_bytes`; the dissector follows.
- `src/lib/job.cpp`: the spill budget is `L_QUOTA`'s, not `PARAMESH_CFG_SPILL_BUDGET`
  (which only `pmd` reads now); the launcher reads its `L_QUOTA` after `L_ADMIT_OK`.
- Tests: `unit.wire_test` for the two layouts; `unit.pmd_test`, the commitment arithmetic,
  and a daemon that declines a second job that does not fit and takes it once the first has
  gone. The quota test now expects the commitment, not the cap, and the test's fake launcher
  reads the `L_QUOTA` after `L_ADMIT_OK`.
- `unit_pmd_test`: 43 of 45. The cap test (16 MB caps, 12 MB region) and the spill test (8 MB
  caps, 48 MB region) ended with exit 1: placement is rendezvous hashing, so a node can be home
  for more than region / N, past its commitment. Asked the person (below): bounded placement.

### 2026-10-20 — bounded placement

- `src/store/placement.{h,cpp}`: no member is home for more than ceil(segments x weight / all
  weights); a segment in `keep` stays with its home while that home has room; the rest go in
  segment order to the highest-scoring member with room. `send_map` passes the current homes
  as `keep`, so a leave still moves only the leaver's segments. INTERNAL_API.md section 6
  follows, with the person's approval.
- `unit_store_test`: the removal test passes `keep`; new checks that 2,048 segments at
  weights 2:4:2 are exactly 512/1,024/512, 6 over 3 are 2 each, 7 over 3 at most 3 each.
- `unit_pmd_test -tc=...` (the seven cap, spill, quota and admission cases): 7 of 7. Largest
  spill file 10,240 KB, the 10 MB committed; most region one process held 1,136 KB, the cache's
  25% of its 4.4 MB commitment.
- New pmd test: one daemon of 4 MB RAM and 4 MB spill; a slow job of a 4 MB region commits all
  the RAM, a second job is refused ("the region needs 4 MB and the 1 node chosen gives 2 MB"),
  and once the first is stopped the second is admitted. Before this task the node announced
  its whole cap and the second would have been admitted beside the first. A first version
  waited for the second job to finish: the test program's chunks wait up to 10 s each for
  nodes 2 and 3, so a one-node job crawls (`main` does the same); the test now checks that
  `pm_init()` returned (`.up`) and stops it.
- clang-tidy: three findings (two `push_back` in a loop, unchecked optional in the test);
  fixed. `cmake --build --preset tidy`: clean. clang-format: clean.
- `tests/multi/run_m4.sh` (namespaces, no impairment): PASS leave 13 s, join 21 s (node 4 ran
  5,120 indexes), abort 14 s (about 1 s), spill 13 s (1,000 KB spill file).
- `tools/dissector/capture.sh`: every frame decoded, 2 `SPAWN_REQ`, malformed or in error: 0.
- `tools/bench/evaluate.sh outofcore` (1 MB caps, 64 MB spill, matmul 512): 273,024 faults,
  no abort, p50 128 us, p99 512 us. The first run printed 9,785 s, which the wall clock did not
  take (the whole run finished inside the 1,200 s timeout): matmul times itself with
  `TIME_UTC`, so the system clock stepped. The rerun: 16.646 s (29.7 s for the script, timed
  outside), against 18.889 s in M6-5.
- Next: the eight-build matrix, the plan check, the pull request.

### 2026-10-20 — commitments that grow; a joiner's cache

- Matrix, before this session's changes: gcc none and gcc address 16 of 16; stopped at gcc
  undefined, since this session changes the source it builds from.
- Reported three limits of the first version: a commitment did not grow after a leave, a node
  joining mid-run committed a full share, and with caps a job that fewer than N nodes accepted
  was refused. The person: fix all three (below), with the frozen-file changes they need.
- `src/store/home_store.{h,cpp}`: `HomeStore::grow(ram, spill)` raises the RAM cap and the
  spill budget, never lowers them. INTERNAL_API.md section 6 follows; the fake store in
  `tests/unit/internal_api_test.cpp` implements it.
- `src/pmd/commit.{h,cpp}`: `JobCommit` keeps the cache part and a `no_homes` mark;
  `joiner_commit` (a cache of `mem.cache_share` of the need, no spill) and `grow_commit` (RAM
  first, then spill, or false with the commitment as it was).
- `src/pmd/pmd.cpp`: a `MID_RUN` `SPAWN_REQ` gets `joiner_commit`; every `L_QUOTA` carries the
  `NO_HOMES` flag from the commitment; `on_segments` grows the commitment when the node's homes
  pass what it holds and answers `L_QUOTA`, or `L_ABORT` with `NO_CAPACITY` (logged
  `commit_grown` / `commit_short`); `finish_admission` counts the members' free RAM and spill
  beside their commitments when fewer than N accepted.
- `src/lib/job.cpp`: `room_short()` after each map; while room is wanted, `take_segments()`
  takes no `SEG_MIGRATE`, a worker holds its first `SEG_MAP_ACK` and a launcher does not call
  `set_up()` (`up_if_room()`); `grant_room()` on a grown `L_QUOTA` grows the store and goes on.
  A `NO_HOMES` quota makes all of the RAM the cache.
- Wire: `LQuotaPayload::flags`, `kQuotaNoHomes`; the dissector follows.
- PROTOCOL.md: `L_QUOTA.flags`, the joiner and growth paragraphs under `SPAWN_REQ`,
  `L_SEGMENTS`, `L_ABORT` to any job process, 8.2 step 5, the [GATE P8] note.
- Tests: the commitment test checks `joiner_commit` and `grow_commit`; a daemon gives a
  `MID_RUN` request 4,661,248 bytes of RAM and no spill (a member would get 8 MB and 10 MB);
  the store takes the pages it refused once grown; a test-program mode `huge-join`; two runs on
  three daemons of 8 MB RAM and 32 MB spill: node 3 leaves a 48 MB job, nearly all written, and
  node 4 joins; and `pmrun -n 4` on three nodes runs the 48 MB job.
- With growth and the short-start check switched off (a temporary edit, then restored), both
  runs failed as before: "job 65537 aborted: the home store is full", and "refused: the region
  needs 48 MB and the 3 nodes chosen give 37 MB". With them: 7 of 7 (`-tc` the commitment,
  joiner, busy-node, quota and both capped runs).
- `unit_store_test` 9 of 9, `unit_wire_test` and `unit_internal_api_test` pass. clang-format
  and `cmake --build --preset tidy`: clean.
- `tests/multi/run_m4.sh`: PASS leave 15 s, join 21 s, abort 16 s, spill 14 s.
  `tools/dissector/capture.sh`: malformed or in error: 0. `evaluate.sh outofcore`: 16.546 s,
  273,024 faults, no abort.
- Matrix, eight builds of this tree: every one green (below).
- Next: the plan check and the pull request.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->

- `docs/PROTOCOL.md` — `SPAWN_REQ.nodes`, `L_QUOTA.spill_bytes` and `flags`, the commitment
  rule and its growth, `L_SEGMENTS`, `L_ABORT`, 8.1 step 6, 8.2 step 5, [GATE P8]
- `docs/INTERNAL_API.md` — section 6, `store_place_segments` and `HomeStore::grow`
- `docs/TODO.md` — the Caps, Admission, Discovery, Spill and minimal `pmd` rows
- `docs/logs/X-node-commit.md` — this log
- `src/pmd/commit.h` — what a node commits to a job, and how it grows
- `src/pmd/commit.cpp` — the same
- `src/pmd/pmd.h` — a commitment on each spawn, worker and run
- `src/pmd/pmd.cpp` — commitments kept, freed, announced, declined, grown, pushed
- `src/store/placement.h` — bounded placement, `keep`
- `src/store/placement.cpp` — the same
- `src/store/home_store.h` — `HomeStore::grow`
- `src/store/home_store.cpp` — the same
- `src/wire/payloads.h` — `SPAWN_REQ.nodes`, `L_QUOTA.spill_bytes` and `flags`
- `src/wire/wire.cpp` — the same
- `src/lib/job.cpp` — RAM and spill from the commitment; room before segments and start
- `tools/dissector/paramesh.lua` — the new fields
- `tests/unit/wire_test.cpp` — the two layouts
- `tests/unit/pmd_test.cpp` — commitments, joiner, busy node, capped leave and short start
- `tests/unit/store_test.cpp` — bounded placement, `grow`
- `tests/unit/internal_api_test.cpp` — the fake store's `grow`

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- The commitment arithmetic lives in `src/pmd/commit.{h,cpp}`, free functions, so it is
  tested without a daemon.
- Placement weights stay 1 for every member that hosts segments (not the commitment): a node
  with no RAM cap commits 0 and must still be home for its share.
- The launcher's commitment reaches it in an `L_QUOTA` right after `L_ADMIT_OK`, not in
  `L_ADMIT_OK`, which has no room for spill.
- A process `pmd` has no job for (none in practice) still gets the whole caps in its
  `L_QUOTA`, as before.
- `PARAMESH_CFG_SPILL_BUDGET` is read by `pmd` only; job processes take their spill budget
  from `L_QUOTA`.
- A grown commitment adds to the home store only: the cache keeps the size it started with.
- A joiner's cache is `mem.cache_share` of a member's need, the cache of a member whose share
  is all in RAM; a member short of RAM has a smaller one. Its pmd knows no other member's
  commitment, so this is the size it can work out.
- Growth answers the `L_SEGMENTS` of the new map; incoming segments wait in the queue
  `take_segments()` already keeps for segments ahead of their map, so the leave sequence of
  8.2 is unchanged.

## Questions and blockers

<!--
Each item starts with [open] or [answered YYYY-MM-DD by <who>], followed by the question and,
once answered, the answer. M0 drafting tasks may also use [for gate] for a Still-open point
drafted for the person to decide at the M0 gate. Write "None" if there are none.
-->
- [answered 2026-10-20 by Amirishetty Sai Vignesh] How much of its RAM cap and spill budget
  does a node commit to a job: its even share of the region, all that is free (one job per
  node), or a fixed fraction (a new tunable)? Answer: even share.
- [answered 2026-10-20 by Amirishetty Sai Vignesh] A worker's `pmd` is not told N and a job
  process is not told a spill budget: may PROTOCOL.md add `SPAWN_REQ.nodes` and
  `L_QUOTA.spill_bytes` and state the rule? Answer: approve.
- [answered 2026-10-20 by Amirishetty Sai Vignesh] Placement is rendezvous hashing (frozen in
  INTERNAL_API.md section 6), so a node can be home for more than its even share: bound each
  member at its weighted share and keep current homes (an INTERNAL_API.md change), commit
  the whole region instead, or stop? Answer: bounded placement.
- [answered 2026-10-20 by Amirishetty Sai Vignesh] Fix the three limits of the first version
  (growth after a leave, a joiner's full share, a refused short start) with the frozen-file
  changes they need, or only the leave and the short start? Answer: fix all three.

## Approvals

<!--
Approvals from the person for anything the rules reserve to them, such as a change to a frozen
file (include/paramesh.h, docs/PROTOCOL.md, docs/STATE_MACHINES.md, docs/INTERNAL_API.md) that
this card does not list. Name the file, who approved, and the date. "None" if there are none.
-->
- The work itself, outside any card: Amirishetty Sai Vignesh, 2026-10-20 ("lets fix 3rd").
- `docs/PROTOCOL.md`: `SPAWN_REQ` gains `nodes` and 2 reserved bytes (fixed part 56 bytes),
  `L_QUOTA` gains `spill_bytes` (20 bytes), and the commitment rule, by Amirishetty Sai
  Vignesh in chat, 2026-10-20 ("Approve").
- `docs/INTERNAL_API.md` section 6: `store_place_segments` bounded at each member's weighted
  share, with a `keep` argument, by Amirishetty Sai Vignesh in chat, 2026-10-20 ("Bounded
  placement").
- `docs/PROTOCOL.md` (`L_QUOTA.flags` `NO_HOMES`, growth answered with `L_QUOTA` or `L_ABORT`,
  `L_ABORT` to any job process, the joiner's cache, the short-start check) and
  `docs/INTERNAL_API.md` (`HomeStore::grow`), by Amirishetty Sai Vignesh in chat, 2026-10-20
  ("Fix all three").

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
- No card. Three existing tests changed with the approved protocol and placement changes, not
  to pass: the quota test expects the commitment (2,330,624 bytes of RAM for one segment)
  where it expected the whole cap, the old behaviour this task removes; the test's fake
  launcher reads the new `L_QUOTA` after `L_ADMIT_OK`; the removal test passes the current
  homes as `keep`, which is how the contract now keeps the property it checks.
- `--cap-ram`'s help text says the cap is for the jobs together.

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
No card, so no Done-when check of its own. What was run, on this machine (Ubuntu 24.04,
gcc 13, clang 18), on the final tree:

- Matrix, `build/nc-<compiler>-<sanitizer>`, Debug, `ctest -j2`: gcc none 16/16, gcc address
  16/16, gcc undefined 16/16, gcc thread 15/15, clang none 16/16, clang address 16/16, clang
  undefined 16/16, clang thread 15/15 (`app.m0_demo` is not registered under ThreadSanitizer,
  as on `main`).
- The new tests fail without the fix: with growth and the short-start check switched off, "job
  65537 aborted: the home store is full" (capped leave) and "refused: the region needs 48 MB
  and the 3 nodes chosen give 37 MB" (`-n 4` on three nodes). The joiner test expects 4,661,248
  bytes where the first version committed 8 MB and 10 MB of spill; the busy-node test's second
  job was admitted beside the first before this task.
- `cmake --build --preset tidy`: no finding. clang-format `--dry-run --Werror` on every changed
  C++ file: clean.
- `tests/multi/run_m4.sh` (three nodes in network namespaces): PASS leave, join, abort, spill.
- `tools/dissector/capture.sh`: every frame decoded, malformed or in error: 0.
- `tools/bench/evaluate.sh outofcore` (`--cap-ram 1M`, 64 MB spill, `matmul 512`): 16.546 s,
  273,024 faults, p50 128 us, p99 512 us, no abort (18.889 s in M6-5).
- Not run: anything on the cabled laptops (none here), and `run_m4.sh` with RAM caps on leave
  and join (the script sets caps only for spill); the capped leave and join run in
  `unit.pmd_test` instead.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None. The three limits the first version left are fixed in this pull request.
